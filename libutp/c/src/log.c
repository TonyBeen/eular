#include "internal/log.h"

#include <stdio.h>
#include <string.h>

static utp_internal_error_t append_fragment(utp_log_tag_t *tag, const char *fragment, size_t fragment_length) {
    size_t required;

    if (tag == NULL || fragment == NULL || fragment_length == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (fragment_length > UTP_LOG_TAG_MAX_LENGTH - 2u ||
        tag->tag_length > UTP_LOG_TAG_MAX_LENGTH - fragment_length - 2u) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    required                  = tag->tag_length + fragment_length + 2u;
    tag->tag[tag->tag_length] = '[';
    memcpy(tag->tag + tag->tag_length + 1u, fragment, fragment_length);
    tag->tag[required - 1u] = ']';
    tag->tag[required]      = '\0';
    tag->tag_length         = required;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_log_tag_clear(utp_log_tag_t *tag) {
    if (tag != NULL) {
        tag->tag[0]     = '\0';
        tag->tag_length = 0u;
    }
}

utp_internal_error_t utp_log_tag_init(utp_log_tag_t *tag, const char *fragment, size_t fragment_length) {
    if (tag == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_log_tag_clear(tag);
    return append_fragment(tag, fragment, fragment_length);
}

utp_internal_error_t utp_log_tag_append(utp_log_tag_t *tag, const utp_log_tag_t *parent, const char *fragment,
                                        size_t fragment_length) {
    if (tag == NULL || parent == NULL || parent->tag_length > UTP_LOG_TAG_MAX_LENGTH) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memcpy(tag, parent, sizeof(*tag));
    return append_fragment(tag, fragment, fragment_length);
}

void utp_internal_log_error(const utp_logger_t *logger, const utp_log_tag_t *tag, utp_internal_error_t error,
                            const char *message) {
    char        formatted[UTP_LOG_MESSAGE_MAX_LENGTH + 1u];
    const char *status;
    const int   system_error = utp_internal_error_to_errno(error);

    if (logger == NULL || logger->sink == NULL || message == NULL) {
        return;
    }
    status = utp_status_string(utp_internal_error_to_status(error));
    if (tag != NULL && tag->tag_length != 0u) {
        if (system_error != 0) {
            (void)snprintf(formatted, sizeof(formatted), "%s %s: status=%s, errno=%d", tag->tag, message, status,
                           system_error);
        } else {
            (void)snprintf(formatted, sizeof(formatted), "%s %s: status=%s", tag->tag, message, status);
        }
    } else if (system_error != 0) {
        (void)snprintf(formatted, sizeof(formatted), "%s: status=%s, errno=%d", message, status, system_error);
    } else {
        (void)snprintf(formatted, sizeof(formatted), "%s: status=%s", message, status);
    }
    logger->sink(UTP_LOG_LEVEL_ERROR, formatted);
}
