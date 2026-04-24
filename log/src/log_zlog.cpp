#include "log.h"
#include "log_format.h"
#ifdef LOG_ENABLE_CALLSTACK
#include "callstack.h"
#endif
#include "3rd_party/zlog/src/zlog.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <string>

#ifdef __APPLE__
#include <pthread.h>
#define gettid() static_cast<uint64_t>(pthread_mach_thread_np(pthread_self()))
#else
#include <sys/syscall.h>
#ifndef gettid
#define gettid() static_cast<uint64_t>(syscall(__NR_gettid))
#endif
#endif

namespace {
constexpr uint32_t kStdoutMask = (1u << static_cast<uint32_t>(eular::OutputType::STDOUT));
constexpr uint32_t kFileMask = (1u << static_cast<uint32_t>(eular::OutputType::FILEOUT));
constexpr int32_t kMsgBufferSize = 4096;

struct ZlogBackendState {
    std::mutex mutex;
    std::atomic<int32_t> level{eular::LogLevel::LEVEL_DEBUG};
    std::atomic<uint32_t> outputMask{kStdoutMask};
    std::atomic<bool> enableColor{true};
    std::string basePath{"./"};
    std::string filePath{"./log.log"};
    zlog_category_t *stdoutCategory{nullptr};
    zlog_category_t *fileCategory{nullptr};
    bool initialized{false};
};

ZlogBackendState &GetState()
{
    static ZlogBackendState state;
    return state;
}

std::string NormalizeBasePath(const std::string &path)
{
    if (path.empty()) {
        return "./";
    }
    if (path.back() == '/') {
        return path;
    }
    return path + "/";
}

std::string EscapeConfString(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\\' || c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

std::string BuildConfigString(const std::string &filePath)
{
    std::string config;
    config.reserve(filePath.size() + 128);
    config += "[formats]\n";
    config += "raw = \"%m\"\n";
    config += "[rules]\n";
    config += "eular_stdout.DEBUG >stdout; raw\n";
    config += "eular_file.DEBUG \"";
    config += EscapeConfString(filePath);
    config += "\"; raw\n";
    return config;
}

void RefreshCategoriesLocked(ZlogBackendState &state)
{
    state.stdoutCategory = zlog_get_category("eular_stdout");
    state.fileCategory = zlog_get_category("eular_file");
}

void EnsureInitializedLocked(ZlogBackendState &state)
{
    if (state.initialized) {
        return;
    }

    const std::string config = BuildConfigString(state.filePath);
    if (zlog_init_from_string(config.c_str()) != 0) {
        fprintf(stderr, "zlog_init_from_string failed\n");
        return;
    }

    state.initialized = true;
    RefreshCategoriesLocked(state);
}

void ReloadLocked(ZlogBackendState &state)
{
    if (!state.initialized) {
        EnsureInitializedLocked(state);
        return;
    }

    const std::string config = BuildConfigString(state.filePath);
    if (zlog_reload_from_string(config.c_str()) != 0) {
        fprintf(stderr, "zlog_reload_from_string failed\n");
        return;
    }
    RefreshCategoriesLocked(state);
}

int32_t ToZlogLevel(int32_t level)
{
    switch (static_cast<eular::LogLevel::Level>(level)) {
    case eular::LogLevel::LEVEL_DEBUG:
        return ZLOG_LEVEL_DEBUG;
    case eular::LogLevel::LEVEL_INFO:
        return ZLOG_LEVEL_INFO;
    case eular::LogLevel::LEVEL_WARN:
        return ZLOG_LEVEL_WARN;
    case eular::LogLevel::LEVEL_ERROR:
        return ZLOG_LEVEL_ERROR;
    case eular::LogLevel::LEVEL_FATAL:
        return ZLOG_LEVEL_FATAL;
    default:
        return ZLOG_LEVEL_DEBUG;
    }
}

void EmitFormatted(const std::string &formatted, int32_t level)
{
    ZlogBackendState &state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    EnsureInitializedLocked(state);
    if (!state.initialized) {
        return;
    }

    const uint32_t mask = state.outputMask.load(std::memory_order_acquire);
    const int32_t zlevel = ToZlogLevel(level);
    if ((mask & kStdoutMask) && state.stdoutCategory != nullptr) {
        zlog(state.stdoutCategory, __FILE__, sizeof(__FILE__) - 1, __func__, sizeof(__func__) - 1,
            __LINE__, zlevel, "%s", formatted.c_str());
    }
    if ((mask & kFileMask) && state.fileCategory != nullptr) {
        zlog(state.fileCategory, __FILE__, sizeof(__FILE__) - 1, __func__, sizeof(__func__) - 1,
            __LINE__, zlevel, "%s", formatted.c_str());
    }
}

void log_write_assertv(const eular::LogEvent *ev)
{
    if (ev == nullptr) {
        abort();
    }

    EmitFormatted(eular::LogFormat::Format(ev, false), static_cast<int32_t>(ev->level));
#ifdef LOG_ENABLE_CALLSTACK
    eular::CallStack cs;
    cs.update(2, 2);
    cs.log("Stack", eular::LogLevel::LEVEL_ERROR);
#endif
    abort();
}

} // namespace

namespace eular {

namespace log {

void InitLog(LogLevel::Level lev)
{
    ZlogBackendState &state = GetState();
    state.level.store(lev, std::memory_order_release);

    std::lock_guard<std::mutex> lock(state.mutex);
    state.basePath = NormalizeBasePath(state.basePath);
    state.filePath = state.basePath + "log.log";
    EnsureInitializedLocked(state);
}

void SetLevel(LogLevel::Level lev)
{
    GetState().level.store(lev, std::memory_order_release);
}

void SetPath(const char *path)
{
    ZlogBackendState &state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);

