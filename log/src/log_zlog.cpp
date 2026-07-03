#include "log/log.h"
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
#define gettid() static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()))
#else
#include <sys/syscall.h>
#ifndef gettid
#define gettid() static_cast<uint32_t>(syscall(__NR_gettid))
#endif
#endif

namespace {
constexpr uint32_t kStdoutMask = (1u << static_cast<uint32_t>(STDOUT));
constexpr uint32_t kFileMask = (1u << static_cast<uint32_t>(FILEOUT));
constexpr int32_t kMsgBufferSize = 4096;

struct ZlogBackendState {
    std::mutex mutex;
    std::atomic<int32_t> level{LEVEL_DEBUG};
    std::atomic<uint32_t> outputMask{kStdoutMask};
    std::atomic<bool> enableColor{true};
    std::string basePath{"./"};
    std::string fileStem{"log"};
    std::string filePath{"./log.log"};
    uint64_t maxFileSize{0};
    uint32_t maxFileCount{0};
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
    if (stem.empty()) {
        return "log";
    }
    return stem;
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

std::string BuildArchivePath(const std::string &filePath)
{
    static const std::string kSuffix = ".log";
    if (filePath.size() >= kSuffix.size() &&
        filePath.compare(filePath.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        return filePath.substr(0, filePath.size() - kSuffix.size()) + "-#r.log";
    }
    return filePath + "-#r";
}

std::string BuildConfigString(const ZlogBackendState &state)
{
    std::string config;
    config.reserve(state.filePath.size() * 2 + 192);
    config += "[formats]\n";
    config += "raw = \"%m\"\n";
    config += "[rules]\n";
    config += "eular_stdout.DEBUG >stdout; raw\n";
    config += "eular_file.DEBUG \"";
    config += EscapeConfString(state.filePath);
    if (state.maxFileSize > 0) {
        config += "\", ";
        config += std::to_string(state.maxFileSize);
        config += " * ";
        config += std::to_string(state.maxFileCount);
        config += " ~ \"";
        config += EscapeConfString(BuildArchivePath(state.filePath));
    }
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

    const std::string config = BuildConfigString(state);
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

    const std::string config = BuildConfigString(state);
    if (zlog_reload_from_string(config.c_str()) != 0) {
        fprintf(stderr, "zlog_reload_from_string failed\n");
        return;
    }
    RefreshCategoriesLocked(state);
}

int32_t ToZlogLevel(int32_t level)
{
    switch (level) {
    case LEVEL_DEBUG:
        return ZLOG_LEVEL_DEBUG;
    case LEVEL_INFO:
        return ZLOG_LEVEL_INFO;
    case LEVEL_WARN:
        return ZLOG_LEVEL_WARN;
    case LEVEL_ERROR:
        return ZLOG_LEVEL_ERROR;
    case LEVEL_FATAL:
        return ZLOG_LEVEL_FATAL;
    default:
        return ZLOG_LEVEL_DEBUG;
    }
}

void EmitEvent(const eular::LogEvent &ev, int32_t level)
{
    ZlogBackendState &state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    EnsureInitializedLocked(state);
    if (!state.initialized) {
        return;
    }

    const uint32_t mask = state.outputMask.load(std::memory_order_acquire);
    const bool writeStdout = ((mask & kStdoutMask) != 0u) && state.stdoutCategory != nullptr;
    const bool writeFile = ((mask & kFileMask) != 0u) && state.fileCategory != nullptr;
    if (!writeStdout && !writeFile) {
        return;
    }

    const int32_t zlevel = ToZlogLevel(level);
    std::string plain;
    if (writeFile || !ev.enableColor) {
        plain = eular::LogFormat::Format(&ev, false);
    }

    if (writeStdout) {
        if (ev.enableColor) {
            const std::string console = eular::LogFormat::Format(&ev, true);
            zlog(state.stdoutCategory, __FILE__, sizeof(__FILE__) - 1, __func__, sizeof(__func__) - 1,
                __LINE__, zlevel, "%s", console.c_str());
        } else {
            zlog(state.stdoutCategory, __FILE__, sizeof(__FILE__) - 1, __func__, sizeof(__func__) - 1,
                __LINE__, zlevel, "%s", plain.c_str());
        }
    }

    if (writeFile) {
        zlog(state.fileCategory, __FILE__, sizeof(__FILE__) - 1, __func__, sizeof(__func__) - 1,
            __LINE__, zlevel, "%s", plain.c_str());
    }
}

void log_write_assertv(const eular::LogEvent *ev)
{
    if (ev == nullptr) {
        abort();
    }

    EmitEvent(*ev, static_cast<int32_t>(ev->level));
#ifdef LOG_ENABLE_CALLSTACK
    eular::CallStack cs;
    cs.update(2, 2);
    cs.log("Stack", LEVEL_ERROR);
#endif
    abort();
}

} // namespace

extern "C" {

void log_set_level(log_level_t lev)
{
    GetState().level.store(static_cast<int32_t>(lev), std::memory_order_release);
}

void log_set_path(const char *path, const char *file_name)
{
    ZlogBackendState &state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);

    if (path == nullptr || path[0] == '\0') {
        state.basePath = "./";
    } else {
        state.basePath = NormalizeBasePath(path);
    }
    state.fileStem = NormalizeFileStem(file_name);
    state.filePath = state.basePath + state.fileStem + ".log";
    ReloadLocked(state);
}

void log_set_file_rotation(uint64_t max_file_size, uint32_t max_file_count)
{
    ZlogBackendState &state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.maxFileSize = max_file_size;
    state.maxFileCount = max_file_count;
    ReloadLocked(state);
}

void log_enable_color(int32_t flag)
{
    GetState().enableColor.store(flag != 0, std::memory_order_release);
}

void log_add_output_node(output_type_t type)
{
    if (type < 0 || type >= UNKNOW) {
        return;
    }
    uint32_t mask = (1u << static_cast<uint32_t>(type));
    GetState().outputMask.fetch_or(mask, std::memory_order_acq_rel);
}

void log_del_output_node(output_type_t type)
{
    if (type < 0 || type >= UNKNOW) {
        return;
    }
    uint32_t mask = (1u << static_cast<uint32_t>(type));
    GetState().outputMask.fetch_and(~mask, std::memory_order_acq_rel);
}

void log_write(int32_t level, const char *tag, const char *fmt, ...)
{
    ZlogBackendState &state = GetState();
    if (state.level.load(std::memory_order_acquire) > level) {
        return;
    }

    eular::LogEvent ev;
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    ev.level = static_cast<log_level_t>(level);
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
    EmitEvent(ev, level);
}

void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...)
{
    ZlogBackendState &state = GetState();
    if (state.level.load(std::memory_order_acquire) > level) {
        return;
    }

    eular::LogEvent ev;
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    ev.level = static_cast<log_level_t>(level);
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

}
