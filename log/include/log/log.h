#ifndef _LOG_H_
#define _LOG_H_

#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <log/level.h>

#ifdef _MSC_VER
    #include <sal.h>
    #define PRINTF_FMT _Printf_format_string_
    #define FORMAT_ATTR(...)
#else
    #define PRINTF_FMT
    #define FORMAT_ATTR(...) __attribute__((format(__VA_ARGS__)))
#endif


#ifndef LOGD
#define LOGD(...) ((void)eular::log_write(LEVEL_DEBUG, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGI
#define LOGI(...) ((void)eular::log_write(LEVEL_INFO, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGW
#define LOGW(...) ((void)eular::log_write(LEVEL_WARN, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGE
#define LOGE(...) ((void)eular::log_write(LEVEL_ERROR, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGF
#define LOGF(...) ((void)eular::log_write(LEVEL_FATAL, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOG_ASSERT
#define LOG_ASSERT(cond, ...) \
    (!(cond) ? ((void)eular::log_write_assert(LEVEL_FATAL, #cond, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif

#ifndef LOG_ASSERT2
#define LOG_ASSERT2(cond)   \
    (!(cond) ? ((void)eular::log_write_assert(LEVEL_FATAL, #cond, LOG_TAG, "")) : (void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @param lev 设置最小输出级别
 */
void log_set_level(log_level_t lev);

/**
 * @brief 设置日志输出路径。建议使用全局路径, 使用相对路径时取决于bash执行时的路径, 相对不稳定
 */
void log_set_path(const char *path);

/**
 * @brief 使输出在stdout的日志携带颜色
 * 
 * @param flag 
 */
void log_enable_color(int32_t flag);

/**
 * @param type 输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void log_add_output_node(output_type_t type);

/**
 * @param type 输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void log_del_output_node(output_type_t type);

void log_write(int32_t level, const char *tag, const char *fmt, ...) FORMAT_ATTR(printf, 3, 4);

void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...) FORMAT_ATTR(printf, 4, 5);

#ifdef __cplusplus
}

namespace eular {

class LogLevel {
public:
    enum Level {
        UNKNOW = LEVEL_UNKNOWN,
        LEVEL_DEBUG = ::LEVEL_DEBUG,
        LEVEL_INFO = ::LEVEL_INFO,
        LEVEL_WARN = ::LEVEL_WARN,
        LEVEL_ERROR = ::LEVEL_ERROR,
        LEVEL_FATAL = ::LEVEL_FATAL,
    };

    static const char *ToString(int level)
    {
        switch (level) {
        case LEVEL_DEBUG:
            return "DEBUG";
        case LEVEL_INFO:
            return "INFO";
        case LEVEL_WARN:
            return "WARN";
        case LEVEL_ERROR:
            return "ERROR";
        case LEVEL_FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
        }
    }

    static const char *ToFormatString(int level)
    {
        switch (level) {
        case LEVEL_DEBUG:
            return "[D]";
        case LEVEL_INFO:
            return "[I]";
        case LEVEL_WARN:
            return "[W]";
        case LEVEL_ERROR:
            return "[E]";
        case LEVEL_FATAL:
            return "[F]";
        default:
            return "[?]";
        }
    }

    static log_level_t String2Level(const char *lev)
    {
        if (lev == nullptr) {
            return LEVEL_UNKNOWN;
        }
        if (strcasecmp(lev, "debug") == 0) {
            return ::LEVEL_DEBUG;
        }
        if (strcasecmp(lev, "info") == 0) {
            return ::LEVEL_INFO;
        }
        if (strcasecmp(lev, "warn") == 0) {
            return ::LEVEL_WARN;
        }
        if (strcasecmp(lev, "error") == 0) {
            return ::LEVEL_ERROR;
        }
        if (strcasecmp(lev, "fatal") == 0) {
            return ::LEVEL_FATAL;
        }
        return ::LEVEL_UNKNOWN;
    }
};

class LogWrite {
public:
    enum : int32_t {
        STDOUT = ::STDOUT,
        FILEOUT = ::FILEOUT,
        CONSOLEOUT = 2,
        UNKNOW = ::UNKNOW,
    };
};

void log_write(int32_t level, const char *tag, const char *fmt, ...) FORMAT_ATTR(printf, 3, 4);
void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...) FORMAT_ATTR(printf, 4, 5);

namespace log {
void InitLog(int32_t lev = LEVEL_DEBUG);
void SetLevel(int32_t lev);
void SetPath(const char *path);
void EnableLogColor(bool flag);
void addOutputNode(int32_t type);
void delOutputNode(int32_t type);

} // namespace log
} // namespace eular
#endif

#endif