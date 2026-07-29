#ifndef EULAR_UTP_LOG_H
#define EULAR_UTP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_LOG_TAG_MAX_LENGTH     256u
#define UTP_LOG_MESSAGE_MAX_LENGTH 2048u

typedef enum utp_log_level {
    UTP_LOG_LEVEL_DEBUG   = 0,
    UTP_LOG_LEVEL_INFO    = 1,
    UTP_LOG_LEVEL_WARNING = 2,
    UTP_LOG_LEVEL_ERROR   = 3,
} utp_log_level_t;

// The callback is synchronous. The formatted message is valid only during the callback.
typedef void (*utp_log_sink_fn)(utp_log_level_t level, const char* message);

typedef struct utp_logger {
    utp_log_sink_fn sink;
} utp_logger_t;

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_LOG_H
