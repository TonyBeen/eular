#include "connection/stream.h"

#include <string.h>

static uint64_t utp_stream_fragment_end(const utp_stream_recv_fragment_t* fragment)
{
    return fragment->offset + (uint64_t)fragment->length;
}

static uint64_t utp_stream_fragment_read_offset(const utp_stream_recv_fragment_t* fragment)
{
    return fragment->offset + (uint64_t)fragment->consumed;
}

static size_t utp_stream_send_index(const utp_stream_t* stream, size_t offset)
{
    return (stream->send_buffer_start + offset) % UTP_STREAM_SEND_BUFFER_CAPACITY;
}

static void utp_stream_copy_into_send_buffer(utp_stream_t* stream, const uint8_t* data, size_t length)
{
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

static const uint8_t* utp_stream_unsent_data(const utp_stream_t* stream, size_t* contiguous_length)
{
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

static void utp_stream_pop_send_prefix(utp_stream_t* stream, size_t length)
{
    stream->send_buffer_start     = utp_stream_send_index(stream, length);
    stream->send_buffer_offset   += (uint64_t)length;
    stream->send_buffer_length   -= length;
    stream->send_in_flight_bytes -= length;
}

static void utp_stream_remove_ack_range(utp_stream_t* stream, size_t index)
{
    if (index + 1u < stream->send_ack_range_count) {
        memmove(&stream->send_ack_ranges[index], &stream->send_ack_ranges[index + 1u],
                (stream->send_ack_range_count - index - 1u) * sizeof(stream->send_ack_ranges[0]));
    }
    --stream->send_ack_range_count;
}

static size_t utp_stream_recv_fragment_memory_cost(const utp_stream_recv_fragment_t* fragment)
{
    if (fragment == NULL) {
        return 0u;
    }
    return sizeof(*fragment) +
           (fragment->packet != NULL && fragment->length != 0u ? (size_t)fragment->packet->capacity : 0u);
}

static bool utp_stream_account_recv_fragment(utp_stream_t* stream, utp_stream_recv_fragment_t* fragment,
                                             const utp_stream_recv_account_t* account)
{
    size_t stream_memory_limit   = UTP_STREAM_RECV_REASSEMBLY_MEMORY_LIMIT;
    size_t stream_fragment_limit = UTP_STREAM_RECV_FRAGMENT_LIMIT;
    size_t connection_memory_limit;
    size_t connection_fragment_limit;
    size_t new_cost;

    if (stream == NULL || fragment == NULL || fragment->accounted) {
        return stream != NULL && fragment != NULL;
    }
    new_cost = utp_stream_recv_fragment_memory_cost(fragment);
    if (stream_memory_limit == 0u) {
        stream_memory_limit = 1u;
    }
    if (stream_fragment_limit == 0u) {
        stream_fragment_limit = 1u;
    }
    if (stream->recv_pinned_memory_bytes > stream_memory_limit ||
        new_cost > stream_memory_limit - stream->recv_pinned_memory_bytes ||
        stream->recv_accounted_fragment_count >= stream_fragment_limit) {
        return false;
    }
    if (account != NULL && account->connection_memory_bytes != NULL && account->connection_fragment_count != NULL) {
        connection_memory_limit   = account->connection_memory_limit == 0u ? 1u : account->connection_memory_limit;
        connection_fragment_limit = account->connection_fragment_limit == 0u ? 1u : account->connection_fragment_limit;
        if (*account->connection_memory_bytes > connection_memory_limit ||
            new_cost > connection_memory_limit - *account->connection_memory_bytes ||
            *account->connection_fragment_count >= connection_fragment_limit) {
            return false;
        }
        *account->connection_memory_bytes += new_cost;
        ++*account->connection_fragment_count;
        fragment->connection_memory_bytes   = account->connection_memory_bytes;
        fragment->connection_fragment_count = account->connection_fragment_count;
    }
    stream->recv_pinned_memory_bytes += new_cost;
    ++stream->recv_accounted_fragment_count;
    fragment->memory_cost = new_cost;
    fragment->accounted   = true;
    return true;
}

static void utp_stream_unaccount_recv_fragment(utp_stream_t* stream, utp_stream_recv_fragment_t* fragment)
{
    size_t cost;

    if (stream == NULL || fragment == NULL || !fragment->accounted) {
        return;
    }
    cost = fragment->memory_cost;
    stream->recv_pinned_memory_bytes =
        stream->recv_pinned_memory_bytes >= cost ? stream->recv_pinned_memory_bytes - cost : 0u;
    if (stream->recv_accounted_fragment_count != 0u) {
        --stream->recv_accounted_fragment_count;
    }
    if (fragment->connection_memory_bytes != NULL) {
        *fragment->connection_memory_bytes =
            *fragment->connection_memory_bytes >= cost ? *fragment->connection_memory_bytes - cost : 0u;
    }
    if (fragment->connection_fragment_count != NULL && *fragment->connection_fragment_count != 0u) {
        --*fragment->connection_fragment_count;
    }
    fragment->connection_memory_bytes   = NULL;
    fragment->connection_fragment_count = NULL;
    fragment->memory_cost               = 0u;
    fragment->accounted                 = false;
}

static utp_internal_error_t utp_stream_insert_ack_range(utp_stream_t* stream, uint64_t start, uint64_t end)
{
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

static void utp_stream_release_acked_prefix(utp_stream_t* stream)
{
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

static void utp_stream_remove_fragment(utp_stream_t* stream, size_t index)
{
    utp_stream_unaccount_recv_fragment(stream, &stream->recv_fragments[index]);
    if (stream->recv_fragments[index].packet != NULL) {
        utp_packet_in_release(stream->recv_fragments[index].packet);
        stream->recv_fragments[index].packet    = NULL;
        stream->recv_fragments[index].data_view = NULL;
    }
    if (index + 1u < stream->recv_fragment_count) {
        memmove(&stream->recv_fragments[index], &stream->recv_fragments[index + 1u],
                (stream->recv_fragment_count - index - 1u) * sizeof(stream->recv_fragments[0]));
    }
    --stream->recv_fragment_count;
}

static void utp_stream_clear_recv_fragments(utp_stream_t* stream)
{
    while (stream->recv_fragment_count != 0u) {
        utp_stream_remove_fragment(stream, stream->recv_fragment_count - 1u);
    }
}

static void utp_stream_rollback_frame_fragments(utp_stream_t* stream, utp_packet_in_t* packet,
                                                const uint64_t* inserted_offsets, size_t inserted_count)
{
    size_t offset_index;

    if (stream == NULL || packet == NULL || inserted_offsets == NULL) {
        return;
    }
    for (offset_index = inserted_count; offset_index != 0u; --offset_index) {
        size_t   fragment_index;
        uint64_t offset = inserted_offsets[offset_index - 1u];

        for (fragment_index = 0u; fragment_index < stream->recv_fragment_count; ++fragment_index) {
            if (stream->recv_fragments[fragment_index].packet == packet &&
                stream->recv_fragments[fragment_index].offset == offset) {
                utp_stream_remove_fragment(stream, fragment_index);
                break;
            }
        }
    }
}

static utp_internal_error_t utp_stream_insert_fragment(utp_stream_t* stream, uint64_t offset, const uint8_t* data,
                                                       size_t length, bool fin, utp_packet_in_t* packet,
                                                       const utp_stream_recv_account_t* account)
{
    size_t                     index    = 0u;
    utp_stream_recv_fragment_t fragment = {NULL, NULL, NULL, NULL, 0u, 0u, 0u, 0u, false, false};

    if (stream->recv_fragment_count >= UTP_STREAM_RECV_FRAGMENT_LIMIT ||
        length > UTP_STREAM_MAX_RECV_BUFFER_BYTES - stream->recv_buffered_bytes ||
        (length != 0u && (data == NULL || packet == NULL))) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    fragment.offset    = offset;
    fragment.length    = length;
    fragment.consumed  = 0u;
    fragment.fin       = fin;
    fragment.packet    = length != 0u ? packet : NULL;
    fragment.data_view = length != 0u ? data : NULL;
    if (length != 0u) {
        if (!utp_packet_in_ref(packet)) {
            return UTP_INTERNAL_ERROR_WOULD_BLOCK;
        }
    }
    if (!utp_stream_account_recv_fragment(stream, &fragment, account)) {
        if (length != 0u) {
            utp_packet_in_release(packet);
        }
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    while (index < stream->recv_fragment_count && stream->recv_fragments[index].offset < offset) {
        ++index;
    }
    if (index < stream->recv_fragment_count) {
        memmove(&stream->recv_fragments[index + 1u], &stream->recv_fragments[index],
                (stream->recv_fragment_count - index) * sizeof(stream->recv_fragments[0]));
    }
    stream->recv_fragments[index] = fragment;
    if (length != 0u) {
        stream->recv_buffered_bytes += length;
    }
    ++stream->recv_fragment_count;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_stream_mark_fin(utp_stream_t* stream, uint64_t fin_offset,
                                                const utp_stream_recv_account_t* account)
{
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
    return utp_stream_insert_fragment(stream, fin_offset, NULL, 0u, true, NULL, account);
}

void utp_stream_init(utp_stream_t* stream, uint32_t stream_id)
{
    if (stream != NULL) {
        stream->connection                       = NULL;
        stream->connection_consumed_total        = NULL;
        stream->send_buffer_offset               = 0u;
        stream->next_send_offset                 = 0u;
        stream->recv_offset                      = 0u;
        stream->local_max_stream_offset_received = 0u;
        stream->stream_id                        = stream_id;
        stream->peer_max_stream_data             = UTP_STREAM_DEFAULT_FLOW_WINDOW;
        stream->local_max_stream_data_advertised = UTP_STREAM_DEFAULT_FLOW_WINDOW;
        stream->last_max_stream_data_sent_us     = 0u;
        stream->last_stream_data_blocked_sent_us = 0u;
        stream->reset_error_code                 = 0u;
        stream->drr_deficit                      = 0u;
        stream->send_buffer_length               = 0u;
        stream->send_buffer_start                = 0u;
        stream->send_in_flight_bytes             = 0u;
        stream->recv_buffered_bytes              = 0u;
        stream->recv_pinned_memory_bytes         = 0u;
        stream->recv_fragment_count              = 0u;
        stream->recv_accounted_fragment_count    = 0u;
        stream->send_ack_range_count             = 0u;
        stream->priority                         = UTP_STREAM_PRIORITY_DEFAULT;
        stream->strict_wait_rounds               = 0u;
        stream->used                             = true;
        stream->local_fin_queued                 = false;
        stream->local_fin_sent                   = false;
        stream->peer_fin                         = false;
        stream->reset                            = false;
        stream->reset_by_peer                    = false;
    }
}

utp_internal_error_t utp_stream_on_reset(utp_stream_t* stream, uint16_t error_code, bool from_peer)
{
    if (stream == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream->reset_error_code     = error_code;
    stream->reset_by_peer        = from_peer;
    stream->send_buffer_length   = 0u;
    stream->send_buffer_start    = 0u;
    stream->send_in_flight_bytes = 0u;
    stream->send_ack_range_count = 0u;
    utp_stream_clear_recv_fragments(stream);
    stream->recv_buffered_bytes           = 0u;
    stream->recv_pinned_memory_bytes      = 0u;
    stream->recv_accounted_fragment_count = 0u;
    stream->local_fin_queued              = true;
    stream->local_fin_sent                = true;
    stream->peer_fin                      = true;
    stream->reset                         = true;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_stream_cleanup(utp_stream_t* stream)
{
    if (stream != NULL && stream->used) {
        (void)utp_stream_on_reset(stream, 0u, false);
        stream->connection                = NULL;
        stream->connection_consumed_total = NULL;
    }
}

utp_internal_error_t utp_stream_send_buffered_end_offset(const utp_stream_t* stream, uint64_t* out_offset)
{
    if (stream == NULL || out_offset == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((uint64_t)stream->send_buffer_length > UINT64_MAX - stream->send_buffer_offset) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *out_offset = stream->send_buffer_offset + (uint64_t)stream->send_buffer_length;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_write(utp_stream_t* stream, const uint8_t* data, size_t length)
{
    if (stream == NULL || !stream->used || (data == NULL && length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->reset || stream->local_fin_queued) {
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    if (stream->send_buffer_length > sizeof(stream->send_buffer) ||
        length > sizeof(stream->send_buffer) - stream->send_buffer_length) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    if (length != 0u) {
        utp_stream_copy_into_send_buffer(stream, data, length);
        stream->send_buffer_length += length;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_close(utp_stream_t* stream)
{
    if (stream == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->reset || stream->local_fin_queued) {
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    stream->local_fin_queued = true;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_acquire_write_views(utp_stream_t* stream, utp_stream_write_view_t* views,
                                                    size_t view_capacity, size_t* out_view_count, size_t* out_capacity)
{
    size_t free_capacity;
    size_t tail;
    size_t first_length;

    if (out_view_count != NULL) {
        *out_view_count = 0u;
    }
    if (out_capacity != NULL) {
        *out_capacity = 0u;
    }
    if (stream == NULL || views == NULL || out_view_count == NULL || out_capacity == NULL || view_capacity == 0u ||
        !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->reset || stream->local_fin_queued) {
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    if (stream->send_buffer_length > UTP_STREAM_SEND_BUFFER_CAPACITY) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    free_capacity = UTP_STREAM_SEND_BUFFER_CAPACITY - stream->send_buffer_length;
    if (free_capacity == 0u) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    tail         = utp_stream_send_index(stream, stream->send_buffer_length);
    first_length = UTP_STREAM_SEND_BUFFER_CAPACITY - tail;
    if (first_length > free_capacity) {
        first_length = free_capacity;
    }
    views[0].data   = stream->send_buffer + tail;
    views[0].length = first_length;
    *out_view_count = 1u;
    *out_capacity   = first_length;
    if (first_length < free_capacity && view_capacity >= 2u) {
        views[1].data   = stream->send_buffer;
        views[1].length = free_capacity - first_length;
        *out_view_count = 2u;
        *out_capacity   = free_capacity;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_commit_write_views(utp_stream_t* stream, size_t length)
{
    if (stream == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->reset || stream->local_fin_queued) {
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    if (stream->send_buffer_length > UTP_STREAM_SEND_BUFFER_CAPACITY ||
        length > UTP_STREAM_SEND_BUFFER_CAPACITY - stream->send_buffer_length) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream->send_buffer_length += length;
    return UTP_INTERNAL_ERROR_OK;
}

bool utp_stream_has_send_work(const utp_stream_t* stream)
{
    return stream != NULL && stream->used && !stream->reset &&
           (stream->send_buffer_length != stream->send_in_flight_bytes ||
            (stream->local_fin_queued && !stream->local_fin_sent));
}

utp_internal_error_t utp_stream_build_frame(utp_stream_t* stream, uint8_t* payload, size_t capacity,
                                            size_t* out_payload_length, uint32_t* out_stream_data_size,
                                            uint64_t* out_stream_offset, bool* out_fin)
{
    utp_frame_stream_t   frame;
    size_t               data_length;
    size_t               contiguous_length;
    const uint8_t*       data;
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

utp_internal_error_t utp_stream_build_frame_view(utp_stream_t* stream, uint8_t* header, size_t capacity,
                                                 size_t* out_header_length, const uint8_t** out_data,
                                                 uint32_t* out_stream_data_size, uint64_t* out_stream_offset,
                                                 bool* out_fin)
{
    return utp_stream_build_frame_view_limited(stream, header, capacity, SIZE_MAX, out_header_length, out_data,
                                               out_stream_data_size, out_stream_offset, out_fin);
}

utp_internal_error_t utp_stream_build_frame_view_limited(utp_stream_t* stream, uint8_t* header, size_t capacity,
                                                         size_t max_data_length, size_t* out_header_length,
                                                         const uint8_t** out_data, uint32_t* out_stream_data_size,
                                                         uint64_t* out_stream_offset, bool* out_fin)
{
    size_t               data_length;
    size_t               contiguous_length;
    size_t               unsent_length;
    uint8_t              flags = UTP_STREAM_FLAG_NONE;
    const uint8_t*       data;
    utp_internal_error_t error;

    if (stream == NULL || header == NULL || out_header_length == NULL || out_data == NULL ||
        out_stream_data_size == NULL || out_stream_offset == NULL || out_fin == NULL ||
        !utp_stream_has_send_work(stream)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (capacity < UTP_FRAME_STREAM_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    data          = utp_stream_unsent_data(stream, &contiguous_length);
    data_length   = contiguous_length;
    unsent_length = stream->send_buffer_length - stream->send_in_flight_bytes;
    if (data_length > max_data_length) {
        data_length = max_data_length;
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
    if (data_length == unsent_length && stream->local_fin_queued && !stream->local_fin_sent) {
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

utp_internal_error_t utp_stream_commit_built_frame(utp_stream_t* stream, uint32_t stream_data_size, bool fin)
{
    size_t data_size = (size_t)stream_data_size;

    if (stream == NULL || !stream->used || data_size > stream->send_buffer_length - stream->send_in_flight_bytes ||
        (uint64_t)data_size > UINT64_MAX - stream->next_send_offset || (fin && !stream->local_fin_queued)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream->next_send_offset     += data_size;
    stream->send_in_flight_bytes += data_size;
    if (fin) {
        stream->local_fin_sent = true;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_abandon_built_frame(utp_stream_t* stream, uint64_t stream_offset,
                                                    uint32_t stream_data_size, bool fin)
{
    const size_t data_size = (size_t)stream_data_size;

    if (stream == NULL || !stream->used || data_size > stream->send_in_flight_bytes ||
        (uint64_t)data_size > UINT64_MAX - stream_offset || stream->next_send_offset != stream_offset + data_size ||
        (fin && !stream->local_fin_sent)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream->next_send_offset     -= data_size;
    stream->send_in_flight_bytes -= data_size;
    if (fin) {
        stream->local_fin_sent = false;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_on_packet_acked(utp_stream_t* stream, uint32_t stream_data_size)
{
    return stream == NULL ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT
                          : utp_stream_on_packet_acked_range(stream, stream->send_buffer_offset, stream_data_size);
}

utp_internal_error_t utp_stream_on_packet_acked_range(utp_stream_t* stream, uint64_t stream_offset,
                                                      uint32_t stream_data_size)
{
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

void utp_stream_update_peer_max_stream_data(utp_stream_t* stream, uint64_t maximum_stream_data)
{
    if (stream != NULL && stream->used && maximum_stream_data > stream->peer_max_stream_data) {
        stream->peer_max_stream_data = maximum_stream_data;
    }
}

utp_internal_error_t utp_stream_on_frame_packet_accounted(utp_stream_t* stream, const utp_frame_stream_t* frame,
                                                          utp_packet_in_t*                 packet,
                                                          const utp_stream_recv_account_t* account)
{
    uint64_t             original_end;
    uint64_t             start;
    uint64_t             end;
    size_t               data_index;
    size_t               index;
    size_t               inserted_count = 0u;
    uint64_t             inserted_offsets[UTP_STREAM_RECV_FRAGMENT_LIMIT];
    utp_internal_error_t error;

    if (stream == NULL || frame == NULL || !stream->used || stream->reset || frame->stream_id != stream->stream_id ||
        (frame->flags & (uint8_t)~UTP_STREAM_FLAG_FIN) != 0u ||
        (frame->data_length == 0u && (frame->flags & UTP_STREAM_FLAG_FIN) == 0u) ||
        (frame->data == NULL && frame->data_length != 0u) || (frame->data_length != 0u && packet == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((uint64_t)frame->data_length > UINT64_MAX - frame->offset) {
        return UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL;
    }
    original_end = frame->offset + (uint64_t)frame->data_length;
    if (original_end > stream->local_max_stream_data_advertised) {
        return UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL;
    }
    if (frame->offset > stream->recv_offset && frame->offset - stream->recv_offset > UTP_STREAM_RECV_MAX_GAP) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
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

            error =
                utp_stream_insert_fragment(stream, start, frame->data + data_index, gap_len, false, packet, account);
            if (error != UTP_INTERNAL_ERROR_OK) {
                utp_stream_rollback_frame_fragments(stream, packet, inserted_offsets, inserted_count);
                return error;
            }
            inserted_offsets[inserted_count] = start;
            ++inserted_count;
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
        error = utp_stream_insert_fragment(stream, start, frame->data + data_index, (size_t)(end - start), false,
                                           packet, account);
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_stream_rollback_frame_fragments(stream, packet, inserted_offsets, inserted_count);
            return error;
        }
        inserted_offsets[inserted_count] = start;
        ++inserted_count;
    }
    if ((frame->flags & UTP_STREAM_FLAG_FIN) != 0u) {
        error = utp_stream_mark_fin(stream, original_end, account);
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_stream_rollback_frame_fragments(stream, packet, inserted_offsets, inserted_count);
        }
        return error;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_on_frame_packet(utp_stream_t* stream, const utp_frame_stream_t* frame,
                                                utp_packet_in_t* packet)
{
    return utp_stream_on_frame_packet_accounted(stream, frame, packet, NULL);
}

utp_internal_error_t utp_stream_on_frame(utp_stream_t* stream, const utp_frame_stream_t* frame)
{
    return utp_stream_on_frame_packet(stream, frame, NULL);
}

utp_internal_error_t utp_stream_acquire_read_view(utp_stream_t* stream, utp_stream_read_view_t* out_view)
{
    utp_stream_read_view_t view = {NULL, 0u, 0u, false};

    if (stream == NULL || out_view == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    view.offset = stream->recv_offset;
    if (stream->recv_fragment_count != 0u &&
        utp_stream_fragment_read_offset(&stream->recv_fragments[0]) == stream->recv_offset) {
        const utp_stream_recv_fragment_t* fragment  = &stream->recv_fragments[0];
        size_t                            remaining = fragment->length - fragment->consumed;

        if (remaining == 0u) {
            if (fragment->fin) {
                view.fin  = true;
                *out_view = view;
                return UTP_INTERNAL_ERROR_OK;
            }
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        view.data   = fragment->data_view + fragment->consumed;
        view.length = remaining;
        *out_view   = view;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (stream->peer_fin) {
        view.fin  = true;
        *out_view = view;
        return UTP_INTERNAL_ERROR_OK;
    }
    *out_view = view;
    return UTP_INTERNAL_ERROR_WOULD_BLOCK;
}

utp_internal_error_t utp_stream_commit_read_view(utp_stream_t* stream, uint64_t offset, size_t length)
{
    utp_stream_recv_fragment_t* fragment;
    size_t                      available;

    if (stream == NULL || !stream->used || offset != stream->recv_offset) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (length == 0u) {
        if (stream->recv_fragment_count != 0u &&
            utp_stream_fragment_read_offset(&stream->recv_fragments[0]) == stream->recv_offset &&
            stream->recv_fragments[0].length == stream->recv_fragments[0].consumed && stream->recv_fragments[0].fin) {
            stream->peer_fin = true;
            utp_stream_remove_fragment(stream, 0u);
            return UTP_INTERNAL_ERROR_OK;
        }
        return stream->peer_fin ? UTP_INTERNAL_ERROR_OK : UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->recv_fragment_count == 0u ||
        utp_stream_fragment_read_offset(&stream->recv_fragments[0]) != stream->recv_offset) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    fragment  = &stream->recv_fragments[0];
    available = fragment->length - fragment->consumed;
    if (available == 0u || length > available) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stream->connection_consumed_total != NULL &&
        (uint64_t)length > UINT64_MAX - *stream->connection_consumed_total) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    fragment->consumed          += length;
    stream->recv_offset         += length;
    stream->recv_buffered_bytes -= length;
    if (stream->connection_consumed_total != NULL) {
        *stream->connection_consumed_total += (uint64_t)length;
    }
    if (fragment->consumed == fragment->length) {
        if (fragment->fin) {
            stream->peer_fin = true;
        }
        utp_stream_remove_fragment(stream, 0u);
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_stream_read(utp_stream_t* stream, uint8_t* buffer, size_t capacity, size_t* out_length,
                                     bool* out_fin)
{
    size_t copied = 0u;

    if (stream == NULL || buffer == NULL || out_length == NULL || out_fin == NULL || !stream->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    while (copied < capacity && stream->recv_fragment_count != 0u &&
           utp_stream_fragment_read_offset(&stream->recv_fragments[0]) == stream->recv_offset) {
        utp_stream_recv_fragment_t* fragment = &stream->recv_fragments[0];
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
        memcpy(buffer + copied, fragment->data_view + fragment->consumed, to_copy);
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
    if (copied != 0u && stream->connection_consumed_total != NULL) {
        if ((uint64_t)copied > UINT64_MAX - *stream->connection_consumed_total) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        *stream->connection_consumed_total += (uint64_t)copied;
    }
    *out_length = copied;
    *out_fin    = copied == 0u && stream->peer_fin;
    if (copied != 0u) {
        return UTP_INTERNAL_ERROR_OK;
    }
    return stream->peer_fin ? UTP_INTERNAL_ERROR_CLOSED : UTP_INTERNAL_ERROR_WOULD_BLOCK;
}

size_t utp_stream_readable_bytes(const utp_stream_t* stream)
{
    size_t   readable = 0u;
    uint64_t offset;
    size_t   index;

    if (stream == NULL || !stream->used) {
        return 0u;
    }
    offset = stream->recv_offset;
    for (index = 0u; index < stream->recv_fragment_count; ++index) {
        const utp_stream_recv_fragment_t* fragment = &stream->recv_fragments[index];
        size_t                            remaining;

        if (utp_stream_fragment_read_offset(fragment) != offset) {
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

size_t utp_stream_send_in_flight_bytes(const utp_stream_t* stream)
{
    return stream == NULL || !stream->used ? 0u : stream->send_in_flight_bytes;
}

bool utp_stream_is_closed(const utp_stream_t* stream)
{
    return stream != NULL && stream->used && stream->local_fin_queued && stream->local_fin_sent && stream->peer_fin;
}
