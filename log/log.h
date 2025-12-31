#ifndef _LOG_H_
#define _LOG_H_

#include "log_level.h"
#include <stdarg.h>

#ifndef LOGD
#define LOGD(...) ((void)eular::log_write(eular::LogLevel::Level::LEVEL_DEBUG, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGI
#define LOGI(...) ((void)eular::log_write(eular::LogLevel::Level::LEVEL_INFO, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGW
#define LOGW(...) ((void)eular::log_write(eular::LogLevel::Level::LEVEL_WARN, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGE
#define LOGE(...) ((void)eular::log_write(eular::LogLevel::Level::LEVEL_ERROR, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGF
#define LOGF(...) ((void)eular::log_write(eular::LogLevel::Level::LEVEL_FATAL, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOG_ASSERT
#define LOG_ASSERT(cond, ...) \
    (!(cond) ? ((void)eular::log_write_assert(eular::LogLevel::Level::LEVEL_FATAL, #cond, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif

#ifndef LOG_ASSERT2
#define LOG_ASSERT2(cond)   \
    (!(cond) ? ((void)eular::log_write_assert(eular::LogLevel::Level::LEVEL_FATAL, #cond, LOG_TAG, "")) : (void)0)
#endif

namespace eular {

namespace log {
/**
 * @brief 修改输出日志级别
 * @param lev 最小输出级别
 */
void InitLog(LogLevel::Level lev = LogLevel::LEVEL_DEBUG);
/**
 * @param lev 设置最小输出级别
 */
void SetLevel(LogLevel::Level lev);

/**
 * @brief 设置日志输出路径。建议使用全局路径, 使用相对路径时取决于bash执行时的路径, 相对不稳定
 */
void SetPath(const char *path);

/**
 * @brief 使输出在stdout的日志携带颜色
 * 
 * @param flag 
 */
void EnableLogColor(bool flag);

/**
 * @param type 输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void addOutputNode(int32_t type);
/**
 * @param type 输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void delOutputNode(int32_t type);
}

void log_write(int32_t level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void log_write_assert(int32_t level, const char *expr, const char *tag, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

}

#endif