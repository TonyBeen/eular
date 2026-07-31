#include "connection/stream.h"

#include <string.h>

static uint64_t utp_stream_fragment_end(const utp_stream_recv_fragment_t *fragment) {
    return fragment->offset + (uint64_t)fragment->length;
}

static size_t utp_stream_send_index(const utp_stream_t *stream, size_t offset) {
    return (stream->send_buffer_start + offset) % UTP_STREAM_SEND_BUFFER_CAPACITY;
}

static void utp_stream_copy_into_send_buffer(utp_stream_t *stream, const uint8_t *data, size_t length) {
    size_t offset = stream->send_buffer_length;

    while (length != 0u) {
        size_t index   = utp_stream_send_index(stream, offset);
        size_t segment = UTP_STREAM_SEND_BUFFER_CAPACITY - index;

        if (segment > length) {
            segment = length;
        }
        memcpy(stream->send_buffer + index, data, segment);
        offset += segment;
        data   += segment;
        length -= segment;
    }
}

static const uint8_t *utp_stream_unsent_data(const utp_stream_t *stream, size_t *contiguous_length) {
    size_t unsent_length;
    size_t index;
    size_t contiguous;

    if (contiguous_length == NULL || stream->send_buffer_length < stream->send_in_flight_bytes) {
        return NULL;
    }
    unsent_length = stream->send_buffer_length - stream->send_in_flight_bytes;
    if (unsent_length == 0u) {
        *contiguous_length = 0u;
        return NULL;
    }
    index      = utp_stream_send_index(stream, stream->send_in_flight_bytes);
    contiguous = UTP_STREAM_SEND_BUFFER_CAPACITY - index;
    if (contiguous > unsent_length) {
        contiguous = unsent_length;
    }
    *contiguous_length = contiguous;
    return stream->send_buffer + index;
}

static void utp_stream_pop_send_prefix(utp_stream_t *stream, size_t length) {
    stream->send_buffer_start     = utp_stream_send_index(stream, length);
    stream->send_buffer_offset   += (uint64_t)length;
    stream->send_buffer_length   -= length;
    stream->send_in_flight_bytes -= length;
}

static void utp_stream_remove_ack_range(utp_stream_t *stream, size_t index) {
    if (index + 1u < stream->send_ack_range_count) {
        memmove(&stream->send_ack_ranges[index], &stream->send_ack_ranges[index + 1u],
                (stream->send_ack_range_count - index - 1u) * sizeof(stream->send_ack_ranges[0]));
    }
    --stream->send_ack_range_count;
}

