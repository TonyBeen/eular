#include "log_main.h"
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t gOnceFlag = PTHREAD_ONCE_INIT;
static eular::LogManager*   gLogManager = nullptr;

namespace {

static const uint32_t kSinkStdout = 1u << 0;
static const uint32_t kSinkFile = 1u << 1;
static const uint32_t kDefaultSinks = kSinkStdout;
static const uint64_t kMaxFileSize = 5ULL * 1024ULL * 1024ULL;

static bool EnsureDir(const std::string &path)
{
    if (path.empty()) {
        return false;
    }
    if (access(path.c_str(), F_OK) == 0) {
        return true;
    }

    char *mutable_path = strdup(path.c_str());
    if (!mutable_path) {
        return false;
    }

    bool ok = true;
    for (char *p = mutable_path + 1; *p; ++p) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (access(mutable_path, F_OK) != 0 && mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
            ok = false;
            break;
        }
        *p = '/';
    }
    if (ok && access(mutable_path, F_OK) != 0 && mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
        ok = false;
    }

    free(mutable_path);
    return ok;
}

} // namespace

namespace eular {
LogManager::LogManager()
    : mRunning(true),
      mOutputMask(kDefaultSinks),
      mDropped(0),
      mFileFd(-1),
      mFileSize(0)
{
    mWorker = std::thread(&LogManager::workerLoop, this);
    ::atexit(deleteInstance);
}

LogManager::~LogManager()
{
    mRunning.store(false, std::memory_order_release);
    mQueueCv.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
    if (mFileFd >= 0) {
        close(mFileFd);
        mFileFd = -1;
    }
}

void LogManager::setPath(const std::string &path)
{
    std::lock_guard<std::mutex> lock(mPathMutex);
    mBasePath = path;
}

void LogManager::WriteLog(const LogEvent *event)
{
    if (!event || event->msg == nullptr) {
        return;
    }

    QueuedLog item;
    item.plain = LogFormat::Format(event, false);
    item.console = event->enableColor ? LogFormat::Format(event, true) : item.plain;

    std::unique_lock<std::mutex> lock(mQueueMutex);
    if (mQueue.size() >= MAX_QUEUE_SIZE) {
        mQueue.pop();
        mDropped.fetch_add(1, std::memory_order_relaxed);
    }
    mQueue.push(std::move(item));
    lock.unlock();
    mQueueCv.notify_one();
}

void LogManager::Flush()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        if (mQueue.empty()) {
            break;
        }
        lock.unlock();
        usleep(1000);
    }
}

void LogManager::addLogWriteToList(int type)
{
    switch (static_cast<OutputType>(type)) {
    case OutputType::STDOUT:
    case OutputType::CONSOLEOUT:
        mOutputMask.fetch_or(kSinkStdout, std::memory_order_relaxed);
        break;
    case OutputType::FILEOUT:
        mOutputMask.fetch_or(kSinkFile, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

void LogManager::delLogWriteFromList(int type)
{
    switch (static_cast<OutputType>(type)) {
    case OutputType::STDOUT:
    case OutputType::CONSOLEOUT:
        mOutputMask.fetch_and(~kSinkStdout, std::memory_order_relaxed);
        break;
    case OutputType::FILEOUT:
        mOutputMask.fetch_and(~kSinkFile, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

void LogManager::workerLoop()
{
    while (mRunning.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        mQueueCv.wait(lock, [this]() {
            return !mQueue.empty() || !mRunning.load(std::memory_order_acquire);
        });

        std::queue<QueuedLog> local;
        local.swap(mQueue);
        lock.unlock();

        const uint32_t sinks = mOutputMask.load(std::memory_order_relaxed);
        while (!local.empty()) {
            const QueuedLog &item = local.front();
            if (sinks & kSinkStdout) {
                const ssize_t n = ::write(STDOUT_FILENO, item.console.data(), item.console.size());
                (void)n;
            }
            if (sinks & kSinkFile) {
                rotateFileIfNeeded(item.plain.size());
                if (ensureFileOpened()) {
                    ssize_t n = ::write(mFileFd, item.plain.data(), item.plain.size());
                    if (n > 0) {
                        mFileSize += static_cast<uint64_t>(n);
                    }
                }
            }
            local.pop();
        }
    }
}

std::string LogManager::resolveBasePath() const
{
    std::lock_guard<std::mutex> lock(mPathMutex);
    std::string path = mBasePath.empty() ? "~/log" : mBasePath;
    if (!path.empty() && path[path.size() - 1] == '/') {
        path.pop_back();
    }
    if (!path.empty() && path[0] == '~') {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir) {
            path = std::string(pw->pw_dir) + path.substr(1);
        }
    }
    return path;
}

std::string LogManager::buildLogFileName() const
{
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char name[64] = {0};
    snprintf(name, sizeof(name), "log-%04d%02d%02d-%02d%02d%02d.log",
             tmv.tm_year + 1900,
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
    return std::string(name);
}

bool LogManager::ensureFileOpened()
{
    if (mFileFd >= 0) {
        return true;
    }

    const std::string base = resolveBasePath();
    if (!EnsureDir(base)) {
        return false;
    }

    const std::string full = base + "/" + buildLogFileName();
    mFileFd = ::open(full.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0664);
    if (mFileFd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(mFileFd, &st) == 0) {
        mFileSize = static_cast<uint64_t>(st.st_size);
    } else {
        mFileSize = 0;
    }
    return true;
}

void LogManager::rotateFileIfNeeded(size_t incoming)
{
    if (mFileFd < 0) {
        return;
    }

    if (mFileSize + incoming <= kMaxFileSize) {
        return;
    }

    close(mFileFd);
    mFileFd = -1;
    mFileSize = 0;
    (void)ensureFileOpened();
}

void LogManager::once_entry()
{
    gLogManager = new (std::nothrow) LogManager();
}

LogManager *LogManager::getInstance()
{
    pthread_once(&gOnceFlag, once_entry);
    return gLogManager;
}

void LogManager::deleteInstance()
{
    if (gLogManager != nullptr) {
        delete gLogManager;
        gLogManager = nullptr;
    }
}

}