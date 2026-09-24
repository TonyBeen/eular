#ifndef EULAR_UTP_LOG_H
#define EULAR_UTP_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_LOG_TAG_MAX_LENGTH     256u
#define UTP_LOG_MESSAGE_MAX_LENGTH 2048u

typedef enum utp_log_level {
    // 级别按严重程度递增；SILENCE 表示不输出任何日志。
    UTP_LOG_LEVEL_DEBUG   = 0,
    UTP_LOG_LEVEL_INFO    = 1,
    UTP_LOG_LEVEL_WARNING = 2,
    UTP_LOG_LEVEL_ERROR   = 3,
    UTP_LOG_LEVEL_SILENCE = 4,
} utp_log_level_t;

// 日志回调同步执行，message 仅在回调期间有效。
typedef void (*utp_log_sink_fn)(utp_log_level_t level, const char* message);

typedef struct utp_logger {
    utp_log_sink_fn sink;   // 同步日志输出回调，为空时关闭日志
    utp_log_level_t level;  // 最低输出级别
} utp_logger_t;

#define UTP_LOGGER_INIT {NULL, UTP_LOG_LEVEL_INFO}

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_LOG_H