static utp_internal_error_t utp_stream_insert_ack_range(utp_stream_t *stream, uint64_t start, uint64_t end) {
    size_t index = 0u;

    while (index < stream->send_ack_range_count && stream->send_ack_ranges[index].end < start) {
        ++index;
    }
    while (index < stream->send_ack_range_count && stream->send_ack_ranges[index].start <= end) {
        if (stream->send_ack_ranges[index].start < start) {
            start = stream->send_ack_ranges[index].start;
        }
        if (stream->send_ack_ranges[index].end > end) {
            end = stream->send_ack_ranges[index].end;
        }
        utp_stream_remove_ack_range(stream, index);
    }
    if (stream->send_ack_range_count >= UTP_STREAM_SEND_ACK_RANGE_LIMIT) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    if (index < stream->send_ack_range_count) {
        memmove(&stream->send_ack_ranges[index + 1u], &stream->send_ack_ranges[index],
                (stream->send_ack_range_count - index) * sizeof(stream->send_ack_ranges[0]));
    }
    stream->send_ack_ranges[index].start = start;
    stream->send_ack_ranges[index].end   = end;
    ++stream->send_ack_range_count;
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_stream_release_acked_prefix(utp_stream_t *stream) {
    while (stream->send_ack_range_count != 0u && stream->send_ack_ranges[0].start <= stream->send_buffer_offset &&
           stream->send_ack_ranges[0].end > stream->send_buffer_offset) {
        size_t length = (size_t)(stream->send_ack_ranges[0].end - stream->send_buffer_offset);

        if (length > stream->send_in_flight_bytes) {
            length = stream->send_in_flight_bytes;
        }
        if (length > stream->send_buffer_length) {
            length = stream->send_buffer_length;
        }
        if (length == 0u) {
            return;
        }
        utp_stream_pop_send_prefix(stream, length);
        utp_stream_remove_ack_range(stream, 0u);
    }
}

static void utp_stream_remove_fragment(utp_stream_t *stream, size_t index) {
    if (index + 1u < stream->recv_fragment_count) {
        memmove(&stream->recv_fragments[index], &stream->recv_fragments[index + 1u],
                (stream->recv_fragment_count - index - 1u) * sizeof(stream->recv_fragments[0]));
    }
    --stream->recv_fragment_count;
}

static utp_internal_error_t utp_stream_insert_fragment(utp_stream_t *stream, uint64_t offset, const uint8_t *data,
                                                       size_t length, bool fin) {
    size_t index = 0u;

    if (length > UTP_STREAM_RECV_FRAGMENT_CAP || stream->recv_fragment_count >= UTP_STREAM_RECV_FRAGMENT_LIMIT ||
        length > UTP_STREAM_MAX_RECV_BUFFER_BYTES - stream->recv_buffered_bytes) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    while (index < stream->recv_fragment_count && stream->recv_fragments[index].offset < offset) {
        ++index;
    }
    if (index < stream->recv_fragment_count) {
        memmove(&stream->recv_fragments[index + 1u], &stream->recv_fragments[index],
                (stream->recv_fragment_count - index) * sizeof(stream->recv_fragments[0]));
    }
    stream->recv_fragments[index].offset   = offset;
    stream->recv_fragments[index].length   = length;
    stream->recv_fragments[index].consumed = 0u;
    stream->recv_fragments[index].fin      = fin;
    if (length != 0u) {
        memcpy(stream->recv_fragments[index].data, data, length);
        stream->recv_buffered_bytes += length;
    }
    ++stream->recv_fragment_count;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_stream_mark_fin(utp_stream_t *stream, uint64_t fin_offset) {
    size_t index;

    if (fin_offset == stream->recv_offset) {
        stream->peer_fin = true;
        return UTP_INTERNAL_ERROR_OK;
    }
    for (index = 0u; index < stream->recv_fragment_count; ++index) {
        if (utp_stream_fragment_end(&stream->recv_fragments[index]) == fin_offset) {
            stream->recv_fragments[index].fin = true;
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    return utp_stream_insert_fragment(stream, fin_offset, NULL, 0u, true);
}

void utp_stream_init(utp_stream_t *stream, uint32_t stream_id) {
    if (stream != NULL) {
        stream->send_buffer_offset               = 0u;
        stream->next_send_offset                 = 0u;
        stream->recv_offset                      = 0u;
        stream->stream_id                        = stream_id;
        stream->peer_max_stream_data             = UTP_STREAM_DEFAULT_FLOW_WINDOW;
        stream->local_max_stream_data_advertised = UTP_STREAM_DEFAULT_FLOW_WINDOW;
        stream->send_buffer_length               = 0u;
        stream->send_buffer_start                = 0u;
        stream->send_in_flight_bytes             = 0u;
        stream->recv_buffered_bytes              = 0u;
        stream->recv_fragment_count              = 0u;
        stream->send_ack_range_count             = 0u;
        stream->used                             = true;
        stream->local_fin_queued                 = false;
        stream->local_fin_sent                   = false;
        stream->peer_fin                         = false;
        stream->reset                            = false;
    }
}

void utp_stream_reset(utp_stream_t *stream) {
    if (stream != NULL) {
        stream->send_buffer_length   = 0u;
        stream->send_buffer_start    = 0u;
        stream->send_in_flight_bytes = 0u;
        stream->send_ack_range_count = 0u;
        stream->recv_buffered_bytes  = 0u;
        stream->recv_fragment_count  = 0u;
        stream->local_fin_queued     = true;
        stream->local_fin_sent       = true;
        stream->peer_fin             = true;
        stream->reset                = true;
    }
}

utp_internal_error_t utp_stream_write(utp_stream_t *stream, const uint8_t *data, size_t length, bool fin) {
    if (stream == NULL || !stream->used || stream->reset || stream->local_fin_queued ||
        (data == NULL && length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->send_buffer_length > sizeof(stream->send_buffer) ||
        length > sizeof(stream->send_buffer) - stream->send_buffer_length) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    if (length != 0u) {
        utp_stream_copy_into_send_buffer(stream, data, length);
        stream->send_buffer_length += length;
    }
    if (fin) {
        stream->local_fin_queued = true;
    }
    return UTP_INTERNAL_ERROR_OK;
}

bool utp_stream_has_send_work(const utp_stream_t *stream) {
    return stream != NULL && stream->used && !stream->reset &&
           (stream->send_buffer_length != stream->send_in_flight_bytes ||
            (stream->local_fin_queued && !stream->local_fin_sent));
}

utp_internal_error_t utp_stream_build_frame(utp_stream_t *stream, uint8_t *payload, size_t capacity,
                                            size_t *out_payload_length, uint32_t *out_stream_data_size,
                                            uint64_t *out_stream_offset, bool *out_fin) {
    utp_frame_stream_t   frame;
    size_t               data_length;
    size_t               contiguous_length;
    const uint8_t       *data;
    utp_internal_error_t error;

    if (stream == NULL || payload == NULL || out_payload_length == NULL || out_stream_data_size == NULL ||
        out_stream_offset == NULL || out_fin == NULL || !utp_stream_has_send_work(stream)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity <= UTP_FRAME_STREAM_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    data        = utp_stream_unsent_data(stream, &contiguous_length);
    data_length = contiguous_length;
    if (data_length > capacity - UTP_FRAME_STREAM_HEADER_SIZE) {
        data_length = capacity - UTP_FRAME_STREAM_HEADER_SIZE;
    }
    if (data_length > UINT16_MAX) {
        data_length = UINT16_MAX;
    }
    if (stream->next_send_offset > stream->peer_max_stream_data) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    if ((uint64_t)data_length > stream->peer_max_stream_data - stream->next_send_offset) {
        data_length = (size_t)(stream->peer_max_stream_data - stream->next_send_offset);
    }
    if (data_length == 0u && stream->send_buffer_length != stream->send_in_flight_bytes) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    frame.flags       = UTP_STREAM_FLAG_NONE;
    frame.stream_id   = stream->stream_id;
    frame.offset      = stream->next_send_offset;
    frame.data        = data;
    frame.data_length = (uint16_t)data_length;
    if (data_length == stream->send_buffer_length - stream->send_in_flight_bytes && stream->local_fin_queued &&
        !stream->local_fin_sent) {
        frame.flags |= UTP_STREAM_FLAG_FIN;
    }
    error = utp_frame_stream_encode(payload, capacity, &frame);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *out_payload_length   = UTP_FRAME_STREAM_HEADER_SIZE + data_length;
    *out_stream_data_size = (uint32_t)data_length;
    *out_stream_offset    = frame.offset;
    *out_fin              = (frame.flags & UTP_STREAM_FLAG_FIN) != 0u;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_build_frame_view(utp_stream_t *stream, uint8_t *header, size_t capacity,
                                                 size_t *out_header_length, const uint8_t **out_data,
                                                 uint32_t *out_stream_data_size, uint64_t *out_stream_offset,
                                                 bool *out_fin) {
    size_t               data_length;
    size_t               contiguous_length;
    uint8_t              flags = UTP_STREAM_FLAG_NONE;
    const uint8_t       *data;
    utp_internal_error_t error;

    if (stream == NULL || header == NULL || out_header_length == NULL || out_data == NULL ||
        out_stream_data_size == NULL || out_stream_offset == NULL || out_fin == NULL ||
        !utp_stream_has_send_work(stream)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_STREAM_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    data        = utp_stream_unsent_data(stream, &contiguous_length);
    data_length = contiguous_length;
    if (data_length > UINT16_MAX) {
        data_length = UINT16_MAX;
    }
    if (stream->next_send_offset > stream->peer_max_stream_data) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    if ((uint64_t)data_length > stream->peer_max_stream_data - stream->next_send_offset) {
        data_length = (size_t)(stream->peer_max_stream_data - stream->next_send_offset);
    }
    if (data_length == 0u && stream->send_buffer_length != stream->send_in_flight_bytes) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    if (data_length == stream->send_buffer_length - stream->send_in_flight_bytes && stream->local_fin_queued &&
        !stream->local_fin_sent) {
        flags |= UTP_STREAM_FLAG_FIN;
    }
    error = utp_frame_stream_header_encode(header, capacity, flags, stream->stream_id, stream->next_send_offset,
                                           (uint16_t)data_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *out_header_length    = UTP_FRAME_STREAM_HEADER_SIZE;
    *out_data             = data;
    *out_stream_data_size = (uint32_t)data_length;
    *out_stream_offset    = stream->next_send_offset;
    *out_fin              = (flags & UTP_STREAM_FLAG_FIN) != 0u;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_commit_built_frame(utp_stream_t *stream, uint32_t stream_data_size, bool fin) {
    size_t data_size = (size_t)stream_data_size;

    if (stream == NULL || !stream->used || data_size > stream->send_buffer_length - stream->send_in_flight_bytes ||
        (fin && !stream->local_fin_queued)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream->next_send_offset     += data_size;
    stream->send_in_flight_bytes += data_size;
    if (fin) {
        stream->local_fin_sent = true;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_on_packet_acked(utp_stream_t *stream, uint32_t stream_data_size) {
    return stream == NULL ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT
                          : utp_stream_on_packet_acked_range(stream, stream->send_buffer_offset, stream_data_size);
}

utp_internal_error_t utp_stream_on_packet_acked_range(utp_stream_t *stream, uint64_t stream_offset,
                                                      uint32_t stream_data_size) {
    size_t               data_size = (size_t)stream_data_size;
    uint64_t             end;
    utp_internal_error_t error;

    if (stream == NULL || !stream->used || data_size > stream->send_in_flight_bytes ||
        (uint64_t)data_size > UINT64_MAX - stream_offset) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    end = stream_offset + (uint64_t)data_size;
    if (stream_offset < stream->send_buffer_offset ||
        end > stream->send_buffer_offset + (uint64_t)stream->send_buffer_length) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (data_size == 0u) {
        return UTP_INTERNAL_ERROR_OK;
    }
    error = utp_stream_insert_ack_range(stream, stream_offset, end);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_stream_release_acked_prefix(stream);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_stream_update_peer_max_stream_data(utp_stream_t *stream, uint64_t maximum_stream_data) {
    if (stream != NULL && stream->used && maximum_stream_data > stream->peer_max_stream_data) {
        stream->peer_max_stream_data = maximum_stream_data;
    }
}

utp_internal_error_t utp_stream_on_frame(utp_stream_t *stream, const utp_frame_stream_t *frame) {
    uint64_t             original_end;
    uint64_t             start;
    uint64_t             end;
    size_t               data_index;
    size_t               index;
    utp_internal_error_t error;

    if (stream == NULL || frame == NULL || !stream->used || stream->reset || frame->stream_id != stream->stream_id ||
        (frame->flags & (uint8_t)~UTP_STREAM_FLAG_FIN) != 0u ||
        (frame->data_length == 0u && (frame->flags & UTP_STREAM_FLAG_FIN) == 0u) ||
        (frame->data == NULL && frame->data_length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((uint64_t)frame->data_length > UINT64_MAX - frame->offset) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    original_end = frame->offset + (uint64_t)frame->data_length;
    if (original_end > stream->local_max_stream_data_advertised) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    if (original_end < stream->recv_offset) {
        return UTP_INTERNAL_ERROR_OK;
    }
    start      = frame->offset;
    data_index = 0u;
    if (start < stream->recv_offset) {
        data_index = (size_t)(stream->recv_offset - start);
        start      = stream->recv_offset;
    }
    end = original_end;
    for (index = 0u; index < stream->recv_fragment_count && start < end; ++index) {
        uint64_t fragment_start = stream->recv_fragments[index].offset;
        uint64_t fragment_end   = utp_stream_fragment_end(&stream->recv_fragments[index]);

        if (fragment_end <= start) {
            continue;
        }
        if (fragment_start > start) {
            uint64_t gap_end = fragment_start < end ? fragment_start : end;
            size_t   gap_len = (size_t)(gap_end - start);

            error = utp_stream_insert_fragment(stream, start, frame->data + data_index, gap_len, false);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            data_index += gap_len;
            start       = gap_end;
            ++index;
        }
        if (fragment_end > start) {
            data_index += (size_t)(fragment_end - start);
            start       = fragment_end;
        }
    }
    if (start < end) {
        error = utp_stream_insert_fragment(stream, start, frame->data + data_index, (size_t)(end - start), false);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if ((frame->flags & UTP_STREAM_FLAG_FIN) != 0u) {
        return utp_stream_mark_fin(stream, original_end);
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_read(utp_stream_t *stream, uint8_t *buffer, size_t capacity, size_t *out_length,
                                     bool *out_fin) {
    size_t copied = 0u;

    if (stream == NULL || buffer == NULL || out_length == NULL || out_fin == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    while (copied < capacity && stream->recv_fragment_count != 0u &&
           stream->recv_fragments[0].offset == stream->recv_offset) {
        utp_stream_recv_fragment_t *fragment = &stream->recv_fragments[0];
        size_t                      remaining;
        size_t                      to_copy;

        remaining = fragment->length - fragment->consumed;
        if (remaining == 0u) {
            if (fragment->fin) {
                stream->peer_fin = true;
            }
            utp_stream_remove_fragment(stream, 0u);
            continue;
        }
        to_copy = remaining < capacity - copied ? remaining : capacity - copied;
        memcpy(buffer + copied, fragment->data + fragment->consumed, to_copy);
        fragment->consumed          += to_copy;
        stream->recv_offset         += to_copy;
        stream->recv_buffered_bytes -= to_copy;
        copied                      += to_copy;
        if (fragment->consumed == fragment->length) {
            if (fragment->fin) {
                stream->peer_fin = true;
            }
            utp_stream_remove_fragment(stream, 0u);
        }
    }
    *out_length = copied;
    *out_fin    = copied == 0u && stream->peer_fin;
    return copied == 0u && !stream->peer_fin ? UTP_INTERNAL_ERROR_WOULD_BLOCK : UTP_INTERNAL_ERROR_OK;
}

size_t utp_stream_readable_bytes(const utp_stream_t *stream) {
    size_t   readable = 0u;
    uint64_t offset;
    size_t   index;

    if (stream == NULL || !stream->used) {
        return 0u;
    }
    offset = stream->recv_offset;
    for (index = 0u; index < stream->recv_fragment_count; ++index) {
        const utp_stream_recv_fragment_t *fragment = &stream->recv_fragments[index];
        size_t                            remaining;

        if (fragment->offset != offset) {
            break;
        }
        remaining  = fragment->length - fragment->consumed;
        readable  += remaining;
        offset    += remaining;
        if (fragment->fin) {
            break;
        }
    }
    return readable;
}

size_t utp_stream_send_in_flight_bytes(const utp_stream_t *stream) {
    return stream == NULL || !stream->used ? 0u : stream->send_in_flight_bytes;
}

bool utp_stream_is_closed(const utp_stream_t *stream) {
    return stream != NULL && stream->used && stream->local_fin_queued && stream->local_fin_sent && stream->peer_fin;
}
