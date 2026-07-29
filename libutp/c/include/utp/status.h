#ifndef EULAR_UTP_STATUS_H
#define EULAR_UTP_STATUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t utp_status_t;

enum {
    UTP_STATUS_OK = 0,

    // Generic errors: -0x0001 through -0x001f.
    UTP_STATUS_INVALID_ARGUMENT = -0x0001,
    UTP_STATUS_NOMEM            = -0x0002,
    UTP_STATUS_LIMIT            = -0x0003,
    UTP_STATUS_EXISTS           = -0x0004,
    UTP_STATUS_NOT_FOUND        = -0x0005,
    UTP_STATUS_OVERFLOW         = -0x0006,
    UTP_STATUS_STATE            = -0x0007,
    UTP_STATUS_IO               = -0x0008,
    UTP_STATUS_WOULD_BLOCK      = -0x0009,
    UTP_STATUS_TIMEOUT          = -0x000a,
    UTP_STATUS_CLOSED           = -0x000b,
    UTP_STATUS_PROTOCOL         = -0x000c,
    UTP_STATUS_CRYPTO           = -0x000d,
    UTP_STATUS_AUTH             = -0x000e,
    UTP_STATUS_UNSUPPORTED      = -0x000f,
    UTP_STATUS_CANCELLED        = -0x0010,
    UTP_STATUS_BUSY             = -0x0011,
    UTP_STATUS_IN_PROGRESS      = -0x0012,
    UTP_STATUS_VERSION_MISMATCH = -0x0013,

    // Socket errors: -0x0020 through -0x003f.
    UTP_STATUS_SOCKET_OPEN      = -0x0020,
    UTP_STATUS_SOCKET_OPTION    = -0x0021,
    UTP_STATUS_SOCKET_NOT_BOUND = -0x0022,
    UTP_STATUS_SOCKET_BIND      = -0x0023,
    UTP_STATUS_SOCKET_IOCTL     = -0x0024,
    UTP_STATUS_SOCKET_READ      = -0x0025,
    UTP_STATUS_SOCKET_WRITE     = -0x0026,
    UTP_STATUS_SOCKET_CONNECTED = -0x0027,
    UTP_STATUS_SOCKET_EVENT     = -0x0028,

    // Stream errors: -0x0040 through -0x005f.
    UTP_STATUS_STREAM_FLOW_CONTROL = -0x0040,
    UTP_STATUS_STREAM_DATA_BLOCKED = -0x0041,
    UTP_STATUS_STREAM_DATA_LIMITED = -0x0042,
    UTP_STATUS_STREAM_ID_EXHAUSTED = -0x0043,

    // Frame errors: -0x0060 through -0x007f.
    UTP_STATUS_FRAME_FORMAT     = -0x0060,
    UTP_STATUS_FRAME_UNEXPECTED = -0x0061,

    // Cryptographic errors: -0x0080 through -0x009f.
    UTP_STATUS_CRYPTO_UNINITIALIZED  = -0x0080,
    UTP_STATUS_CRYPTO_INITIALIZATION = -0x0081,
    UTP_STATUS_CRYPTO_ENCRYPTION     = -0x0082,
    UTP_STATUS_CRYPTO_DECRYPTION     = -0x0083,
    UTP_STATUS_RANDOM_GENERATION     = -0x0084,

    // Context errors: -0x00a0 through -0x00bf.
    UTP_STATUS_CONTEXT_UNAVAILABLE = -0x00a0,
    UTP_STATUS_CONTEXT_EVENT       = -0x00a1,

    // Connection errors: -0x00c0 through -0x00df.
    UTP_STATUS_CONNECTION_HANDSHAKE                    = -0x00c0,
    UTP_STATUS_CONNECTION_CLOSING                      = -0x00c1,
    UTP_STATUS_CONNECTION_CID_CONFLICT                 = -0x00c2,
    UTP_STATUS_CONNECTION_STREAM_LIMITED               = -0x00c3,
    UTP_STATUS_CONNECTION_PATH_VALIDATION_BLOCKED      = -0x00c4,
    UTP_STATUS_CONNECTION_SESSION_TOKEN_UNAVAILABLE    = -0x00c5,
    UTP_STATUS_CONNECTION_RESUMPTION_STATE_UNAVAILABLE = -0x00c6,

    // Rendezvous errors: -0x00e0 through -0x00ff.
    UTP_STATUS_RENDEZVOUS_UNAVAILABLE = -0x00e0,
    UTP_STATUS_RENDEZVOUS_REJECTED    = -0x00e1,

    // Application errors are reserved at and below this value.
    UTP_STATUS_APPLICATION_ERROR_BASE = -0x0100
};

const char* utp_status_string(utp_status_t status);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_STATUS_H
