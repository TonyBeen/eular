#include "nat/nat.h"

#include <string.h>

#include "crypto/crypto.h"
#include "proto/proto.h"
#include "proto/wire.h"

static utp_internal_error_t utp_nat_probe_write_tlv(utp_wire_writer_t* writer, uint16_t type, const uint8_t* value,
                                                    size_t value_length)
{
    utp_internal_error_t error;

    if (value_length > UINT16_MAX || (value == NULL && value_length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_write_u16(writer, type);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(writer, (uint16_t)value_length);
    }
    for (size_t index = 0u; error == UTP_INTERNAL_ERROR_OK && index < value_length; ++index) {
        error = utp_wire_write_u8(writer, value[index]);
    }
    return error;
}

static utp_internal_error_t utp_nat_probe_decode_endpoint(const uint8_t* value, size_t length, utp_address_t* endpoint)
{
    utp_wire_reader_t    reader;
    uint8_t              family;
    uint8_t              reserved;
    size_t               address_length;
    utp_address_t        decoded = {0};
    utp_internal_error_t error;

    if (value == NULL || endpoint == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, value, length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &reserved);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u16(&reader, &decoded.port);
    }
    if (error != UTP_INTERNAL_ERROR_OK || reserved != 0u || decoded.port == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (family == UTP_ADDRESS_FAMILY_IPV4) {
        address_length = 4u;
    } else if (family == UTP_ADDRESS_FAMILY_IPV6) {
        address_length = 16u;
    } else {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (reader.remaining != address_length) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    decoded.family = family;
    for (size_t index = 0u; index < address_length; ++index) {
        error = utp_wire_read_u8(&reader, &decoded.address[index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    *endpoint = decoded;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_nat_probe_task_reset(utp_nat_probe_task_t* task)
{
    *task                         = (utp_nat_probe_task_t){0};
    task->result.primary_rtt_ms   = -1;
    task->result.secondary_rtt_ms = -1;
}

uint8_t utp_nat_probe_request_message_type(uint8_t phase)
{
    return phase == UTP_NAT_PROBE_PHASE_PROBE1 || phase == UTP_NAT_PROBE_PHASE_PROBE2
               ? UTP_NAT_PROBE_MESSAGE_PROBE_REQ
               : UTP_NAT_PROBE_MESSAGE_FILTER_REQ;
}

uint8_t utp_nat_probe_response_message_type(uint8_t phase)
{
    return phase == UTP_NAT_PROBE_PHASE_PROBE1 || phase == UTP_NAT_PROBE_PHASE_PROBE2
               ? UTP_NAT_PROBE_MESSAGE_PROBE_RSP
               : UTP_NAT_PROBE_MESSAGE_FILTER_RSP;
}

utp_internal_error_t utp_nat_probe_encode_request(uint8_t packet[UTP_NAT_PROBE_PACKET_SIZE], uint64_t packet_number,
                                                  uint8_t message_type, uint8_t phase,
                                                  const uint8_t token[UTP_NAT_PROBE_TOKEN_SIZE])
{
    const utp_packet_header_t header = {0u,
                                        0u,
                                        packet_number,
                                        (uint16_t)(UTP_NAT_PROBE_PACKET_SIZE - UTP_PACKET_HEADER_SIZE),
                                        UTP_PACKET_TYPE_NAT_PROBE,
                                        0u};
    utp_wire_writer_t         writer;
    utp_internal_error_t      error;
    size_t                    padding_length;

    if (packet_number == 0u || token == NULL || message_type != utp_nat_probe_request_message_type(phase) ||
        phase < UTP_NAT_PROBE_PHASE_PROBE1 || phase > UTP_NAT_PROBE_PHASE_PROBE2) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_proto_encode_header(packet, UTP_NAT_PROBE_PACKET_SIZE, &header);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_writer_init(&writer, packet + UTP_PACKET_HEADER_SIZE,
                                 UTP_NAT_PROBE_PACKET_SIZE - UTP_PACKET_HEADER_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, UTP_NAT_PROBE_VERSION);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, message_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, phase);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, 0u);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_nat_probe_write_tlv(&writer, UTP_NAT_PROBE_TLV_PROBE_TOKEN, token, UTP_NAT_PROBE_TOKEN_SIZE);
    }
    if (error != UTP_INTERNAL_ERROR_OK || writer.remaining < 4u) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_OVERFLOW : error;
    }
    padding_length = writer.remaining - 4u;
    error          = utp_wire_write_u16(&writer, UTP_NAT_PROBE_TLV_PADDING);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, (uint16_t)padding_length);
    }
    for (size_t index = 0u; error == UTP_INTERNAL_ERROR_OK && index < padding_length; ++index) {
        error = utp_wire_write_u8(&writer, 0u);
    }
    return error == UTP_INTERNAL_ERROR_OK && writer.remaining == 0u ? UTP_INTERNAL_ERROR_OK
                                                                    : UTP_INTERNAL_ERROR_OVERFLOW;
}

