#include "proto/ack.h"

#include <limits.h>

#include "proto/frame.h"
#include "proto/proto.h"
#include "proto/wire.h"

utp_internal_error_t utp_ack_from_receive_history(utp_ack_info_t* ack, const utp_receive_history_t* history,
                                                  uint64_t now, size_t max_ranges)
{
    if (ack == NULL || history == NULL || max_ranges == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (max_ranges > UTP_ACK_MAX_RANGES) {
        max_ranges = UTP_ACK_MAX_RANGES;
    }
    size_t range_count = utp_receive_history_range_count(history);
    if (range_count > max_ranges) {
        range_count = max_ranges;
    }
    if (range_count != 0u && (ack->ranges == NULL || ack->range_capacity < range_count)) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }

    if (range_count == 0u) {
        ack->largest_acked = 0u;
        ack->ack_delay     = 0u;
        ack->range_count   = 0u;
        return UTP_INTERNAL_ERROR_OK;
    }
    for (size_t index = 0u; index < range_count; ++index) {
        const utp_receive_range_t* range = utp_receive_history_range_at(history, index);

        if (range == NULL) {
            return UTP_INTERNAL_ERROR_STATE;
        }
        ack->ranges[index].low  = range->low;
        ack->ranges[index].high = range->high;
    }
    uint64_t largest_received_at = utp_receive_history_largest_received_at(history);
    ack->largest_acked           = utp_receive_history_largest(history);
    ack->ack_delay               = now >= largest_received_at ? now - largest_received_at : 0u;
    ack->range_count             = range_count;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_ack_range_length(const utp_ack_range_t* range, uint32_t* length)
{
    if (range == NULL || length == NULL || range->low > range->high || range->high > UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    uint64_t difference = range->high - range->low;
    if (difference >= UINT32_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *length = (uint32_t)(difference + 1u);
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_ack_validate_encode_input(const utp_ack_info_t* ack)
{
    uint32_t range_length;

    if (ack == NULL || ack->range_count > UTP_ACK_MAX_RANGES || ack->range_count > ack->range_capacity) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (ack->range_count == 0u) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (ack->ranges == NULL || utp_ack_range_length(&ack->ranges[0], &range_length) != UTP_INTERNAL_ERROR_OK ||
        ack->largest_acked != ack->ranges[0].high) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    uint64_t previous_low = ack->ranges[0].low;
    for (size_t index = 1u; index < ack->range_count; ++index) {
        if (utp_ack_range_length(&ack->ranges[index], &range_length) != UTP_INTERNAL_ERROR_OK ||
            previous_low <= ack->ranges[index].high || previous_low - ack->ranges[index].high - 1u > UINT32_MAX) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        previous_low = ack->ranges[index].low;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_ack_encode(uint8_t* buffer, size_t capacity, const utp_ack_info_t* ack,
                                    uint8_t ack_delay_exponent, size_t* encoded_length)
{
    if (buffer == NULL || ack == NULL || encoded_length == NULL || ack_delay_exponent > UTP_ACK_MAX_DELAY_EXPONENT) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_internal_error_t error = utp_ack_validate_encode_input(ack);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (ack->range_count == 0u) {
        *encoded_length = 0u;
        return UTP_INTERNAL_ERROR_OK;
    }
    size_t frame_length = UTP_ACK_FRAME_HEADER_SIZE + (ack->range_count - 1u) * UTP_ACK_FRAME_RANGE_SIZE;
    if (capacity < frame_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    utp_wire_writer_t writer;
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    uint64_t encoded_delay = ack->ack_delay >> ack_delay_exponent;
    if (encoded_delay > UINT16_MAX) {
        encoded_delay = UINT16_MAX;
    }
    uint32_t range_length;
    error = utp_ack_range_length(&ack->ranges[0], &range_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, UTP_FRAME_TYPE_ACK);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u8(&writer, (uint8_t)(ack->range_count - 1u));
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u16(&writer, (uint16_t)encoded_delay);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u32(&writer, range_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_write_u64(&writer, ack->largest_acked);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    uint64_t previous_low = ack->ranges[0].low;
    for (size_t index = 1u; index < ack->range_count; ++index) {
        uint32_t gap = (uint32_t)(previous_low - ack->ranges[index].high - 1u);

        error = utp_ack_range_length(&ack->ranges[index], &range_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        error = utp_wire_write_u32(&writer, gap);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        error = utp_wire_write_u32(&writer, range_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        previous_low = ack->ranges[index].low;
    }
    *encoded_length = frame_length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_ack_decode(utp_ack_info_t* ack, const uint8_t* frame, size_t frame_length,
                                    uint8_t ack_delay_exponent, size_t* consumed)
{
    uint32_t gaps[UTP_ACK_MAX_RANGES - 1u];
    uint32_t range_lengths[UTP_ACK_MAX_RANGES - 1u];

    if (ack == NULL || frame == NULL || consumed == NULL || ack_delay_exponent > UTP_ACK_MAX_DELAY_EXPONENT ||
        ack->ranges == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (frame_length < UTP_ACK_FRAME_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    if (frame[0] != UTP_FRAME_TYPE_ACK) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    uint8_t additional_range_count = frame[1];
    size_t  range_count            = (size_t)additional_range_count + 1u;
    size_t  expected_length = UTP_ACK_FRAME_HEADER_SIZE + (size_t)additional_range_count * UTP_ACK_FRAME_RANGE_SIZE;
    if (frame_length < expected_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    if (range_count > ack->range_capacity) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    utp_wire_reader_t    reader;
    utp_internal_error_t error = utp_wire_reader_init(&reader, frame, expected_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    uint8_t frame_type;
    error = utp_wire_read_u8(&reader, &frame_type);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_read_u8(&reader, &additional_range_count);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    uint16_t encoded_delay;
    error = utp_wire_read_u16(&reader, &encoded_delay);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    uint32_t first_range_length;
    error = utp_wire_read_u32(&reader, &first_range_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    uint64_t largest_acked;
    error = utp_wire_read_u64(&reader, &largest_acked);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (first_range_length == 0u || largest_acked > UTP_PACKET_NUMBER_MAX ||
        largest_acked < (uint64_t)first_range_length - 1u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    uint64_t first_low    = largest_acked - (uint64_t)first_range_length + 1u;
    uint64_t previous_low = first_low;
    for (size_t index = 1u; index < range_count; ++index) {
        uint32_t gap;
        uint32_t range_length;
        uint64_t range_high;

        error = utp_wire_read_u32(&reader, &gap);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        error = utp_wire_read_u32(&reader, &range_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (range_length == 0u || previous_low <= (uint64_t)gap) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        range_high = previous_low - (uint64_t)gap - 1u;
        if (range_high < (uint64_t)range_length - 1u) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        gaps[index - 1u]          = gap;
        range_lengths[index - 1u] = range_length;
        previous_low              = range_high - (uint64_t)range_length + 1u;
    }
    ack->ranges[0].low  = first_low;
    ack->ranges[0].high = largest_acked;
    previous_low        = first_low;
    for (size_t index = 1u; index < range_count; ++index) {
        uint64_t range_high;

        range_high              = previous_low - (uint64_t)gaps[index - 1u] - 1u;
        ack->ranges[index].high = range_high;
        ack->ranges[index].low  = range_high - (uint64_t)range_lengths[index - 1u] + 1u;
        previous_low            = ack->ranges[index].low;
    }
    ack->largest_acked = largest_acked;
    ack->ack_delay     = (uint64_t)encoded_delay << ack_delay_exponent;
    ack->range_count   = range_count;
    *consumed          = expected_length;
    return UTP_INTERNAL_ERROR_OK;
}
