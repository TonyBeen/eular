#ifndef EULAR_UTP_INTERNAL_WIRE_H
#define EULAR_UTP_INTERNAL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_wire_reader {
    const uint8_t* cursor;     // 下一读取位置
    size_t         remaining;  // 剩余可读取字节数
} utp_wire_reader_t;

typedef struct utp_wire_writer {
    uint8_t* cursor;     // 下一写入位置
    size_t   remaining;  // 剩余可写入字节数
} utp_wire_writer_t;

static inline utp_internal_error_t utp_wire_reader_init(utp_wire_reader_t* reader, const uint8_t* buffer, size_t length)
{
    if (reader == NULL || (buffer == NULL && length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    reader->cursor    = buffer;
    reader->remaining = length;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_writer_init(utp_wire_writer_t* writer, uint8_t* buffer, size_t capacity)
{
    if (writer == NULL || (buffer == NULL && capacity != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    writer->cursor    = buffer;
    writer->remaining = capacity;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_write_u8(utp_wire_writer_t* writer, uint8_t value)
{
    if (writer == NULL || writer->remaining < 1u) {
        return writer == NULL ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT : UTP_INTERNAL_ERROR_OVERFLOW;
    }
    writer->cursor[0]  = value;
    writer->cursor    += 1u;
    writer->remaining -= 1u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_write_u16(utp_wire_writer_t* writer, uint16_t value)
{
    if (writer == NULL || writer->remaining < 2u) {
        return writer == NULL ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT : UTP_INTERNAL_ERROR_OVERFLOW;
    }
    writer->cursor[0]  = (uint8_t)(value >> 8u);
    writer->cursor[1]  = (uint8_t)value;
    writer->cursor    += 2u;
    writer->remaining -= 2u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_write_u32(utp_wire_writer_t* writer, uint32_t value)
{
    if (writer == NULL || writer->remaining < 4u) {
        return writer == NULL ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT : UTP_INTERNAL_ERROR_OVERFLOW;
    }
    writer->cursor[0]  = (uint8_t)(value >> 24u);
    writer->cursor[1]  = (uint8_t)(value >> 16u);
    writer->cursor[2]  = (uint8_t)(value >> 8u);
    writer->cursor[3]  = (uint8_t)value;
    writer->cursor    += 4u;
    writer->remaining -= 4u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_write_u64(utp_wire_writer_t* writer, uint64_t value)
{
    if (writer == NULL || writer->remaining < 8u) {
        return writer == NULL ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT : UTP_INTERNAL_ERROR_OVERFLOW;
    }
    writer->cursor[0]  = (uint8_t)(value >> 56u);
    writer->cursor[1]  = (uint8_t)(value >> 48u);
    writer->cursor[2]  = (uint8_t)(value >> 40u);
    writer->cursor[3]  = (uint8_t)(value >> 32u);
    writer->cursor[4]  = (uint8_t)(value >> 24u);
    writer->cursor[5]  = (uint8_t)(value >> 16u);
    writer->cursor[6]  = (uint8_t)(value >> 8u);
    writer->cursor[7]  = (uint8_t)value;
    writer->cursor    += 8u;
    writer->remaining -= 8u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_read_u8(utp_wire_reader_t* reader, uint8_t* value)
{
    if (reader == NULL || value == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (reader->remaining < 1u) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *value             = reader->cursor[0];
    reader->cursor    += 1u;
    reader->remaining -= 1u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_read_u16(utp_wire_reader_t* reader, uint16_t* value)
{
    if (reader == NULL || value == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (reader->remaining < 2u) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *value             = (uint16_t)(((uint16_t)reader->cursor[0] << 8u) | reader->cursor[1]);
    reader->cursor    += 2u;
    reader->remaining -= 2u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_read_u32(utp_wire_reader_t* reader, uint32_t* value)
{
    if (reader == NULL || value == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (reader->remaining < 4u) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *value = ((uint32_t)reader->cursor[0] << 24u) | ((uint32_t)reader->cursor[1] << 16u) |
             ((uint32_t)reader->cursor[2] << 8u) | reader->cursor[3];
    reader->cursor    += 4u;
    reader->remaining -= 4u;
    return UTP_INTERNAL_ERROR_OK;
}

static inline utp_internal_error_t utp_wire_read_u64(utp_wire_reader_t* reader, uint64_t* value)
{
    if (reader == NULL || value == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (reader->remaining < 8u) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *value = ((uint64_t)reader->cursor[0] << 56u) | ((uint64_t)reader->cursor[1] << 48u) |
             ((uint64_t)reader->cursor[2] << 40u) | ((uint64_t)reader->cursor[3] << 32u) |
             ((uint64_t)reader->cursor[4] << 24u) | ((uint64_t)reader->cursor[5] << 16u) |
             ((uint64_t)reader->cursor[6] << 8u) | reader->cursor[7];
    reader->cursor    += 8u;
    reader->remaining -= 8u;
    return UTP_INTERNAL_ERROR_OK;
}

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_WIRE_H
