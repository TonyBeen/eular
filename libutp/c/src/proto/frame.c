#include "proto/frame.h"

#include "proto/ack.h"
#include "proto/wire.h"

/** @brief 从网络字节序缓冲区读取无对齐要求的 16 位整数。 */
static uint16_t utp_frame_read_u16(const uint8_t* data) { return (uint16_t)(((uint16_t)data[0] << 8u) | data[1]); }

/** @brief 确认输入缓冲区至少容纳指定长度的帧或固定头。 */
static utp_internal_error_t utp_frame_require_size(size_t available, size_t required)
{
    return available < required ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_measure(const uint8_t* frame, size_t available, uint8_t* frame_type,
                                       size_t* frame_length)
{
    uint8_t              type;
    size_t               length;
    size_t               variable_length;
    utp_internal_error_t error;

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
    case UTP_FRAME_TYPE_STOP_SENDING:
        length = UTP_FRAME_STOP_SENDING_SIZE;
        break;
    case UTP_FRAME_TYPE_STREAMS_BLOCKED:
    case UTP_FRAME_TYPE_MAX_STREAMS:
        length = UTP_FRAME_STREAMS_LIMIT_SIZE;
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
    case UTP_FRAME_TYPE_RENDEZVOUS:
        error = utp_frame_require_size(available, UTP_FRAME_RENDEZVOUS_HEADER_SIZE);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        variable_length = (size_t)utp_frame_read_u16(frame + 2u);
        length          = UTP_FRAME_RENDEZVOUS_HEADER_SIZE + variable_length;
        break;
    case UTP_FRAME_TYPE_OBSERVED_ADDRESS:
        error = utp_frame_require_size(available, 2u);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (frame[1] == 4u) {
            length = UTP_FRAME_OBSERVED_ADDRESS_IPV4_SIZE;
        } else if (frame[1] == 6u) {
            length = UTP_FRAME_OBSERVED_ADDRESS_IPV6_SIZE;
        } else {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
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

utp_internal_error_t utp_frame_scan(const uint8_t* payload, size_t payload_length, uint32_t* frame_types)
{
    size_t   offset = 0u;
    uint32_t types  = 0u;

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

utp_internal_error_t utp_packet_view_decode(utp_packet_view_t* view, const uint8_t* packet, size_t packet_length)
{
    utp_packet_view_t    decoded;
    utp_internal_error_t error;

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

utp_internal_error_t utp_packet_view_next_frame(const utp_packet_view_t* view, size_t* offset, uint8_t* frame_type,
                                                const uint8_t** frame_data, size_t* frame_length)
{
    const uint8_t*       data;
    size_t               length;
    uint8_t              type;
    utp_internal_error_t error;

    if (*offset >= view->payload_length) {
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

utp_internal_error_t utp_frame_path_encode(uint8_t* buffer, size_t capacity, uint8_t type, const utp_frame_path_t* path)
{
    utp_wire_writer_t    writer;
    size_t               index;
    utp_internal_error_t error;

    if (type != UTP_FRAME_TYPE_PATH_CHALLENGE && type != UTP_FRAME_TYPE_PATH_RESPONSE) {
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

utp_internal_error_t utp_frame_path_decode(utp_frame_path_t* path, const uint8_t* buffer, size_t length,
                                           uint8_t expected_type)
{
    utp_wire_reader_t    reader;
    utp_frame_path_t     decoded;
    uint8_t              type;
    size_t               index;
    utp_internal_error_t error;

    if (expected_type != UTP_FRAME_TYPE_PATH_CHALLENGE && expected_type != UTP_FRAME_TYPE_PATH_RESPONSE) {
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

utp_internal_error_t utp_frame_rendezvous_encode(uint8_t* buffer, size_t capacity,
                                                 const utp_frame_rendezvous_t* rendezvous)
{
    utp_wire_writer_t    writer;
    size_t               index;
    size_t               frame_length;
    utp_internal_error_t error;

    if (rendezvous->message_type == 0u || (rendezvous->payload_length != 0u && rendezvous->payload == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    frame_length = UTP_FRAME_RENDEZVOUS_HEADER_SIZE + (size_t)rendezvous->payload_length;
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_RENDEZVOUS);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, rendezvous->message_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, rendezvous->payload_length);
    }
    for (index = 0u; index < (size_t)rendezvous->payload_length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(&writer, rendezvous->payload[index]);
    }
    return error;
}

utp_internal_error_t utp_frame_rendezvous_decode(utp_frame_rendezvous_t* rendezvous, const uint8_t* buffer,
                                                 size_t length)
{
    utp_wire_reader_t      reader;
    utp_frame_rendezvous_t decoded;
    uint8_t                type;
    utp_internal_error_t   error;
    size_t                 frame_length;

    if (length < UTP_FRAME_RENDEZVOUS_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_RENDEZVOUS_HEADER_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.message_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u16(&reader, &decoded.payload_length);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_RENDEZVOUS || decoded.message_type == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    frame_length = UTP_FRAME_RENDEZVOUS_HEADER_SIZE + (size_t)decoded.payload_length;
    if (length < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    decoded.payload = buffer + UTP_FRAME_RENDEZVOUS_HEADER_SIZE;
    *rendezvous     = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_observed_address_encode(uint8_t* buffer, size_t capacity,
                                                       const utp_frame_observed_address_t* address)
{
    utp_wire_writer_t    writer;
    size_t               address_length;
    size_t               index;
    size_t               frame_length;
    utp_internal_error_t error;

    if (address->family == 4u) {
        address_length = 4u;
        frame_length   = UTP_FRAME_OBSERVED_ADDRESS_IPV4_SIZE;
    } else if (address->family == 6u) {
        address_length = 16u;
        frame_length   = UTP_FRAME_OBSERVED_ADDRESS_IPV6_SIZE;
    } else {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (address->port == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_OBSERVED_ADDRESS);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, address->family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, address->port);
    }
    for (index = 0u; index < address_length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(&writer, address->address[index]);
    }
    return error;
}

utp_internal_error_t utp_frame_observed_address_decode(utp_frame_observed_address_t* address, const uint8_t* buffer,
                                                       size_t length)
{
    utp_wire_reader_t            reader;
    utp_frame_observed_address_t decoded = {0};
    size_t                       address_length;
    size_t                       frame_length;
    size_t                       index;
    uint8_t                      type;
    utp_internal_error_t         error;

    if (length < UTP_FRAME_OBSERVED_ADDRESS_IPV4_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_OBSERVED_ADDRESS_IPV4_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u16(&reader, &decoded.port);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_OBSERVED_ADDRESS || decoded.port == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (decoded.family == 4u) {
        address_length = 4u;
        frame_length   = UTP_FRAME_OBSERVED_ADDRESS_IPV4_SIZE;
    } else if (decoded.family == 6u) {
        address_length = 16u;
        frame_length   = UTP_FRAME_OBSERVED_ADDRESS_IPV6_SIZE;
    } else {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (length < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer + 4u, address_length);
    for (index = 0u; index < address_length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_read_u8(&reader, &decoded.address[index]);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *address = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_crypto_encode(uint8_t* buffer, size_t capacity, const utp_frame_crypto_t* crypto)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;
    size_t               index;

    if (crypto->crypto_type > UTP_FRAME_CRYPTO_TYPE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_CRYPTO_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_CRYPTO);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, crypto->crypto_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, 0u);
    }
    for (index = 0u; index < sizeof(crypto->ephemeral_public_key) && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(&writer, crypto->ephemeral_public_key[index]);
    }
    return error;
}

utp_internal_error_t utp_frame_crypto_decode(utp_frame_crypto_t* crypto, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t    reader;
    utp_frame_crypto_t   decoded;
    uint8_t              type;
    uint8_t              reserved;
    size_t               index;
    utp_internal_error_t error;

    if (length < UTP_FRAME_CRYPTO_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_CRYPTO_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.crypto_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &reserved);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_CRYPTO || decoded.crypto_type > UTP_FRAME_CRYPTO_TYPE_AES_GCM_256 || reserved != 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    for (index = 0u; index < sizeof(decoded.ephemeral_public_key); ++index) {
        error = utp_wire_read_u8(&reader, &decoded.ephemeral_public_key[index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    *crypto = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_version_encode(uint8_t* buffer, size_t capacity, const utp_frame_version_t* version)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_version_decode(utp_frame_version_t* version, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t    reader;
    utp_frame_version_t  decoded;
    uint8_t              type;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_handshake_done_encode(uint8_t* buffer, size_t capacity,
                                                     const utp_frame_handshake_done_t* done)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_handshake_done_decode(utp_frame_handshake_done_t* done, const uint8_t* buffer,
                                                     size_t length)
{
    utp_wire_reader_t          reader;
    utp_frame_handshake_done_t decoded;
    uint8_t                    type;
    utp_internal_error_t       error;

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

utp_internal_error_t utp_frame_handshake_delay_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_frame_handshake_delay_t* delay)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (capacity < UTP_FRAME_HANDSHAKE_DELAY_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_HANDSHAKE_DELAY);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u32(&writer, delay->delay_time_us);
    }
    return error;
}

utp_internal_error_t utp_frame_handshake_delay_decode(utp_frame_handshake_delay_t* delay, const uint8_t* buffer,
                                                      size_t length)
{
    utp_wire_reader_t           reader;
    utp_frame_handshake_delay_t decoded;
    uint8_t                     type;
    utp_internal_error_t        error;

    if (length < UTP_FRAME_HANDSHAKE_DELAY_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_HANDSHAKE_DELAY_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &type);
    }
    if (error == UTP_INTERNAL_ERROR_OK && type != UTP_FRAME_TYPE_HANDSHAKE_DELAY) {
        error = UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u32(&reader, &decoded.delay_time_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *delay = decoded;
    }
    return error;
}

utp_internal_error_t utp_frame_stream_encode(uint8_t* buffer, size_t capacity, const utp_frame_stream_t* stream)
{
    size_t               index;
    size_t               frame_length;
    utp_internal_error_t error;

    if (stream->data_length != 0u && stream->data == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    frame_length = UTP_FRAME_STREAM_HEADER_SIZE + (size_t)stream->data_length;
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_frame_stream_header_encode(buffer, capacity, stream->flags, stream->stream_id, stream->offset,
                                           stream->data_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (index = 0u; index < (size_t)stream->data_length; ++index) {
        buffer[UTP_FRAME_STREAM_HEADER_SIZE + index] = stream->data[index];
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_stream_header_encode(uint8_t* buffer, size_t capacity, uint8_t flags, uint32_t stream_id,
                                                    uint64_t offset, uint16_t data_length)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (capacity < UTP_FRAME_STREAM_HEADER_SIZE) {
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
    error = utp_wire_write_u8(&writer, flags);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, data_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, stream_id);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u64(&writer, offset);
}

utp_internal_error_t utp_frame_stream_decode(utp_frame_stream_t* stream, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t    reader;
    utp_frame_stream_t   decoded;
    uint8_t              type;
    size_t               frame_length;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_session_token_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_frame_session_token_t* token)
{
    utp_wire_writer_t    writer;
    size_t               index;
    utp_internal_error_t error;

    if (token->payload_length != 0u && token->payload == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + (size_t)token->payload_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_SESSION_TOKEN);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, token->payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u64(&writer, token->expires_at_seconds);
    }
    for (index = 0u; error == UTP_INTERNAL_ERROR_OK && index < (size_t)token->payload_length; ++index) {
        error = utp_wire_write_u8(&writer, token->payload[index]);
    }
    return error;
}

utp_internal_error_t utp_frame_session_token_decode(utp_frame_session_token_t* token, const uint8_t* buffer,
                                                    size_t length)
{
    utp_wire_reader_t         reader;
    utp_frame_session_token_t decoded;
    uint8_t                   type;
    utp_internal_error_t      error;

    if (length < UTP_FRAME_SESSION_TOKEN_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_SESSION_TOKEN_HEADER_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &type);
    }
    if (error == UTP_INTERNAL_ERROR_OK && type != UTP_FRAME_TYPE_SESSION_TOKEN) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u64(&reader, &decoded.expires_at_seconds);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (length < UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + (size_t)decoded.payload_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    decoded.payload = buffer + UTP_FRAME_SESSION_TOKEN_HEADER_SIZE;
    *token          = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_padding_encode(uint8_t* buffer, size_t capacity, uint16_t padding_length)
{
    utp_wire_writer_t    writer;
    size_t               frame_length = UTP_FRAME_PADDING_HEADER_SIZE + (size_t)padding_length;
    size_t               index;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_padding_decode(uint16_t* padding_length, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t    reader;
    uint8_t              type;
    uint16_t             decoded_length;
    size_t               frame_length;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_connection_close_encode(uint8_t* buffer, size_t capacity,
                                                       const utp_frame_connection_close_t* close)
{
    utp_wire_writer_t    writer;
    size_t               frame_length;
    size_t               index;
    utp_internal_error_t error;

    if (close->reason_length != 0u && close->reason == NULL) {
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

utp_internal_error_t utp_frame_connection_close_decode(utp_frame_connection_close_t* close, const uint8_t* buffer,
                                                       size_t length)
{
    utp_wire_reader_t            reader;
    utp_frame_connection_close_t decoded;
    uint8_t                      type;
    size_t                       frame_length;
    utp_internal_error_t         error;

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

utp_internal_error_t utp_frame_reset_stream_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_frame_reset_stream_t* reset)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

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

utp_internal_error_t utp_frame_reset_stream_decode(utp_frame_reset_stream_t* reset, const uint8_t* buffer,
                                                   size_t length)
{
    utp_wire_reader_t        reader;
    utp_frame_reset_stream_t decoded;
    uint8_t                  type;
    utp_internal_error_t     error;

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

utp_internal_error_t utp_frame_stop_sending_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_frame_stop_sending_t* stop)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (capacity < UTP_FRAME_STOP_SENDING_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_STOP_SENDING);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, stop->error_code);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u32(&writer, stop->stream_id);
    }
    return error;
}

utp_internal_error_t utp_frame_stop_sending_decode(utp_frame_stop_sending_t* stop, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t        reader;
    utp_frame_stop_sending_t decoded;
    uint8_t                  type;
    utp_internal_error_t     error;

    if (length < UTP_FRAME_STOP_SENDING_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_STOP_SENDING_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &type);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (type != UTP_FRAME_TYPE_STOP_SENDING) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u16(&reader, &decoded.error_code);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u32(&reader, &decoded.stream_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *stop = decoded;
    }
    return error;
}

static utp_internal_error_t utp_frame_streams_limit_encode(uint8_t* buffer, size_t capacity, uint8_t type,
                                                           const utp_frame_streams_limit_t* limit)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    /* type 由本文件的固定帧包装函数传入。 */
    if (limit->stream_type > UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_STREAMS_LIMIT_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, UTP_FRAME_STREAMS_LIMIT_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, limit->stream_type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u16(&writer, limit->stream_limit);
}

static utp_internal_error_t utp_frame_streams_limit_decode(utp_frame_streams_limit_t* limit, const uint8_t* buffer,
                                                           size_t length, uint8_t expected_type)
{
    utp_wire_reader_t         reader;
    utp_frame_streams_limit_t decoded;
    uint8_t                   type;
    utp_internal_error_t      error;

    /* expected_type 由本文件的固定帧包装函数传入。 */
    if (length < UTP_FRAME_STREAMS_LIMIT_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, UTP_FRAME_STREAMS_LIMIT_SIZE);
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
    error = utp_wire_read_u8(&reader, &decoded.stream_type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (decoded.stream_type > UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u16(&reader, &decoded.stream_limit);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *limit = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_streams_blocked_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_frame_streams_limit_t* blocked)
{
    return utp_frame_streams_limit_encode(buffer, capacity, UTP_FRAME_TYPE_STREAMS_BLOCKED, blocked);
}

utp_internal_error_t utp_frame_streams_blocked_decode(utp_frame_streams_limit_t* blocked, const uint8_t* buffer,
                                                      size_t length)
{
    return utp_frame_streams_limit_decode(blocked, buffer, length, UTP_FRAME_TYPE_STREAMS_BLOCKED);
}

utp_internal_error_t utp_frame_max_streams_encode(uint8_t* buffer, size_t capacity,
                                                  const utp_frame_streams_limit_t* maximum)
{
    return utp_frame_streams_limit_encode(buffer, capacity, UTP_FRAME_TYPE_MAX_STREAMS, maximum);
}

utp_internal_error_t utp_frame_max_streams_decode(utp_frame_streams_limit_t* maximum, const uint8_t* buffer,
                                                  size_t length)
{
    return utp_frame_streams_limit_decode(maximum, buffer, length, UTP_FRAME_TYPE_MAX_STREAMS);
}

static utp_internal_error_t utp_frame_u64_encode(uint8_t* buffer, size_t capacity, uint8_t type, uint64_t value,
                                                 size_t frame_size)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (capacity < frame_size) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, frame_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u64(&writer, value);
}

static utp_internal_error_t utp_frame_u64_decode(uint64_t* value, const uint8_t* buffer, size_t length, uint8_t type,
                                                 size_t frame_size)
{
    utp_wire_reader_t    reader;
    uint8_t              decoded_type;
    uint64_t             decoded_value;
    utp_internal_error_t error;

    if (length < frame_size) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, frame_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &decoded_type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (decoded_type != type) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u64(&reader, &decoded_value);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *value = decoded_value;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_frame_u32_u64_encode(uint8_t* buffer, size_t capacity, uint8_t type, uint32_t first,
                                                     uint64_t second, size_t frame_size)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (capacity < frame_size) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_writer_init(&writer, buffer, frame_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, first);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u64(&writer, second);
}

static utp_internal_error_t utp_frame_u32_u64_decode(uint32_t* first, uint64_t* second, const uint8_t* buffer,
                                                     size_t length, uint8_t type, size_t frame_size)
{
    utp_wire_reader_t    reader;
    uint8_t              decoded_type;
    uint32_t             decoded_first;
    uint64_t             decoded_second;
    utp_internal_error_t error;

    if (length < frame_size) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    error = utp_wire_reader_init(&reader, buffer, frame_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &decoded_type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (decoded_type != type) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_wire_read_u32(&reader, &decoded_first);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u64(&reader, &decoded_second);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *first  = decoded_first;
    *second = decoded_second;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_max_data_encode(uint8_t* buffer, size_t capacity, const utp_frame_max_data_t* max_data)
{
    return utp_frame_u64_encode(buffer, capacity, UTP_FRAME_TYPE_MAX_DATA, max_data->maximum_data,
                                UTP_FRAME_MAX_DATA_SIZE);
}

utp_internal_error_t utp_frame_max_data_decode(utp_frame_max_data_t* max_data, const uint8_t* buffer, size_t length)
{
    return utp_frame_u64_decode(&max_data->maximum_data, buffer, length, UTP_FRAME_TYPE_MAX_DATA,
                                UTP_FRAME_MAX_DATA_SIZE);
}

utp_internal_error_t utp_frame_max_stream_data_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_frame_max_stream_data_t* max_stream_data)
{
    return utp_frame_u32_u64_encode(buffer, capacity, UTP_FRAME_TYPE_MAX_STREAM_DATA, max_stream_data->stream_id,
                                    max_stream_data->maximum_stream_data, UTP_FRAME_MAX_STREAM_DATA_SIZE);
}

utp_internal_error_t utp_frame_max_stream_data_decode(utp_frame_max_stream_data_t* max_stream_data,
                                                      const uint8_t* buffer, size_t length)
{
    return utp_frame_u32_u64_decode(&max_stream_data->stream_id, &max_stream_data->maximum_stream_data, buffer, length,
                                    UTP_FRAME_TYPE_MAX_STREAM_DATA, UTP_FRAME_MAX_STREAM_DATA_SIZE);
}

utp_internal_error_t utp_frame_data_blocked_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_frame_data_blocked_t* blocked)
{
    return utp_frame_u64_encode(buffer, capacity, UTP_FRAME_TYPE_DATA_BLOCKED, blocked->data_limit,
                                UTP_FRAME_DATA_BLOCKED_SIZE);
}

utp_internal_error_t utp_frame_data_blocked_decode(utp_frame_data_blocked_t* blocked, const uint8_t* buffer,
                                                   size_t length)
{
    return utp_frame_u64_decode(&blocked->data_limit, buffer, length, UTP_FRAME_TYPE_DATA_BLOCKED,
                                UTP_FRAME_DATA_BLOCKED_SIZE);
}

utp_internal_error_t utp_frame_stream_data_blocked_encode(uint8_t* buffer, size_t capacity,
                                                          const utp_frame_stream_data_blocked_t* blocked)
{
    return utp_frame_u32_u64_encode(buffer, capacity, UTP_FRAME_TYPE_STREAM_DATA_BLOCKED, blocked->stream_id,
                                    blocked->stream_data_limit, UTP_FRAME_STREAM_DATA_BLOCKED_SIZE);
}

utp_internal_error_t utp_frame_stream_data_blocked_decode(utp_frame_stream_data_blocked_t* blocked,
                                                          const uint8_t* buffer, size_t length)
{
    return utp_frame_u32_u64_decode(&blocked->stream_id, &blocked->stream_data_limit, buffer, length,
                                    UTP_FRAME_TYPE_STREAM_DATA_BLOCKED, UTP_FRAME_STREAM_DATA_BLOCKED_SIZE);
}

utp_internal_error_t utp_frame_ack_frequency_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_frame_ack_frequency_t* frequency)
{
    utp_wire_writer_t    writer;
    uint8_t              threshold;
    uint8_t              reorder;
    uint32_t             delay;
    utp_internal_error_t error;

    if (capacity < UTP_FRAME_ACK_FREQUENCY_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    threshold = frequency->ack_eliciting_threshold == 0u ? 5u : frequency->ack_eliciting_threshold;
    reorder   = frequency->reordering_threshold == 0u ? 3u : frequency->reordering_threshold;
    delay     = frequency->max_ack_delay_ms == 0u ? 25u : frequency->max_ack_delay_ms;
    if (threshold > UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD)
        threshold = UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD;
    if (reorder > UTP_ACK_FREQUENCY_MAX_REORDERING_THRESHOLD) reorder = UTP_ACK_FREQUENCY_MAX_REORDERING_THRESHOLD;
    if (delay > UTP_ACK_FREQUENCY_MAX_DELAY_MS) delay = UTP_ACK_FREQUENCY_MAX_DELAY_MS;
    error = utp_wire_writer_init(&writer, buffer, UTP_FRAME_ACK_FREQUENCY_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_ACK_FREQUENCY);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u8(&writer, threshold);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u8(&writer, reorder);
    return error == UTP_INTERNAL_ERROR_OK ? utp_wire_write_u32(&writer, delay) : error;
}

utp_internal_error_t utp_frame_ack_frequency_decode(utp_frame_ack_frequency_t* frequency, const uint8_t* buffer,
                                                    size_t length)
{
    utp_wire_reader_t    reader;
    uint8_t              type;
    utp_internal_error_t error;

    if (length != UTP_FRAME_ACK_FREQUENCY_SIZE) return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u8(&reader, &type);
    if (error == UTP_INTERNAL_ERROR_OK && type != UTP_FRAME_TYPE_ACK_FREQUENCY) return UTP_INTERNAL_ERROR_PROTOCOL;
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u8(&reader, &frequency->ack_eliciting_threshold);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u8(&reader, &frequency->reordering_threshold);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u32(&reader, &frequency->max_ack_delay_ms);
    if (error != UTP_INTERNAL_ERROR_OK) return error;
    if (frequency->ack_eliciting_threshold == 0u) frequency->ack_eliciting_threshold = 5u;
    if (frequency->reordering_threshold == 0u) frequency->reordering_threshold = 3u;
    if (frequency->max_ack_delay_ms == 0u) frequency->max_ack_delay_ms = 25u;
    if (frequency->ack_eliciting_threshold > UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD)
        frequency->ack_eliciting_threshold = UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD;
    if (frequency->reordering_threshold > UTP_ACK_FREQUENCY_MAX_REORDERING_THRESHOLD)
        frequency->reordering_threshold = UTP_ACK_FREQUENCY_MAX_REORDERING_THRESHOLD;
    if (frequency->max_ack_delay_ms > UTP_ACK_FREQUENCY_MAX_DELAY_MS)
        frequency->max_ack_delay_ms = UTP_ACK_FREQUENCY_MAX_DELAY_MS;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_frame_transport_params_encode(uint8_t* buffer, size_t capacity,
                                                       const utp_frame_transport_params_t* params)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (capacity < UTP_FRAME_TRANSPORT_PARAMS_SIZE ||
        (params->flags & (uint16_t)~UTP_TRANSPORT_PARAMS_DEFAULT_FLAGS) != 0u ||
        params->ack_delay_exponent > UTP_TRANSPORT_PARAMS_MAX_ACK_EXPONENT ||
        params->initial_max_data > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL ||
        params->initial_max_stream_data_bidi_local > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL ||
        params->initial_max_stream_data_bidi_remote > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL)
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    error = utp_wire_writer_init(&writer, buffer, UTP_FRAME_TRANSPORT_PARAMS_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_TRANSPORT_PARAMS);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u16(&writer, params->flags);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u32(&writer, params->max_idle_timeout_ms);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u16(&writer, params->handshake_timeout_ms);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u16(&writer, params->initial_max_streams_bidi);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u16(&writer, params->initial_max_streams_uni);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u8(&writer, params->ack_delay_exponent);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u64(&writer, params->initial_max_data);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_write_u64(&writer, params->initial_max_stream_data_bidi_local);
    return error == UTP_INTERNAL_ERROR_OK ? utp_wire_write_u64(&writer, params->initial_max_stream_data_bidi_remote)
                                          : error;
}

utp_internal_error_t utp_frame_transport_params_decode(utp_frame_transport_params_t* params, const uint8_t* buffer,
                                                       size_t length)
{
    utp_wire_reader_t    reader;
    uint8_t              type;
    utp_internal_error_t error;

    if (length != UTP_FRAME_TRANSPORT_PARAMS_SIZE) return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u8(&reader, &type);
    if (error == UTP_INTERNAL_ERROR_OK && type != UTP_FRAME_TYPE_TRANSPORT_PARAMS) return UTP_INTERNAL_ERROR_PROTOCOL;
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u16(&reader, &params->flags);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u32(&reader, &params->max_idle_timeout_ms);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u16(&reader, &params->handshake_timeout_ms);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u16(&reader, &params->initial_max_streams_bidi);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u16(&reader, &params->initial_max_streams_uni);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u8(&reader, &params->ack_delay_exponent);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u64(&reader, &params->initial_max_data);
    if (error == UTP_INTERNAL_ERROR_OK) error = utp_wire_read_u64(&reader, &params->initial_max_stream_data_bidi_local);
    if (error == UTP_INTERNAL_ERROR_OK)
        error = utp_wire_read_u64(&reader, &params->initial_max_stream_data_bidi_remote);
    if (error != UTP_INTERNAL_ERROR_OK) return error;
    return (params->flags & (uint16_t)~UTP_TRANSPORT_PARAMS_DEFAULT_FLAGS) != 0u ||
                   params->ack_delay_exponent > UTP_TRANSPORT_PARAMS_MAX_ACK_EXPONENT ||
                   params->initial_max_data > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL ||
                   params->initial_max_stream_data_bidi_local > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL ||
                   params->initial_max_stream_data_bidi_remote > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL
               ? UTP_INTERNAL_ERROR_PROTOCOL
               : UTP_INTERNAL_ERROR_OK;
}
