#ifndef EULAR_UTP_C_CONNECTION_H
#define EULAR_UTP_C_CONNECTION_H

#include <stdbool.h>
#include <stdint.h>

#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_connection utp_connection_t;
typedef struct utp_stream     utp_stream_t;

typedef enum utp_stream_type { UTP_STREAM_TYPE_BIDIRECTIONAL = 0, UTP_STREAM_TYPE_UNIDIRECTIONAL } utp_stream_type_t;

// Starts graceful connection shutdown. Errors are reported asynchronously through the Context callback.
void          utp_connection_close(utp_connection_t* connection);
utp_status_t  utp_connection_create_stream(utp_connection_t* connection, utp_stream_type_t type,
                                           uint32_t* out_stream_id);
// Returns a connection-owned, borrowed stream pointer. It remains valid until the stream is fully closed and its
// receive data has been consumed, or until its connection is destroyed. Do not retain it after either condition.
utp_stream_t* utp_connection_get_stream(utp_connection_t* connection, uint32_t stream_id);
bool          utp_connection_is_connected(const utp_connection_t* connection);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_CONNECTION_H
