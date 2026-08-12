#include "proto/packet_out.h"

#include <limits.h>
#include <string.h>

#include "proto/frame.h"
#include "proto/proto.h"

static void bucket_cleanup(utp_packet_out_bucket_t* bucket, const utp_allocator_t* allocator)
{
    if (bucket->nodes != NULL) {
        utp_allocator_free(allocator, bucket->nodes);
    }
    if (bucket->storage != NULL) {
        utp_allocator_free(allocator, bucket->storage);
    }
    bucket->size    = 0u;
    bucket->count   = 0u;
    bucket->storage = NULL;
    bucket->nodes   = NULL;
    TAILQ_INIT(&bucket->free_buffers);
}

static utp_internal_error_t bucket_init(utp_packet_out_bucket_t* bucket, const utp_allocator_t* allocator,
                                        uint16_t size, size_t count)
{
    TAILQ_INIT(&bucket->free_buffers);
    bucket->size    = size;
    bucket->count   = count;
    bucket->storage = NULL;
    bucket->nodes   = NULL;

    if (count > SIZE_MAX / size || count > SIZE_MAX / sizeof(*bucket->nodes)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    bucket->storage = utp_allocator_alloc(allocator, (size_t)size * count);
    if (bucket->storage == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    bucket->nodes = utp_allocator_alloc(allocator, count * sizeof(*bucket->nodes));
    if (bucket->nodes == NULL) {
        bucket_cleanup(bucket, allocator);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    for (size_t i = 0u; i < count; ++i) {
        bucket->nodes[i].data = bucket->storage + i * (size_t)size;
        TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[i], link);
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_cleanup(utp_packet_out_pool_t* pool)
{
    if (pool == NULL) {
        return;
    }
    for (size_t i = 0u; i < pool->bucket_count; ++i) {
        bucket_cleanup(&pool->buckets[i], pool->allocator);
    }
    if (pool->structs != NULL) {
        utp_allocator_free(pool->allocator, pool->structs);
    }
    pool->allocator       = NULL;
    pool->structs         = NULL;
    pool->struct_capacity = 0u;
    pool->bucket_count    = 0u;
    TAILQ_INIT(&pool->free_structs);
}

utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t* pool, const utp_allocator_t* allocator,
                                              size_t struct_capacity, const utp_packet_out_bucket_config_t* buckets,
                                              size_t bucket_count)
{
    utp_packet_out_bucket_config_t sorted[UTP_PACKET_OUT_MAX_BUCKETS];

    if (pool == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pool->allocator       = NULL;
    pool->structs         = NULL;
    pool->struct_capacity = 0u;
    pool->bucket_count    = 0u;
    TAILQ_INIT(&pool->free_structs);

    if (struct_capacity == 0u || struct_capacity > SIZE_MAX / sizeof(utp_packet_out_t) || buckets == NULL ||
        bucket_count == 0u || bucket_count > UTP_PACKET_OUT_MAX_BUCKETS) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < bucket_count; ++i) {
        if (buckets[i].size == 0u || buckets[i].count == 0u) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }

    for (size_t i = 0u; i < bucket_count; ++i) {
        sorted[i] = buckets[i];
    }
    for (size_t i = 1u; i < bucket_count; ++i) {
        for (size_t j = i; j > 0u && sorted[j - 1u].size > sorted[j].size; --j) {
            utp_packet_out_bucket_config_t tmp = sorted[j];

            sorted[j]      = sorted[j - 1u];
            sorted[j - 1u] = tmp;
        }
    }

    pool->allocator = utp_allocator_resolve(allocator);
    if (pool->allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pool->structs = utp_allocator_alloc(pool->allocator, struct_capacity * sizeof(utp_packet_out_t));
    if (pool->structs == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    pool->struct_capacity = struct_capacity;
    TAILQ_INIT(&pool->free_structs);
    for (size_t i = 0u; i < struct_capacity; ++i) {
        pool->structs[i].loss_chain = &pool->structs[i];
        TAILQ_INSERT_TAIL(&pool->free_structs, &pool->structs[i], po_next);
    }

    for (size_t i = 0u; i < bucket_count; ++i) {
        utp_internal_error_t error = bucket_init(&pool->buckets[i], pool->allocator, sorted[i].size, sorted[i].count);

        if (!utp_internal_error_is_ok(error)) {
            pool->bucket_count = i;
            utp_packet_out_pool_cleanup(pool);
            return error;
        }
    }
    pool->bucket_count = bucket_count;
    return UTP_INTERNAL_ERROR_OK;
}

static size_t choose_bucket(const utp_packet_out_pool_t* pool, uint16_t requested_size)
{
    for (size_t i = 0u; i < pool->bucket_count; ++i) {
        if (pool->buckets[i].size >= requested_size) {
            return i;
        }
    }
    return pool->bucket_count;
}

static void reset_packet_out_for_acquire(utp_packet_out_t* pkt, utp_packet_out_buffer_node_t* node,
                                         const utp_packet_out_bucket_t* bucket, size_t bucket_index)
{
    pkt->sent_time_us                = 0u;
    pkt->packet_number               = 0u;
    pkt->ack_number                  = 0u;
    pkt->loss_chain                  = pkt;
    pkt->frame_types                 = 0u;
    pkt->po_flags                    = 0u;
    pkt->local_flags                 = 0u;
    pkt->data_size                   = 0u;
    pkt->encrypt_data_size           = 0u;
    pkt->alloc_size                  = bucket->size;
    pkt->packet_type                 = 0u;
    pkt->slice_count                 = 0u;
    pkt->frame_meta_count            = 0u;
    pkt->stream_data_size            = 0u;
    pkt->path_validation_generation  = 0u;
    pkt->transient_ack_size          = 0u;
    pkt->control_prefix_size         = 0u;
    pkt->early_plaintext_prefix_size = 0u;
    pkt->stream_id                   = 0u;
    pkt->stream_offset               = 0u;
    pkt->attempts                    = NULL;
    pkt->attempt_count               = 0u;
    pkt->bw_packet_state.valid       = false;
    pkt->bw_state                    = NULL;
    pkt->raw_data                    = node->data;
    pkt->encrypt_data                = node->data;
    pkt->destination.family          = UTP_ADDRESS_FAMILY_UNSPECIFIED;
    pkt->destination.port            = 0u;
    pkt->destination.scope_id        = 0u;
    pkt->has_destination             = false;
    pkt->bucket_index                = bucket_index;
}

utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t* pool, uint16_t requested_size,
                                                 utp_packet_out_t** out)
{
    if (pool == NULL || out == NULL || requested_size == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    size_t bucket_index = choose_bucket(pool, requested_size);
    if (bucket_index == pool->bucket_count) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_packet_out_bucket_t* bucket = &pool->buckets[bucket_index];
    if (TAILQ_EMPTY(&bucket->free_buffers) || TAILQ_EMPTY(&pool->free_structs)) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }

    utp_packet_out_buffer_node_t* node = TAILQ_FIRST(&bucket->free_buffers);
    TAILQ_REMOVE(&bucket->free_buffers, node, link);

    utp_packet_out_t* pkt = TAILQ_FIRST(&pool->free_structs);
    TAILQ_REMOVE(&pool->free_structs, pkt, po_next);

    reset_packet_out_for_acquire(pkt, node, bucket, bucket_index);

    *out = pkt;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_release(utp_packet_out_pool_t* pool, utp_packet_out_t* pkt)
{
    if (pool == NULL || pkt == NULL) {
        return;
    }

    uint8_t*                 raw_data     = pkt->raw_data;
    uint8_t*                 encrypt_data = pkt->encrypt_data;
    uint16_t                 alloc_size   = pkt->alloc_size;
    size_t                   bucket_index = pkt->bucket_index;
    utp_packet_out_bucket_t* bucket       = &pool->buckets[bucket_index];

    pkt->sent_time_us                = 0u;
    pkt->packet_number               = 0u;
    pkt->ack_number                  = 0u;
    pkt->loss_chain                  = pkt;
    pkt->frame_types                 = 0u;
    pkt->po_flags                    = 0u;
    pkt->local_flags                 = 0u;
    pkt->data_size                   = 0u;
    pkt->encrypt_data_size           = 0u;
    pkt->alloc_size                  = alloc_size;
    pkt->packet_type                 = 0u;
    pkt->slice_count                 = 0u;
    pkt->frame_meta_count            = 0u;
    pkt->stream_data_size            = 0u;
    pkt->path_validation_generation  = 0u;
    pkt->transient_ack_size          = 0u;
    pkt->control_prefix_size         = 0u;
    pkt->early_plaintext_prefix_size = 0u;
    pkt->stream_id                   = 0u;
    pkt->stream_offset               = 0u;
    pkt->attempts                    = NULL;
    pkt->attempt_count               = 0u;
    pkt->bw_packet_state.valid       = false;
    pkt->bw_state                    = NULL;
    pkt->raw_data                    = raw_data;
    pkt->encrypt_data                = encrypt_data;
    pkt->destination.family          = UTP_ADDRESS_FAMILY_UNSPECIFIED;
    pkt->destination.port            = 0u;
    pkt->destination.scope_id        = 0u;
    pkt->has_destination             = false;
    pkt->bucket_index                = bucket_index;
    TAILQ_INSERT_TAIL(&pool->free_structs, pkt, po_next);

    size_t node_index = (size_t)(raw_data - bucket->storage) / bucket->size;
    TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[node_index], link);
}

utp_internal_error_t utp_packet_out_strip_prefix(utp_packet_out_t* pkt, uint16_t prefix_length)
{
    utp_packet_out_slice_t slices[UTP_PACKET_OUT_MAX_SLICES];
    uint32_t               frame_types  = 0u;
    size_t                 input_offset = 0u;
    size_t                 output_count = 0u;
    uint8_t                meta_count   = 0u;

    if (pkt == NULL || pkt->raw_data == NULL || prefix_length == 0u || pkt->data_size < UTP_PACKET_HEADER_SIZE ||
        prefix_length > pkt->data_size - UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_packet_header_t header;
    if (utp_proto_decode_header(&header, pkt->raw_data, UTP_PACKET_HEADER_SIZE) != UTP_INTERNAL_ERROR_OK ||
        header.payload_length < prefix_length) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (pkt->slice_count == 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    for (size_t index = 0u; index < pkt->slice_count; ++index) {
        const utp_packet_out_slice_t* slice       = &pkt->slices[index];
        size_t                        slice_start = input_offset;
        size_t                        slice_end;
        size_t                        keep_start;
        size_t                        keep_end;

        if (slice->length == 0u || slice->length > SIZE_MAX - slice_start) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        slice_end    = slice_start + slice->length;
        input_offset = slice_end;
        keep_start   = slice_start;
        keep_end     = slice_end;
        if (keep_start < UTP_PACKET_HEADER_SIZE) {
            keep_end = keep_end < UTP_PACKET_HEADER_SIZE ? keep_end : UTP_PACKET_HEADER_SIZE;
        } else if (keep_start < (size_t)UTP_PACKET_HEADER_SIZE + prefix_length) {
            keep_start = (size_t)UTP_PACKET_HEADER_SIZE + prefix_length;
        }
        if (keep_start >= keep_end) {
            continue;
        }
        if (output_count >= UTP_PACKET_OUT_MAX_SLICES) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        slices[output_count]        = *slice;
        slices[output_count].length = (uint16_t)(keep_end - keep_start);
        if (slice->source == UTP_PACKET_OUT_SLICE_RAW_OFFSET) {
            slices[output_count].offset = (uint16_t)(slice->offset + (keep_start - slice_start));
        } else if (slice->source == UTP_PACKET_OUT_SLICE_EXTERNAL) {
            slices[output_count].data = (const uint8_t*)slice->data + (keep_start - slice_start);
        } else {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        ++output_count;
        if (slice_start < UTP_PACKET_HEADER_SIZE && slice_end > (size_t)UTP_PACKET_HEADER_SIZE + prefix_length) {
            keep_start = (size_t)UTP_PACKET_HEADER_SIZE + prefix_length;
            keep_end   = slice_end;
            if (output_count >= UTP_PACKET_OUT_MAX_SLICES) {
                return UTP_INTERNAL_ERROR_LIMIT;
            }
            slices[output_count]        = *slice;
            slices[output_count].length = (uint16_t)(keep_end - keep_start);
            if (slice->source == UTP_PACKET_OUT_SLICE_RAW_OFFSET) {
                slices[output_count].offset = (uint16_t)(slice->offset + (keep_start - slice_start));
            } else {
                slices[output_count].data = (const uint8_t*)slice->data + (keep_start - slice_start);
            }
            ++output_count;
        }
    }
    if (input_offset != pkt->data_size) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    for (size_t index = 0u; index < pkt->frame_meta_count; ++index) {
        utp_frame_meta_info_t meta = pkt->frame_meta[index];

        if ((meta.frame_flags & (UTP_FRAME_META_TRANSIENT_ON_RETRANSMIT | UTP_FRAME_META_SEMANTIC_CONTROL)) != 0u) {
            continue;
        }
        pkt->frame_meta[meta_count++]  = meta;
        frame_types                   |= UTP_FRAME_BIT(meta.frame_type);
    }
    header.payload_length = (uint16_t)(header.payload_length - prefix_length);
    if (utp_proto_encode_header(pkt->raw_data, pkt->alloc_size, &header) != UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    pkt->data_size   = (uint16_t)(pkt->data_size - prefix_length);
    pkt->slice_count = (uint8_t)output_count;
    for (size_t index = 0u; index < output_count; ++index) {
        pkt->slices[index] = slices[index];
    }
    pkt->frame_meta_count    = meta_count;
    pkt->frame_types         = frame_types;
    pkt->transient_ack_size  = 0u;
    pkt->control_prefix_size = 0u;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_packet_out_resolve_slice(const utp_packet_out_t*       pkt,
                                                         const utp_packet_out_slice_t* slice, const uint8_t** data)
{
    if (pkt == NULL || slice == NULL || data == NULL || slice->length == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (slice->source == UTP_PACKET_OUT_SLICE_RAW_OFFSET) {
        if (pkt->raw_data == NULL || slice->offset > pkt->alloc_size ||
            slice->length > pkt->alloc_size - slice->offset) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        *data = pkt->raw_data + slice->offset;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (slice->source == UTP_PACKET_OUT_SLICE_EXTERNAL) {
        if (slice->data == NULL) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        *data = slice->data;
        return UTP_INTERNAL_ERROR_OK;
    }
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
}

utp_internal_error_t utp_packet_out_flatten(const utp_packet_out_t* pkt, uint8_t* buffer, size_t capacity,
                                            size_t* out_length)
{
    size_t offset = 0u;

    if (out_length != NULL) {
        *out_length = 0u;
    }
    if (pkt == NULL || buffer == NULL || out_length == NULL || pkt->raw_data == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (pkt->data_size > capacity) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    if (pkt->slice_count == 0u) {
        memcpy(buffer, pkt->raw_data, pkt->data_size);
        *out_length = pkt->data_size;
        return UTP_INTERNAL_ERROR_OK;
    }
    for (uint8_t index = 0u; index < pkt->slice_count; ++index) {
        const uint8_t*       data;
        utp_internal_error_t error = utp_packet_out_resolve_slice(pkt, &pkt->slices[index], &data);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (pkt->slices[index].length > capacity - offset) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        memcpy(buffer + offset, data, pkt->slices[index].length);
        offset += pkt->slices[index].length;
    }
    if (offset != pkt->data_size) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    *out_length = offset;
    return UTP_INTERNAL_ERROR_OK;
}
