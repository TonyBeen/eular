#ifndef _LOG_H_
#define _LOG_H_

#include "log_main.h"
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#ifndef LOGD
#define LOGD(...) ((void)eular::log_write(eular::LogLevel::Level::DEBUG, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGI
#define LOGI(...) ((void)eular::log_write(eular::LogLevel::Level::INFO, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGW
#define LOGW(...) ((void)eular::log_write(eular::LogLevel::Level::WARN, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGE
#define LOGE(...) ((void)eular::log_write(eular::LogLevel::Level::ERROR, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGF
#define LOGF(...) ((void)eular::log_write(eular::LogLevel::Level::FATAL, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOG_ASSERT
#define LOG_ASSERT(cond, ...) \
    (!(cond) ? ((void)eular::log_write_assert(eular::LogLevel::Level::FATAL, #cond, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif

#ifndef LOG_ASSERT2
#define LOG_ASSERT2(cond)   \
    (!(cond) ? ((void)eular::log_write_assert(eular::LogLevel::Level::FATAL, #cond, LOG_TAG, "")) : (void)0)
#endif

namespace eular {

/**
 * @brief 修改输出日志级别
 * @param lev   最小输出级别
 */
void InitLog(LogLevel::Level lev = LogLevel::DEBUG);
/**
 * @param lev   设置最小输出级别
 */
void SetLevel(LogLevel::Level lev);

/**
 * @brief 使能stdout上色
 * 
 * @param flag 
 */
void EnableLogColor(bool flag);

/**
 * @param type  输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void addOutputNode(int type);
/**
 * @param type  输出节点类型；STDOUT，FILEOUT，CONSOLEOUT.
 */
void delOutputNode(int type);

void log_write(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void log_write_assert(int level, const char *expr, const char *tag, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void log_writev(const LogEvent *ev);
static void log_write_assertv(const LogEvent *ev);

}

#endif