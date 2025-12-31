#include "log.h"
#include "log_main.h"
#include "callstack.h"
#include <sys/syscall.h>
#include <sys/time.h>
#include <assert.h>
#include <time.h>
#include <atomic>

#ifndef gettid
#define gettid() syscall(__NR_gettid)
#endif

#define MSG_BUF_SIZE    (1024)
#define EXPAND_SIZE     (8)     // for \n \0

namespace eular {
static LogManager *gLogManager = nullptr;
static std::atomic<int32_t> gLevel{LogLevel::LEVEL_DEBUG};
static volatile bool gEnableLogoutColor = true;
static thread_local char g_logBuffer[MSG_BUF_SIZE + EXPAND_SIZE] = {0};

namespace log {
void getLogManager()
{
    // NOTE 多线程情况下获取, 因为获取到的是同一个地址, 故不会导致问题
    // 当主线程退出时, 调用atexit注册的回调, 会释放内存, 故需要接口每次都获取一遍
    gLogManager = LogManager::getInstance();
}

void InitLog(LogLevel::Level lev)
{
    getLogManager();
    gLevel.store(lev, std::memory_order_release);
}

void SetLevel(LogLevel::Level lev)
{
    getLogManager();
    gLevel.store(lev, std::memory_order_release);
}

void SetPath(const char *path)
{
    getLogManager();
    if (gLogManager != nullptr) {
        gLogManager->setPath(path);
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
    bool needFree = false;
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
    int32_t n = vsnprintf(nullptr, 0, fmt, tmpArgs);
    va_end(tmpArgs);

    uint32_t outSize = MSG_BUF_SIZE;
    if (n > MSG_BUF_SIZE) { // 扩充buffer
        outSize = n;
        needFree = true;
        out = (char *)malloc(outSize + EXPAND_SIZE);
        if (out == nullptr) {
            out = g_logBuffer;
            outSize = MSG_BUF_SIZE;
            needFree = false;
        }
    }
    int32_t formatSize = vsnprintf(out, outSize, fmt, ap);
    va_end(ap);
    if (formatSize < 0) {
        perror("vsnprintf error");
        goto need_free;
    }

    len = formatSize;
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

need_free:
    if (needFree) {
        free(out);
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
        std::string msgString = LogFormat::Format(ev);
        std::list<LogWrite*> logWriteList = gLogManager->GetLogWrite();
        for (LogManager::LogWriteIt it = logWriteList.begin(); it != logWriteList.end(); ++it) {
            if (*it != nullptr) {
                (*it)->WriteToFile(msgString);
            }
        }
    }
    CallStack cs;
    cs.update(2, 2);
    cs.log("Stack", LogLevel::LEVEL_ERROR);
    abort();
}

} // namespace eular