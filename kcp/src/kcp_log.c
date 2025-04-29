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

void default_log_cb(int32_t level, const char *log, int32_t size)
{
    const char *str_level = "UNKNOW";
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

    printf("[%s]: %.*s\n", str_level, size, log);
}

kcp_log_callback_t  g_log_callback = default_log_cb;
int32_t             g_log_level = LOG_LEVEL_SILENT;

int32_t kcp_log_format(int32_t level, const char *file_name, const char *func_name, int32_t line, const char* fmt, ...)
{
    if (level < g_log_level) {
        return 0;
    }

    int32_t len = 0;
    int32_t offset = 0;
    char log_buffer[LOG_BUFFER_SIZE] = {0};

    offset = sprintf(log_buffer, "[%s:%s:%d] ->", file_name, func_name, line);

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