    if (path == nullptr || path[0] == '\0') {
        state.basePath = "./";
    } else {
        state.basePath = NormalizeBasePath(path);
    }
    state.filePath = state.basePath + "log.log";
    ReloadLocked(state);
}

void EnableLogColor(bool flag)
{
    GetState().enableColor.store(flag, std::memory_order_release);
}

void addOutputNode(int32_t type)
{
    if (type < 0 || type >= static_cast<int32_t>(OutputType::UNKNOW)) {
        return;
    }
    uint32_t mask = (1u << static_cast<uint32_t>(type));
    GetState().outputMask.fetch_or(mask, std::memory_order_acq_rel);
}

void delOutputNode(int32_t type)
{
    if (type < 0 || type >= static_cast<int32_t>(OutputType::UNKNOW)) {
        return;
    }
    uint32_t mask = (1u << static_cast<uint32_t>(type));
    GetState().outputMask.fetch_and(~mask, std::memory_order_acq_rel);
}

} // namespace log

void log_write(int32_t level, const char *tag, const char *fmt, ...)
{
    ZlogBackendState &state = GetState();
    if (state.level.load(std::memory_order_acquire) > level) {
        return;
    }

    LogEvent ev;
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    ev.level = static_cast<LogLevel::Level>(level);
    ev.enableColor = state.enableColor.load(std::memory_order_acquire);
    ev.time = tv;
    ev.pid = getpid();
    ev.tid = gettid();

    const char *safeTag = tag != nullptr ? tag : "UNKNOWN";
    strncpy(ev.tag, safeTag, LOG_TAG_SIZE - 1);
    ev.tag[LOG_TAG_SIZE - 1] = '\0';

    char msgBuffer[kMsgBufferSize] = {0};
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(msgBuffer, sizeof(msgBuffer), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }

    ev.msg = msgBuffer;
    EmitFormatted(LogFormat::Format(&ev, ev.enableColor), level);
}

void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...)
{
    ZlogBackendState &state = GetState();
    if (state.level.load(std::memory_order_acquire) > level) {
        return;
    }

    LogEvent ev;
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    ev.level = static_cast<LogLevel::Level>(level);
    ev.enableColor = false;
    ev.time = tv;
    ev.pid = getpid();
    ev.tid = gettid();

    const char *safeTag = tag != nullptr ? tag : "ASSERT";
    strncpy(ev.tag, safeTag, LOG_TAG_SIZE - 1);
    ev.tag[LOG_TAG_SIZE - 1] = '\0';

    char msgBuffer[kMsgBufferSize] = {0};
    size_t index = snprintf(msgBuffer, sizeof(msgBuffer), "assertion \"%s\" failed. ", expr != nullptr ? expr : "<null>");
    if (index >= sizeof(msgBuffer)) {
        index = sizeof(msgBuffer) - 1;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgBuffer + index, sizeof(msgBuffer) - index, fmt, ap);
    va_end(ap);

    ev.msg = msgBuffer;
    log_write_assertv(&ev);
}

} // namespace eular
