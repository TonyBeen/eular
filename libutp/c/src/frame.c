#include "internal/frame.h"

#include "internal/ack.h"
#include "internal/wire.h"

#define UTP_FRAME_CRYPTO_SIZE               35u
#define UTP_FRAME_SESSION_TOKEN_HEADER_SIZE 4u
#define UTP_FRAME_ACK_FREQUENCY_SIZE        7u
#define UTP_FRAME_TRANSPORT_PARAMS_SIZE     38u
#define UTP_FRAME_HANDSHAKE_DELAY_SIZE      5u
#define UTP_FRAME_MAX_DATA_SIZE             9u
#define UTP_FRAME_MAX_STREAM_DATA_SIZE      13u
#define UTP_FRAME_DATA_BLOCKED_SIZE         9u
#define UTP_FRAME_STREAM_DATA_BLOCKED_SIZE  13u

static uint16_t utp_frame_read_u16(const uint8_t *data) { return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]); }

static utp_internal_error_t utp_frame_require_size(size_t available, size_t required) {
    return available < required ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_measure(const uint8_t *frame, size_t available, uint8_t *frame_type,
                                       size_t *frame_length) {
    uint8_t              type;
    size_t               length;
    size_t               variable_length;
    utp_internal_error_t error;

    if (frame == NULL || frame_type == NULL || frame_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (available == 0u) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    type   = frame[0];
    length = 0u;
    switch (type) {
        case UTP_FRAME_TYPE_PING:
            length = 1u;
            break;
        case UTP_FRAME_TYPE_HANDSHAKE_DONE:
            length = UTP_FRAME_HANDSHAKE_DONE_SIZE;
            break;
        case UTP_FRAME_TYPE_PATH_CHALLENGE:
        case UTP_FRAME_TYPE_PATH_RESPONSE:
            length = UTP_FRAME_PATH_SIZE;
            break;
        case UTP_FRAME_TYPE_VERSION:
            length = UTP_FRAME_VERSION_SIZE;
            break;
        case UTP_FRAME_TYPE_PADDING:
            error = utp_frame_require_size(available, UTP_FRAME_PADDING_HEADER_SIZE);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            variable_length = (size_t)utp_frame_read_u16(frame + 1u);
            length          = UTP_FRAME_PADDING_HEADER_SIZE + variable_length;
            break;
        case UTP_FRAME_TYPE_SESSION_TOKEN:
            error = utp_frame_require_size(available, UTP_FRAME_SESSION_TOKEN_HEADER_SIZE);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            variable_length = (size_t)frame[1];
            length          = UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + variable_length;
            break;
        case UTP_FRAME_TYPE_CONNECTION_CLOSE:
            error = utp_frame_require_size(available, UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            variable_length = (size_t)utp_frame_read_u16(frame + 3u);
            length          = UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE + variable_length;
            break;
        case UTP_FRAME_TYPE_STREAM:
            error = utp_frame_require_size(available, UTP_FRAME_STREAM_HEADER_SIZE);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            variable_length = (size_t)utp_frame_read_u16(frame + 2u);
            length          = UTP_FRAME_STREAM_HEADER_SIZE + variable_length;
            break;
        case UTP_FRAME_TYPE_ACK:
            error = utp_frame_require_size(available, UTP_ACK_FRAME_HEADER_SIZE);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            variable_length = (size_t)frame[1] * UTP_ACK_FRAME_RANGE_SIZE;
            length          = UTP_ACK_FRAME_HEADER_SIZE + variable_length;
            break;
        case UTP_FRAME_TYPE_CRYPTO:
            length = UTP_FRAME_CRYPTO_SIZE;
            break;
        case UTP_FRAME_TYPE_RESET_STREAM:
            length = UTP_FRAME_RESET_STREAM_SIZE;
            break;
        case UTP_FRAME_TYPE_ACK_FREQUENCY:
            length = UTP_FRAME_ACK_FREQUENCY_SIZE;
            break;
        case UTP_FRAME_TYPE_TRANSPORT_PARAMS:
            length = UTP_FRAME_TRANSPORT_PARAMS_SIZE;
            break;
        case UTP_FRAME_TYPE_HANDSHAKE_DELAY:
            length = UTP_FRAME_HANDSHAKE_DELAY_SIZE;
            break;
        case UTP_FRAME_TYPE_MAX_DATA:
            length = UTP_FRAME_MAX_DATA_SIZE;
            break;
        case UTP_FRAME_TYPE_MAX_STREAM_DATA:
            length = UTP_FRAME_MAX_STREAM_DATA_SIZE;
            break;
        case UTP_FRAME_TYPE_DATA_BLOCKED:
            length = UTP_FRAME_DATA_BLOCKED_SIZE;
            break;
        case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED:
            length = UTP_FRAME_STREAM_DATA_BLOCKED_SIZE;
            break;
        default:
            return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_frame_require_size(available, length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *frame_type   = type;
    *frame_length = length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_scan(const uint8_t *payload, size_t payload_length, uint32_t *frame_types) {
    size_t   offset = 0u;
    uint32_t types  = 0u;

    if (frame_types == NULL || (payload == NULL && payload_length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    while (offset < payload_length) {
        size_t               frame_length;
        uint8_t              frame_type;
        utp_internal_error_t error =
            utp_frame_measure(payload + offset, payload_length - offset, &frame_type, &frame_length);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        types  |= UTP_FRAME_BIT(frame_type);
        offset += frame_length;
    }
    *frame_types = types;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_packet_view_decode(utp_packet_view_t *view, const uint8_t *packet, size_t packet_length) {
    utp_packet_view_t    decoded;
    utp_internal_error_t error;

    if (view == NULL || packet == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_proto_decode_header(&decoded.header, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (packet_length - UTP_PACKET_HEADER_SIZE < (size_t)decoded.header.payload_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    decoded.payload        = packet + UTP_PACKET_HEADER_SIZE;
    decoded.payload_length = (size_t)decoded.header.payload_length;
    error                  = utp_frame_scan(decoded.payload, decoded.payload_length, &decoded.frame_types);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    *view = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_packet_view_next_frame(const utp_packet_view_t *view, size_t *offset, uint8_t *frame_type,
                                                const uint8_t **frame_data, size_t *frame_length) {
    const uint8_t       *data;
    size_t               length;
    uint8_t              type;
    utp_internal_error_t error;

    if (view == NULL || offset == NULL || frame_type == NULL || frame_data == NULL || frame_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (view->payload == NULL || *offset >= view->payload_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    data  = view->payload + *offset;
    error = utp_frame_measure(data, view->payload_length - *offset, &type, &length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *offset       += length;
    *frame_type    = type;
    *frame_data    = data;
    *frame_length  = length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_path_encode(uint8_t *buffer, size_t capacity, uint8_t type,
                                           const utp_frame_path_t *path) {
    utp_wire_writer_t    writer;
    size_t               index;
    utp_internal_error_t error;

    if (buffer == NULL || path == NULL ||
        (type != UTP_FRAME_TYPE_PATH_CHALLENGE && type != UTP_FRAME_TYPE_PATH_RESPONSE)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_PATH_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (index = 0u; index < sizeof(path->data); ++index) {
        error = utp_wire_write_u8(&writer, path->data[index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_path_decode(utp_frame_path_t *path, const uint8_t *buffer, size_t length,
                                           uint8_t expected_type) {
    utp_wire_reader_t    reader;
    utp_frame_path_t     decoded;
    uint8_t              type;
    size_t               index;
    utp_internal_error_t error;

    if (path == NULL || buffer == NULL ||
        (expected_type != UTP_FRAME_TYPE_PATH_CHALLENGE && expected_type != UTP_FRAME_TYPE_PATH_RESPONSE)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_PATH_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_PATH_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != expected_type) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    for (index = 0u; index < sizeof(decoded.data); ++index) {
        error = utp_wire_read_u8(&reader, &decoded.data[index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    *path = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_version_encode(uint8_t *buffer, size_t capacity, const utp_frame_version_t *version) {
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (buffer == NULL || version == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_VERSION_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_VERSION);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u32(&writer, version->version);
}

utp_internal_error_t utp_frame_version_decode(utp_frame_version_t *version, const uint8_t *buffer, size_t length) {
    utp_wire_reader_t    reader;
    utp_frame_version_t  decoded;
    uint8_t              type;
    utp_internal_error_t error;

    if (version == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_VERSION_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_VERSION_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_VERSION) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u32(&reader, &decoded.version);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *version = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_handshake_done_encode(uint8_t *buffer, size_t capacity,
                                                     const utp_frame_handshake_done_t *done) {
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (buffer == NULL || done == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_HANDSHAKE_DONE_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_HANDSHAKE_DONE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u64(&writer, done->ack_handshake_packet_number);
}

utp_internal_error_t utp_frame_handshake_done_decode(utp_frame_handshake_done_t *done, const uint8_t *buffer,
                                                     size_t length) {
    utp_wire_reader_t          reader;
    utp_frame_handshake_done_t decoded;
    uint8_t                    type;
    utp_internal_error_t       error;

    if (done == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_HANDSHAKE_DONE_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_HANDSHAKE_DONE_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_HANDSHAKE_DONE) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u64(&reader, &decoded.ack_handshake_packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *done = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_stream_encode(uint8_t *buffer, size_t capacity, const utp_frame_stream_t *stream) {
    utp_wire_writer_t    writer;
    size_t               index;
    size_t               frame_length;
    utp_internal_error_t error;

    if (buffer == NULL || stream == NULL || (stream->data_length != 0u && stream->data == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    frame_length = UTP_FRAME_STREAM_HEADER_SIZE + (size_t)stream->data_length;
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_STREAM);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, stream->flags);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, stream->data_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, stream->stream_id);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u64(&writer, stream->offset);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (index = 0u; index < (size_t)stream->data_length; ++index) {
        error = utp_wire_write_u8(&writer, stream->data[index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_stream_decode(utp_frame_stream_t *stream, const uint8_t *buffer, size_t length) {
    utp_wire_reader_t    reader;
    utp_frame_stream_t   decoded;
    uint8_t              type;
    size_t               frame_length;
    utp_internal_error_t error;

    if (stream == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_STREAM_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_STREAM_HEADER_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_STREAM) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u8(&reader, &decoded.flags);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u16(&reader, &decoded.data_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    frame_length = UTP_FRAME_STREAM_HEADER_SIZE + (size_t)decoded.data_length;
    if (length < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_read_u32(&reader, &decoded.stream_id);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u64(&reader, &decoded.offset);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    decoded.data = buffer + UTP_FRAME_STREAM_HEADER_SIZE;
    *stream      = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_padding_encode(uint8_t *buffer, size_t capacity, uint16_t padding_length) {
    utp_wire_writer_t    writer;
    size_t               frame_length = UTP_FRAME_PADDING_HEADER_SIZE + (size_t)padding_length;
    size_t               index;
    utp_internal_error_t error;

    if (buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_PADDING);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, padding_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (index = 0u; index < (size_t)padding_length; ++index) {
        error = utp_wire_write_u8(&writer, 0u);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_padding_decode(uint16_t *padding_length, const uint8_t *buffer, size_t length) {
    utp_wire_reader_t    reader;
    uint8_t              type;
    uint16_t             decoded_length;
    size_t               frame_length;
    utp_internal_error_t error;

    if (padding_length == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_PADDING_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_PADDING_HEADER_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_PADDING) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u16(&reader, &decoded_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    frame_length = UTP_FRAME_PADDING_HEADER_SIZE + (size_t)decoded_length;
    if (length < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *padding_length = decoded_length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_connection_close_encode(uint8_t *buffer, size_t capacity,
                                                       const utp_frame_connection_close_t *close) {
    utp_wire_writer_t    writer;
    size_t               frame_length;
    size_t               index;
    utp_internal_error_t error;

    if (buffer == NULL || close == NULL || (close->reason_length != 0u && close->reason == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    frame_length = UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE + (size_t)close->reason_length;
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_CONNECTION_CLOSE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, close->error_code);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, close->reason_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (index = 0u; index < (size_t)close->reason_length; ++index) {
        error = utp_wire_write_u8(&writer, close->reason[index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_connection_close_decode(utp_frame_connection_close_t *close, const uint8_t *buffer,
                                                       size_t length) {
    utp_wire_reader_t            reader;
    utp_frame_connection_close_t decoded;
    uint8_t                      type;
    size_t                       frame_length;
    utp_internal_error_t         error;

    if (close == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_CONNECTION_CLOSE) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u16(&reader, &decoded.error_code);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u16(&reader, &decoded.reason_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    frame_length = UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE + (size_t)decoded.reason_length;
    if (length < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    decoded.reason = buffer + UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE;
    *close         = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_reset_stream_encode(uint8_t *buffer, size_t capacity,
                                                   const utp_frame_reset_stream_t *reset) {
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (buffer == NULL || reset == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_RESET_STREAM_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_RESET_STREAM);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, reset->error_code);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, reset->stream_id);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u64(&writer, reset->final_size);
}

utp_internal_error_t utp_frame_reset_stream_decode(utp_frame_reset_stream_t *reset, const uint8_t *buffer,
                                                   size_t length) {
    utp_wire_reader_t        reader;
    utp_frame_reset_stream_t decoded;
    uint8_t                  type;
    utp_internal_error_t     error;

    if (reset == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_FRAME_RESET_STREAM_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_RESET_STREAM_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_RESET_STREAM) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u16(&reader, &decoded.error_code);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u32(&reader, &decoded.stream_id);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u64(&reader, &decoded.final_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *reset = decoded;
    return UTP_INTERNAL_ERROR_OK;
}