utp_internal_error_t utp_nat_probe_decode_response(const uint8_t* payload, size_t payload_length,
                                                   utp_nat_probe_response_t* response)
{
    utp_wire_reader_t        reader;
    utp_nat_probe_response_t decoded    = {0};
    bool                     has_token  = false;
    bool                     has_mapped = false;
    bool                     has_origin = false;
    utp_internal_error_t     error;

    if (payload == NULL || response == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, payload, payload_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        uint8_t version;
        uint8_t flags;

        error = utp_wire_read_u8(&reader, &version);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u8(&reader, &decoded.message_type);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u8(&reader, &decoded.phase);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u8(&reader, &flags);
        }
        if (error != UTP_INTERNAL_ERROR_OK || version != UTP_NAT_PROBE_VERSION || flags != 0u ||
            decoded.phase < UTP_NAT_PROBE_PHASE_PROBE1 || decoded.phase > UTP_NAT_PROBE_PHASE_PROBE2 ||
            decoded.message_type != utp_nat_probe_response_message_type(decoded.phase)) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
    }
    while (error == UTP_INTERNAL_ERROR_OK && reader.remaining != 0u) {
        uint16_t       type;
        uint16_t       length;
        const uint8_t* value;

        error = utp_wire_read_u16(&reader, &type);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u16(&reader, &length);
        }
        if (error != UTP_INTERNAL_ERROR_OK || reader.remaining < (size_t)length) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        value             = reader.cursor;
        reader.cursor    += length;
        reader.remaining -= length;
        if (type == UTP_NAT_PROBE_TLV_PROBE_TOKEN && !has_token && length == UTP_NAT_PROBE_TOKEN_SIZE) {
            decoded.token        = value;
            decoded.token_length = length;
            has_token            = true;
        } else if (type == UTP_NAT_PROBE_TLV_MAPPED_ADDR && !has_mapped) {
            error      = utp_nat_probe_decode_endpoint(value, length, &decoded.mapped);
            has_mapped = error == UTP_INTERNAL_ERROR_OK;
        } else if (type == UTP_NAT_PROBE_TLV_ORIGIN_ADDR && !has_origin) {
            error      = utp_nat_probe_decode_endpoint(value, length, &decoded.origin);
            has_origin = error == UTP_INTERNAL_ERROR_OK;
        } else if (type == UTP_NAT_PROBE_TLV_ALTERNATE_PROBE_ENDPOINT && !decoded.has_alternate &&
                   decoded.phase == UTP_NAT_PROBE_PHASE_PROBE1) {
            error                 = utp_nat_probe_decode_endpoint(value, length, &decoded.alternate);
            decoded.has_alternate = error == UTP_INTERNAL_ERROR_OK;
        } else {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
    }
    if (error != UTP_INTERNAL_ERROR_OK || !has_token || !has_mapped || !has_origin) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    *response = decoded;
    return UTP_INTERNAL_ERROR_OK;
}
