#include "proto/proto.h"

#include "proto/wire.h"

static utp_internal_error_t utp_proto_validate_header(const utp_packet_header_t* header)
{
    if (header == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (header->packet_number > UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_proto_encode_header(uint8_t* buffer, size_t capacity, const utp_packet_header_t* header)
{
    if (buffer == NULL || header == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    utp_internal_error_t error = utp_proto_validate_header(header);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_wire_writer_t writer;
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, header->scid);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, header->dcid);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u64(&writer, header->packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, header->payload_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, header->type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_wire_write_u8(&writer, header->reserve);
}

utp_internal_error_t utp_proto_decode_header(utp_packet_header_t* header, const uint8_t* buffer, size_t length)
{
    if (header == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    utp_packet_header_t  decoded;
    utp_wire_reader_t    reader;
    utp_internal_error_t error = utp_wire_reader_init(&reader, buffer, length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u32(&reader, &decoded.scid);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u32(&reader, &decoded.dcid);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u64(&reader, &decoded.packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u16(&reader, &decoded.payload_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &decoded.type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &decoded.reserve);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_proto_validate_header(&decoded);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *header = decoded;
    return UTP_INTERNAL_ERROR_OK;
}
