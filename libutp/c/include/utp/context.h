#ifndef EULAR_UTP_C_CONTEXT_H
#define EULAR_UTP_C_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/log.h>
#include <utp/option.h>
#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_VERSION_MAJOR  1u
#define UTP_VERSION_MINOR  0u
#define UTP_VERSION_PATCH  1u
#define UTP_VERSION_STRING "1.0.1"

// Opaque handles reserved for the future stable C API.
typedef struct utp_context    utp_context_t;
typedef struct utp_connection utp_connection_t;
typedef struct utp_stream     utp_stream_t;
struct event_base;

typedef enum utp_encryption_mode {
    UTP_ENCRYPTION_NONE,
    UTP_ENCRYPTION_AES_GCM_128,
    UTP_ENCRYPTION_AES_GCM_256,
} utp_encryption_mode_t;

typedef enum utp_connect_attempt_type {
    UTP_CONNECT_ATTEMPT_NORMAL,
    UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN,
    UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE,
    UTP_CONNECT_ATTEMPT_PASSIVE,
} utp_connect_attempt_type_t;

typedef struct utp_endpoint {
    uint8_t  family;
    uint16_t port;
    uint32_t scope_id;
    uint8_t  address[16];
} utp_endpoint_t;

// Full initializer for utp_context_options_t. Set event_base before creating the Context.
#define UTP_CONTEXT_OPTIONS_INIT  \
    {NULL,                        \
     NULL,                        \
     0u,                          \
     UTP_LOG_LEVEL_INFO,          \
     UTP_STREAM_SCHEDULER_STRICT, \
     true,                        \
     1280u,                       \
     1500u,                       \
     1400u,                       \
     300u,                        \
     16u,                         \
     2000u,                       \
     1u,                          \
     3u,                          \
     3000u,                       \
     5000u}

typedef struct utp_connect_options {
    const char*           address;
    uint16_t              port;
    uint32_t              timeout_ms;
    int8_t                retries;
    utp_encryption_mode_t encryption;
} utp_connect_options_t;

// Full initializer for utp_connect_options_t.
#define UTP_CONNECT_OPTIONS_INIT {NULL, 0u, 3000u, 0, UTP_ENCRYPTION_NONE}

typedef struct utp_new_connection_info {
    utp_endpoint_t        remote;
    uint32_t              local_cid;
    uint32_t              peer_cid;
    utp_encryption_mode_t encryption;
} utp_new_connection_info_t;

typedef struct utp_connect_attempt_info {
    utp_endpoint_t             remote;
    uint32_t                   timeout_ms;
    int8_t                     retries;
    utp_encryption_mode_t      encryption;
    utp_connect_attempt_type_t type;
    uint32_t                   session_token_size;
    uint32_t                   resumption_state_size;
    uint32_t                   early_data_size;
    bool                       early_fin;
} utp_connect_attempt_info_t;

typedef void (*utp_on_connected_fn)(utp_connection_t* connection, void* user_data);
typedef void (*utp_on_connect_error_fn)(utp_status_t status, const char* message,
                                        const utp_connect_attempt_info_t* attempt, void* user_data);
typedef bool (*utp_on_new_connection_fn)(const utp_new_connection_info_t* info, void* user_data);
typedef struct utp_connection_error_info {
    // UTP_STATUS_OK denotes a normal peer close. Local fatal errors use their terminal status.
    utp_status_t   status;
    // The raw CONNECTION_CLOSE code from the peer, or zero for a local failure.
    uint16_t       peer_error_code;
    // A zero-copy reason view that is valid only for the duration of the callback.
    const uint8_t* reason;
    size_t         reason_length;
    bool           peer_initiated;
} utp_connection_error_info_t;

typedef void (*utp_on_connection_error_fn)(utp_connection_t* connection, const utp_connection_error_info_t* info,
                                           void* user_data);

// Returns the semantic version of the linked C library.
const char*  utp_version(void);
utp_status_t utp_context_create(const utp_context_options_t* options, utp_context_t** out_context);
// Synchronously destroys all Context-owned connections and streams. Established connections get one direct
// CONNECTION_CLOSE write attempt; this call does not wait for ACKs or a draining timeout.
void         utp_context_destroy(utp_context_t* context);
utp_status_t utp_context_bind(utp_context_t* context, const char* address, uint16_t port, const char* ifname,
                              uint16_t* out_port);
void         utp_context_set_on_connected(utp_context_t* context, utp_on_connected_fn callback, void* user_data);
void utp_context_set_on_connect_error(utp_context_t* context, utp_on_connect_error_fn callback, void* user_data);
void utp_context_set_on_new_connection(utp_context_t* context, utp_on_new_connection_fn callback, void* user_data);
void utp_context_set_on_connection_error(utp_context_t* context, utp_on_connection_error_fn callback, void* user_data);
utp_status_t utp_context_connect(utp_context_t* context, const utp_connect_options_t* options);
utp_status_t utp_context_accept(utp_context_t* context);

#ifdef __cplusplus
}
#endif

#include <utp/connection.h>
#include <utp/stream.h>

#endif  // EULAR_UTP_C_CONTEXT_H
