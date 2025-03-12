/*************************************************************************
    > File Name: kcp_log.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年03月10日 星期一 10时52分17秒
 ************************************************************************/

#include "kcp_log.h"

#define LOG_BUFFER_SIZE 1024

kcp_log_callback_t  g_log_callback = NULL;
int32_t             g_log_level = LOG_LEVEL_SILENT;

int32_t kcp_log_format(int32_t level, const char *fmt, ...)
{
    if (level < g_log_level) {
        return 0;
    }

    int32_t len = 0;
    char log_buffer[LOG_BUFFER_SIZE] = {0};
    va_list ap;
    va_start(ap, fmt);
    len = vsnprintf(log_buffer, LOG_BUFFER_SIZE, fmt, ap);
    va_end(ap);

    if (len > 0 && g_log_callback != NULL) {
        g_log_callback(level, log_buffer, len);
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
