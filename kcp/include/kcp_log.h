/*************************************************************************
    > File Name: kcp_log.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年03月10日 星期一 10时51分23秒
 ************************************************************************/

#ifndef __KCP_LOG_H__
#define __KCP_LOG_H__

#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <kcp_def.h>

#ifdef OS_WINDOWS
#define DIR_SEPARATOR       '\\'
#define DIR_SEPARATOR_STR   "\\"
#else
#define DIR_SEPARATOR       '/'
#define DIR_SEPARATOR_STR   "/"
#endif

#ifndef __FILENAME__
#define __FILENAME__  (strrchr(DIR_SEPARATOR_STR __FILE__, DIR_SEPARATOR) + 1)
#endif

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_SILENT,
} kcp_log_level_t;

#define KCP_LOGD(fmt, ...)  kcp_log_format(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define KCP_LOGI(fmt, ...)  kcp_log_format(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define KCP_LOGW(fmt, ...)  kcp_log_format(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define KCP_LOGE(fmt, ...)  kcp_log_format(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define KCP_LOGF(fmt, ...)  kcp_log_format(LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)

typedef void (*kcp_log_callback_t) (int32_t, const char *, int32_t);

EXTERN_C_BEGIN

// thread safe
KCP_PORT int32_t kcp_log_format(int32_t level, const char* fmt, ...);

// thread unsafe
KCP_PORT void    kcp_log_callback(kcp_log_callback_t cb);

// thread unsafe, default LOG_LEVEL_SILENT
KCP_PORT void    kcp_log_level(int32_t level);

EXTERN_C_END

#endif // __KCP_LOG_H__
