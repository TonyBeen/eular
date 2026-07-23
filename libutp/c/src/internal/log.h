#ifndef EULAR_UTP_INTERNAL_LOG_H
#define EULAR_UTP_INTERNAL_LOG_H

#include <stddef.h>
#include <utp/log.h>

#include "internal/error.h"

typedef struct utp_log_tag {
  char tag[UTP_LOG_TAG_MAX_LENGTH + 1u];
  size_t tag_length;
} utp_log_tag_t;

void utp_log_tag_clear(utp_log_tag_t *tag);
utp_internal_error_t utp_log_tag_init(utp_log_tag_t *tag, const char *fragment,
                                      size_t fragment_length);
utp_internal_error_t utp_log_tag_append(utp_log_tag_t *tag,
                                        const utp_log_tag_t *parent,
                                        const char *fragment,
                                        size_t fragment_length);
void utp_internal_log_error(const utp_logger_t *logger,
                            const utp_log_tag_t *tag,
                            utp_internal_error_t error, const char *message);

#endif /* EULAR_UTP_INTERNAL_LOG_H */
