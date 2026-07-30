#ifndef EULAR_UTP_INTERNAL_FRAME_H
#define EULAR_UTP_INTERNAL_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "proto/proto.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_FRAME_BIT(type)                    (UINT32_C(1) << (type))
#define UTP_FRAME_PATH_SIZE                    9u
#define UTP_FRAME_VERSION_SIZE                 5u
#define UTP_FRAME_HANDSHAKE_DONE_SIZE          9u
#define UTP_FRAME_STREAM_HEADER_SIZE           16u
#define UTP_FRAME_PADDING_HEADER_SIZE          3u
#define UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE 5u
#define UTP_FRAME_RESET_STREAM_SIZE            15u
#define UTP_STREAM_FLAG_NONE                   0x00u
#define UTP_STREAM_FLAG_FIN                    0x01u

typedef enum utp_frame_type {
    UTP_FRAME_TYPE_INVALID             = 0x00,
    UTP_FRAME_TYPE_STREAM              = 0x01,
    UTP_FRAME_TYPE_ACK                 = 0x02,
    UTP_FRAME_TYPE_PADDING             = 0x03,
    UTP_FRAME_TYPE_CONNECTION_CLOSE    = 0x04,
    UTP_FRAME_TYPE_PING                = 0x05,
    UTP_FRAME_TYPE_RESET_STREAM        = 0x06,
    UTP_FRAME_TYPE_STREAMS_BLOCKED     = 0x07,
    UTP_FRAME_TYPE_MAX_STREAMS         = 0x08,
    UTP_FRAME_TYPE_PATH_CHALLENGE      = 0x09,
    UTP_FRAME_TYPE_PATH_RESPONSE       = 0x0a,
    UTP_FRAME_TYPE_CRYPTO              = 0x0b,
    UTP_FRAME_TYPE_SESSION_TOKEN       = 0x0c,
    UTP_FRAME_TYPE_ACK_FREQUENCY       = 0x0d,
    UTP_FRAME_TYPE_VERSION             = 0x0e,
    UTP_FRAME_TYPE_HANDSHAKE_DONE      = 0x0f,
    UTP_FRAME_TYPE_TRANSPORT_PARAMS    = 0x10,
    UTP_FRAME_TYPE_HANDSHAKE_DELAY     = 0x11,
    UTP_FRAME_TYPE_MAX_DATA            = 0x12,
    UTP_FRAME_TYPE_MAX_STREAM_DATA     = 0x13,
    UTP_FRAME_TYPE_DATA_BLOCKED        = 0x14,
    UTP_FRAME_TYPE_STREAM_DATA_BLOCKED = 0x15,
    UTP_FRAME_TYPE_MAX                 = 0x16
} utp_frame_type_t;

typedef struct utp_packet_view {
    utp_packet_header_t header;
    const uint8_t      *payload;
    size_t              payload_length;
    uint32_t            frame_types;
} utp_packet_view_t;

typedef struct utp_frame_path {
    uint8_t data[8];
} utp_frame_path_t;

typedef struct utp_frame_version {
    uint32_t version;
} utp_frame_version_t;

typedef struct utp_frame_handshake_done {
    uint64_t ack_handshake_packet_number;
} utp_frame_handshake_done_t;

typedef struct utp_frame_stream {
    uint8_t        flags;
    uint32_t       stream_id;
    uint64_t       offset;
    const uint8_t *data;
    uint16_t       data_length;
} utp_frame_stream_t;

typedef struct utp_frame_connection_close {
    uint16_t       error_code;
    const uint8_t *reason;
    uint16_t       reason_length;
} utp_frame_connection_close_t;

typedef struct utp_frame_reset_stream {
    uint16_t error_code;
    uint32_t stream_id;
    uint64_t final_size;
} utp_frame_reset_stream_t;

utp_internal_error_t utp_frame_measure(const uint8_t *frame, size_t available, uint8_t *frame_type,
                                       size_t *frame_length);
utp_internal_error_t utp_frame_scan(const uint8_t *payload, size_t payload_length, uint32_t *frame_types);
utp_internal_error_t utp_packet_view_decode(utp_packet_view_t *view, const uint8_t *packet, size_t packet_length);
utp_internal_error_t utp_packet_view_next_frame(const utp_packet_view_t *view, size_t *offset, uint8_t *frame_type,
                                                const uint8_t **frame_data, size_t *frame_length);
utp_internal_error_t utp_frame_path_encode(uint8_t *buffer, size_t capacity, uint8_t type,
                                           const utp_frame_path_t *path);
utp_internal_error_t utp_frame_path_decode(utp_frame_path_t *path, const uint8_t *buffer, size_t length,
                                           uint8_t expected_type);
utp_internal_error_t utp_frame_version_encode(uint8_t *buffer, size_t capacity, const utp_frame_version_t *version);
utp_internal_error_t utp_frame_version_decode(utp_frame_version_t *version, const uint8_t *buffer, size_t length);
utp_internal_error_t utp_frame_handshake_done_encode(uint8_t *buffer, size_t capacity,
                                                     const utp_frame_handshake_done_t *done);
utp_internal_error_t utp_frame_handshake_done_decode(utp_frame_handshake_done_t *done, const uint8_t *buffer,
                                                     size_t length);
utp_internal_error_t utp_frame_stream_encode(uint8_t *buffer, size_t capacity, const utp_frame_stream_t *stream);
utp_internal_error_t utp_frame_stream_decode(utp_frame_stream_t *stream, const uint8_t *buffer, size_t length);
utp_internal_error_t utp_frame_padding_encode(uint8_t *buffer, size_t capacity, uint16_t padding_length);
utp_internal_error_t utp_frame_padding_decode(uint16_t *padding_length, const uint8_t *buffer, size_t length);
utp_internal_error_t utp_frame_connection_close_encode(uint8_t *buffer, size_t capacity,
                                                       const utp_frame_connection_close_t *close);
utp_internal_error_t utp_frame_connection_close_decode(utp_frame_connection_close_t *close, const uint8_t *buffer,
                                                       size_t length);
utp_internal_error_t utp_frame_reset_stream_encode(uint8_t *buffer, size_t capacity,
                                                   const utp_frame_reset_stream_t *reset);
utp_internal_error_t utp_frame_reset_stream_decode(utp_frame_reset_stream_t *reset, const uint8_t *buffer,
                                                   size_t length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_FRAME_H
