#ifndef EULAR_UTP_LOG_H
#define EULAR_UTP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_LOG_TAG_MAX_LENGTH     256u
#define UTP_LOG_MESSAGE_MAX_LENGTH 2048u

typedef enum utp_log_level {
    // 级别按严重程度递增，可直接作为 Context 的最低输出阈值。
    UTP_LOG_LEVEL_DEBUG   = 0,
    UTP_LOG_LEVEL_INFO    = 1,
    UTP_LOG_LEVEL_WARNING = 2,
    UTP_LOG_LEVEL_ERROR   = 3,
} utp_log_level_t;

// 日志回调同步执行，message 仅在回调期间有效。
typedef void (*utp_log_sink_fn)(utp_log_level_t level, const char* message);

typedef struct utp_logger {
    utp_log_sink_fn sink;
} utp_logger_t;

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_LOG_H
