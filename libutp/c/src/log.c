#include "internal/log.h"

#include <string.h>

static utp_internal_error_t append_fragment(utp_log_tag_t *tag,
                                            const char *fragment,
                                            size_t fragment_length) {
  size_t required;

  if (tag == NULL || fragment == NULL || fragment_length == 0u) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  if (fragment_length > UTP_LOG_TAG_MAX_LENGTH - 2u ||
      tag->tag_length > UTP_LOG_TAG_MAX_LENGTH - fragment_length - 2u) {
    return UTP_INTERNAL_ERROR_LIMIT;
  }
  required = tag->tag_length + fragment_length + 2u;
  tag->tag[tag->tag_length] = '[';
  memcpy(tag->tag + tag->tag_length + 1u, fragment, fragment_length);
  tag->tag[required - 1u] = ']';
  tag->tag[required] = '\0';
  tag->tag_length = required;
  return UTP_INTERNAL_ERROR_OK;
}

void utp_log_tag_clear(utp_log_tag_t *tag) {
  if (tag != NULL) {
    tag->tag[0] = '\0';
    tag->tag_length = 0u;
  }
}

utp_internal_error_t utp_log_tag_init(utp_log_tag_t *tag, const char *fragment,
                                      size_t fragment_length) {
  if (tag == NULL) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  utp_log_tag_clear(tag);
  return append_fragment(tag, fragment, fragment_length);
}

utp_internal_error_t utp_log_tag_append(utp_log_tag_t *tag,
                                        const utp_log_tag_t *parent,
                                        const char *fragment,
                                        size_t fragment_length) {
  if (tag == NULL || parent == NULL ||
      parent->tag_length > UTP_LOG_TAG_MAX_LENGTH) {
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
  }
  memcpy(tag, parent, sizeof(*tag));
  return append_fragment(tag, fragment, fragment_length);
}

void utp_internal_log_error(const utp_logger_t *logger,
                            const utp_log_tag_t *tag,
                            utp_internal_error_t error, const char *message) {
  utp_log_event_t event;

  if (logger == NULL || logger->sink == NULL || message == NULL) {
    return;
  }
  event.level = UTP_LOG_LEVEL_ERROR;
  event.tag = tag == NULL ? NULL : tag->tag;
  event.tag_length = tag == NULL ? 0u : tag->tag_length;
  event.status = utp_internal_error_to_status(error);
  event.system_error = utp_internal_error_to_errno(error);
  event.message = message;
  logger->sink(&event, logger->user_data);
}
