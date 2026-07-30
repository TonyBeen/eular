#ifndef EULAR_UTP_C_UTP_H
#define EULAR_UTP_C_UTP_H

#include <stdint.h>
#include <utp/log.h>
#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_VERSION_MAJOR  1u
#define UTP_VERSION_MINOR  0u
#define UTP_VERSION_PATCH  0u
#define UTP_VERSION_STRING "1.0.0"

// Opaque handles reserved for the future stable C API.
typedef struct utp_context    utp_context_t;
typedef struct utp_connection utp_connection_t;
typedef struct utp_stream     utp_stream_t;
struct event_base;

typedef struct utp_context_options {
    struct event_base *event_base;
    utp_log_sink_fn    log_sink;
    uint64_t           context_id;
} utp_context_options_t;

// Returns the semantic version of the linked C library.
const char  *utp_version(void);
utp_status_t utp_context_create(const utp_context_options_t *options, utp_context_t **out_context);
void         utp_context_destroy(utp_context_t *context);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_UTP_H
