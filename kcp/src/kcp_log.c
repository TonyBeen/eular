/*************************************************************************
    > File Name: kcp_log.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年03月10日 星期一 10时52分17秒
 ************************************************************************/

#include "kcp_log.h"

#include <string.h>
#include <stdio.h>

#define LOG_BUFFER_SIZE 1024

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

#define COLOR_MAP(XXX)                        \
    XXX(LOG_LEVEL_DEBUG,        CLR_CLR)      \
    XXX(LOG_LEVEL_INFO,         CLR_SKYBLUE)  \
    XXX(LOG_LEVEL_WARN,         CLR_GREEN)    \
    XXX(LOG_LEVEL_ERROR,        CLR_YELLOW)   \
    XXX(LOG_LEVEL_FATAL,        CLR_RED)      \
    XXX(LOG_LEVEL_SILENT,       CLR_PURPLE)   \

bool                g_log_enable_color  = true;

void default_log_cb(int32_t level, const char *log, int32_t size)
{
    const char *str_level = "UNKNOW";
    const char *color = CLR_CLR;

    switch (level) {
    case LOG_LEVEL_DEBUG:
        str_level = "DEBUG";
        break;
    case LOG_LEVEL_INFO:
        str_level = "INFO";
        break;
    case LOG_LEVEL_WARN:
        str_level = "WARN";
        break;
    case LOG_LEVEL_ERROR:
        str_level = "ERROR";
        break;
    case LOG_LEVEL_FATAL:
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

    if (g_log_enable_color) {
        printf("%s[%s]: %.*s\n" CLR_CLR, color, str_level, size, log);
    } else {
        printf("[%s]: %.*s\n", str_level, size, log);
    }
}

kcp_log_callback_t  g_log_callback      = default_log_cb;
int32_t             g_log_level         = LOG_LEVEL_SILENT;

int32_t kcp_log_format(int32_t level, const char *file_name, const char *func_name, int32_t line, const char* fmt, ...)
{
    if (level < g_log_level) {
        return 0;
    }

    int32_t len = 0;
    int32_t offset = 0;
    char log_buffer[LOG_BUFFER_SIZE] = {0};

    offset = sprintf(log_buffer, "[%s:%d:%s] -> ", file_name, line, func_name);

    va_list ap;
    va_start(ap, fmt);
    len = vsnprintf(log_buffer + offset, LOG_BUFFER_SIZE - offset, fmt, ap);
    va_end(ap);

    if (len > 0 && g_log_callback != NULL) {
        g_log_callback(level, log_buffer, offset + len);
    }

    return len;
}

void kcp_log_callback(kcp_log_callback_t cb)
{
    g_log_callback = cb;
}

void kcp_log_level(int32_t level)
{
    g_log_level = level;
}

void kcp_log_enable_color(bool enable)
{
    g_log_enable_color = enable;
}