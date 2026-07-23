#ifndef EULAR_UTP_LOG_H
#define EULAR_UTP_LOG_H

#include <stddef.h>
#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_LOG_TAG_MAX_LENGTH 256u

typedef enum utp_log_level {
  UTP_LOG_LEVEL_ERROR = 0,
  UTP_LOG_LEVEL_WARNING = 1,
  UTP_LOG_LEVEL_INFO = 2,
  UTP_LOG_LEVEL_DEBUG = 3
} utp_log_level_t;

typedef struct utp_log_event {
  utp_log_level_t level;
  const char *tag;
  size_t tag_length;
  utp_status_t status;
  int system_error;
  const char *message;
} utp_log_event_t;

/* The callback is synchronous and must not retain tag or message pointers. */
typedef void (*utp_log_sink_fn)(const utp_log_event_t *event, void *user_data);

typedef struct utp_logger {
  utp_log_sink_fn sink;
  void *user_data;
} utp_logger_t;

#ifdef __cplusplus
}
#endif

#endif /* EULAR_UTP_LOG_H */
