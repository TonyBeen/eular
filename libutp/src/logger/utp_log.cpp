/*************************************************************************
    > File Name: utp_log.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:47:50 PM CST
 ************************************************************************/

#include "logger/utp_log.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#define CLR_CLR         "\033[0m"       // 恢复颜色
#define CLR_BLACK       "\033[30m"      // 黑色字
#define CLR_RED         "\033[31m"      // 红色字
#define CLR_GREEN       "\033[32m"      // 绿色字
#define CLR_YELLOW      "\033[33m"      // 黄色字
#define CLR_BLUE        "\033[34m"      // 蓝色字
#define CLR_PURPLE      "\033[35m"      // 紫色字
#define CLR_SKYBLUE     "\033[36m"      // 天蓝字
#define CLR_WHITE       "\033[37m"      // 白色字

#define CLR_BLK_WHT     "\033[40;37m"   // 黑底白字
#define CLR_RED_WHT     "\033[41;37m"   // 红底白字
#define CLR_GREEN_WHT   "\033[42;37m"   // 绿底白字
#define CLR_YELLOW_WHT  "\033[43;37m"   // 黄底白字
#define CLR_BLUE_WHT    "\033[44;37m"   // 蓝底白字
#define CLR_PURPLE_WHT  "\033[45;37m"   // 紫底白字
#define CLR_SKYBLUE_WHT "\033[46;37m"   // 天蓝底白字
#define CLR_WHT_BLK     "\033[47;30m"   // 白底黑字

#define CLR_MAX_SIZE    (12)

#define COLOR_MAP(XXX)                  \
    XXX(UTP_LOG_DEBUG,  CLR_SKYBLUE)    \
    XXX(UTP_LOG_INFO,   CLR_GREEN)      \
    XXX(UTP_LOG_WARN,   CLR_YELLOW)     \
    XXX(UTP_LOG_ERROR,  CLR_RED)        \
    XXX(UTP_LOG_FATAL,  CLR_PURPLE)     \

#ifdef UTP_COLOR_CONSOLE
bool    g_log_enable_color  = true;
#else
bool    g_log_enable_color  = false;
#endif

void default_log_cb(int32_t level, const char *log, int32_t size)
{
    const char *str_level = "UNKNOW";
    const char *color = CLR_CLR;

    switch (level) {
    case UTP_LOG_DEBUG:
        str_level = "DEBUG";
        break;
    case UTP_LOG_INFO:
        str_level = "INFO";
        break;
    case UTP_LOG_WARN:
        str_level = "WARN";
        break;
    case UTP_LOG_ERROR:
        str_level = "ERROR";
        break;
    case UTP_LOG_FATAL:
        str_level = "FATAL";
        break;
    default:
        break;
    }

#define XXX(level, clr) \
    case level:         \
        color = clr;    \
        break;          \

    switch (level) {
        COLOR_MAP(XXX)

        default:
            break;
    }
#undef XXX
    if (log[size - 1] == '\n') {
        size--;
    }

    if (g_log_enable_color) {
        printf("%s[%s]: %.*s" CLR_CLR "\n", color, str_level, size, log);
    } else {
        printf("[%s]: %.*s\n", str_level, size, log);
    }
}

void utp_set_log_cb(utp_log_callback_t log_cb)
{
    g_log_cb = log_cb;
}

UTP_API void utp_set_log_level(utp_log_level_t level)
{
    g_log_level = level;
}

namespace eular {
namespace utp {

void UtpLog(int32_t level, const char *fileName, const char *funcName, int32_t line, const char* fmt, ...)
{
    if (level < g_log_level || g_log_cb == nullptr) {
        return;
    }

    const int savedErrno = errno;

    char logBuffer[LOG_BUFFER_SIZE];
    int32_t offset = 0;

    offset = snprintf(logBuffer, LOG_BUFFER_SIZE, "[%s:%d:%s()] -> ", fileName, line, funcName);

    va_list ap;
    va_start(ap, fmt);
    offset += vsnprintf(logBuffer + offset, LOG_BUFFER_SIZE - offset, fmt, ap);
    va_end(ap);

    if (offset > 0) {
        if (offset >= LOG_BUFFER_SIZE) {
            offset = LOG_BUFFER_SIZE - 1;
        }
        logBuffer[LOG_BUFFER_SIZE - 1] = '\0';
        g_log_cb(level, logBuffer, offset);
    }

    errno = savedErrno;
}

} // namespace utp
} // namespace eular