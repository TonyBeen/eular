#include "log/log.h"
#include "log_main.h"
#ifdef LOG_ENABLE_CALLSTACK
#include "callstack.h"
#endif
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <atomic>

#ifdef __APPLE__
#include <pthread.h>
#define gettid() static_cast<uint64_t>(pthread_mach_thread_np(pthread_self()))
#else
#include <sys/syscall.h>
#ifndef gettid
#define gettid() static_cast<uint64_t>(syscall(__NR_gettid))
#endif
#endif

#define MSG_BUF_SIZE    (1024)
#define EXPAND_SIZE     (8)     // for \n \0
#define FAST_MSG_BUF_SIZE (4096)

namespace eular {
static LogManager *gLogManager = nullptr;
static std::atomic<int32_t> gLevel{LogLevel::LEVEL_DEBUG};
static volatile bool gEnableLogoutColor = true;
static thread_local char g_logBuffer[FAST_MSG_BUF_SIZE + EXPAND_SIZE] = {0};

namespace log {
void getLogManager()
{
    // NOTE 多线程情况下获取, 因为获取到的是同一个地址, 故不会导致问题
    // 当主线程退出时, 调用atexit注册的回调, 会释放内存, 故需要接口每次都获取一遍
    gLogManager = LogManager::getInstance();
}

void InitLog(int32_t lev)
{
    getLogManager();
    gLevel.store(lev, std::memory_order_release);
}

void SetLevel(int32_t lev)
{
    getLogManager();
    gLevel.store(lev, std::memory_order_release);
}

std::string NormalizeFileStem(const char *fileName)
{
    if (fileName == nullptr || fileName[0] == '\0') {
        return "log";
    }

    std::string stem(fileName);
    static const std::string kSuffix = ".log";
    if (stem.size() >= kSuffix.size() &&
        stem.compare(stem.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        stem.erase(stem.size() - kSuffix.size());
    }
    return stem.empty() ? "log" : stem;
}

void SetPath(const char *path, const char *fileName)
{
    getLogManager();
    if (gLogManager != nullptr) {
        gLogManager->setPath(path == nullptr ? "" : path, NormalizeFileStem(fileName));
    }
}

void SetFileRotation(uint64_t maxFileSize, uint32_t maxFileCount)
{
    getLogManager();
    if (gLogManager != nullptr) {
        gLogManager->setFileRotation(maxFileSize, maxFileCount);
    }
}

void EnableLogColor(bool flag)
{
    getLogManager();
    gEnableLogoutColor = flag;
}

void addOutputNode(int32_t type)
{
    getLogManager();
    if (gLogManager != nullptr) {
        gLogManager->addLogWriteToList(type);
    }
}

void delOutputNode(int32_t type)
{
    getLogManager();
    if (gLogManager != nullptr) {
        gLogManager->delLogWriteFromList(type);
    }
}
} // namespace log

void log_write(int32_t level, const char *tag, const char *fmt, ...)
{
    if (gLevel.load(std::memory_order_acquire) > level) {
        return;
    }

    char *out = g_logBuffer;
    LogEvent ev;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    size_t len = 0;

    ev.enableColor = gEnableLogoutColor;
    ev.level = (LogLevel::Level)level;
    assert(strlen(tag) < LOG_TAG_SIZE);
    strcpy(ev.tag, tag);
    ev.time = tv;
    ev.pid = getpid();
    ev.tid = gettid();

    va_list ap, tmpArgs;
    va_start(ap, fmt);
    va_copy(tmpArgs, ap);
    const int32_t formatSize = vsnprintf(out, FAST_MSG_BUF_SIZE, fmt, tmpArgs);
    va_end(tmpArgs);
    va_end(ap);
    if (formatSize < 0) {
        perror("vsnprintf error");
        return;
    }

    len = static_cast<size_t>(formatSize);
    if (len >= FAST_MSG_BUF_SIZE) {
        len = FAST_MSG_BUF_SIZE - 1;
    }
    if (len && out[len - 1] != '\n') {
        out[len] = '\n';
        ++len;
    }
    out[len] = '\0';

    ev.msg = out;
    log::getLogManager();
    if (gLogManager) {
        gLogManager->WriteLog(&ev);
    }
}

void log_write_assertv(const LogEvent *ev);

void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...)
{
    if (gLevel.load(std::memory_order_acquire) > level) {
        return;
    }

    LogEvent ev;
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    ev.level = (LogLevel::Level)level;
    assert(strlen(tag) < LOG_TAG_SIZE);
    strcpy(ev.tag, tag);
    ev.time = tv;
    ev.pid = getpid();
    ev.tid = gettid();

    size_t index = snprintf(g_logBuffer, MSG_BUF_SIZE - 1, "assertion \"%s\" failed. ", expr);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_logBuffer + index, MSG_BUF_SIZE - index - 1, fmt, ap);
    va_end(ap);

    size_t len = strlen(g_logBuffer);
    if (len > 0 && g_logBuffer[len - 1] != '\n') {
        g_logBuffer[len] = '\n';
    }
    ev.msg = g_logBuffer;
    log_write_assertv(&ev);
}

void log_write_assertv(const LogEvent *ev)
{
    log::getLogManager();
    if (gLogManager != nullptr) {
        gLogManager->WriteLog(ev);
        gLogManager->Flush();
    }
#ifdef LOG_ENABLE_CALLSTACK
    CallStack cs;
    cs.update(2, 2);
    cs.log("Stack", LogLevel::LEVEL_ERROR);
#endif
    abort();
}

} // namespace eular

extern "C" {

void log_set_level(log_level_t lev)
{
    eular::log::SetLevel(static_cast<int32_t>(lev));
}

void log_set_path(const char *path, const char *file_name)
{
    eular::log::SetPath(path, file_name);
}

void log_set_file_rotation(uint64_t max_file_size, uint32_t max_file_count)
{
    eular::log::SetFileRotation(max_file_size, max_file_count);
}

void log_enable_color(int32_t flag)
{
    eular::log::EnableLogColor(flag != 0);
}

void log_add_output_node(output_type_t type)
{
    eular::log::addOutputNode(static_cast<int32_t>(type));
}

void log_del_output_node(output_type_t type)
{
    eular::log::delOutputNode(static_cast<int32_t>(type));
}

void log_write(int32_t level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    char msg[4096] = {0};
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    eular::log_write(level, tag, "%s", msg);
}

void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    char msg[4096] = {0};
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    eular::log_write_assert(level, expr, tag, "%s", msg);
}

}