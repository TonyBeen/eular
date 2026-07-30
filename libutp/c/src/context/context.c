#include "context/context.h"

#include <stdio.h>
#include <string.h>

#include "util/allocator.h"
#include "util/error.h"

utp_status_t utp_context_create(const utp_context_options_t *options, utp_context_t **out_context) {
    utp_context_t       *context;
    utp_internal_error_t error;
    char                 fragment[32];
    int                  fragment_length;

    if (out_context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    *out_context = NULL;
    if (options == NULL || options->event_base == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    context = utp_allocator_alloc(NULL, sizeof(*context));
    if (context == NULL) {
        return UTP_STATUS_NOMEM;
    }
    memset(context, 0, sizeof(*context));
    context->logger.sink = options->log_sink;
    fragment_length = snprintf(fragment, sizeof(fragment), "context %llu", (unsigned long long)options->context_id);
    if (fragment_length < 0 || (size_t)fragment_length >= sizeof(fragment)) {
        utp_allocator_free(NULL, context);
        return UTP_STATUS_OVERFLOW;
    }
    error = utp_log_tag_init(&context->tag, fragment, (size_t)fragment_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_event_loop_init(&context->event_loop, options->event_base, &context->logger, &context->tag);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context initialization failed");
        utp_allocator_free(NULL, context);
        return utp_internal_error_to_status(error);
    }
    *out_context = context;
    return UTP_STATUS_OK;
}

void utp_context_destroy(utp_context_t *context) {
    if (context != NULL) {
        utp_event_loop_close(&context->event_loop);
        utp_allocator_free(NULL, context);
    }
}
