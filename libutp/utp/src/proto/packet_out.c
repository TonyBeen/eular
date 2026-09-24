#include "proto/packet_out.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

#include "proto/frame.h"
#include "proto/proto.h"

static const uint16_t k_packet_out_bucket_sizes[] = {1280u, 1500u, 4096u, 9000u, UINT16_MAX};

#define UTP_PACKET_OUT_POOL_SAMPLE_PERIOD 1024u

static void bucket_init(utp_packet_out_bucket_t* bucket, uint16_t size)
{
    assert(bucket != NULL);
    memset(bucket, 0, sizeof(*bucket));
    bucket->size = size;
    TAILQ_INIT(&bucket->free_buffers);
    TAILQ_INIT(&bucket->blocks);
}

static void bucket_cleanup(utp_packet_out_bucket_t* bucket, const utp_allocator_t* allocator)
{
    utp_packet_out_buffer_block_t* block;

    assert(bucket != NULL);
    assert(allocator != NULL);
    while ((block = TAILQ_FIRST(&bucket->blocks)) != NULL) {
        TAILQ_REMOVE(&bucket->blocks, block, link);
        utp_allocator_free(allocator, block);
    }
    memset(bucket, 0, sizeof(*bucket));
    TAILQ_INIT(&bucket->free_buffers);
    TAILQ_INIT(&bucket->blocks);
}

static size_t choose_bucket(const utp_packet_out_buffer_pool_t* pool, uint16_t requested_size)
{
    assert(pool != NULL);
    for (size_t index = 0u; index < pool->bucket_count; ++index) {
        if (pool->buckets[index].size >= requested_size) {
            return index;
        }
    }
    return pool->bucket_count;
}

static bool bucket_has_new_sample(const utp_packet_out_bucket_t* bucket)
{
    assert(bucket != NULL);
    return bucket->sample_calls != 0u && bucket->sample_calls % UTP_PACKET_OUT_POOL_SAMPLE_PERIOD == 0u;
}

static void bucket_record_activity(utp_packet_out_bucket_t* bucket)
{
    assert(bucket != NULL);
    ++bucket->sample_calls;
    if (bucket->in_use_count > bucket->sample_max_in_use) {
        bucket->sample_max_in_use = bucket->in_use_count;
    }
    if (!bucket_has_new_sample(bucket)) {
        return;
    }
    if (bucket->sample_max_average == 0u) {
        bucket->sample_max_average = bucket->sample_max_in_use;
    } else {
        bucket->sample_max_average -= bucket->sample_max_average / 8u;
        bucket->sample_max_average += bucket->sample_max_in_use / 8u;
    }
    bucket->sample_max_in_use = bucket->in_use_count;
}

static void bucket_shrink(utp_packet_out_bucket_t* bucket, const utp_allocator_t* allocator)
{
    utp_packet_out_buffer_block_t* block;
    size_t                         target_count;

    assert(bucket != NULL);
    assert(allocator != NULL);
    if (bucket->sample_max_average >= bucket->allocated_count / 4u ||
        bucket->allocated_count <= UTP_PACKET_OUT_GROW_COUNT) {
        return;
    }
    target_count = bucket->allocated_count / 2u;
    block        = TAILQ_FIRST(&bucket->blocks);
    while (block != NULL && bucket->allocated_count > target_count) {
        utp_packet_out_buffer_block_t* next = TAILQ_NEXT(block, link);

        if (block->free_count == UTP_PACKET_OUT_GROW_COUNT) {
            for (size_t index = 0u; index < UTP_PACKET_OUT_GROW_COUNT; ++index) {
                TAILQ_REMOVE(&bucket->free_buffers, &block->nodes[index], link);
            }
            TAILQ_REMOVE(&bucket->blocks, block, link);
            utp_allocator_free(allocator, block);
            bucket->allocated_count -= UTP_PACKET_OUT_GROW_COUNT;
        }
        block = next;
    }
}

static utp_internal_error_t bucket_grow(utp_packet_out_bucket_t* bucket, const utp_allocator_t* allocator)
{
    const size_t                   storage_size = (size_t)bucket->size * UTP_PACKET_OUT_GROW_COUNT;
    utp_packet_out_buffer_block_t* block;

    assert(bucket != NULL);
    assert(allocator != NULL);
    block = utp_allocator_alloc(allocator, sizeof(*block) + storage_size);
    if (block == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    block->free_count = UTP_PACKET_OUT_GROW_COUNT;
    block->storage    = (uint8_t*)(block + 1);
    for (size_t index = 0u; index < UTP_PACKET_OUT_GROW_COUNT; ++index) {
        block->nodes[index].block = block;
        block->nodes[index].data  = block->storage + index * (size_t)bucket->size;
        TAILQ_INSERT_TAIL(&bucket->free_buffers, &block->nodes[index], link);
    }
    TAILQ_INSERT_TAIL(&bucket->blocks, block, link);
    bucket->allocated_count += UTP_PACKET_OUT_GROW_COUNT;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t bucket_acquire(utp_packet_out_buffer_pool_t* buffer_pool, size_t bucket_index,
                                           utp_packet_out_buffer_node_t** out)
{
    assert(buffer_pool != NULL);
    assert(buffer_pool->allocator != NULL);
    assert(bucket_index < buffer_pool->bucket_count);
    assert(out != NULL);
    utp_packet_out_bucket_t*      bucket = &buffer_pool->buckets[bucket_index];
    utp_packet_out_buffer_node_t* node;
    utp_internal_error_t          error;

    if (TAILQ_EMPTY(&bucket->free_buffers)) {
        error = bucket_grow(bucket, buffer_pool->allocator);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    node = TAILQ_FIRST(&bucket->free_buffers);
    TAILQ_REMOVE(&bucket->free_buffers, node, link);
    --node->block->free_count;
    ++bucket->in_use_count;
    bucket_record_activity(bucket);
    if (bucket_has_new_sample(bucket)) {
        bucket_shrink(bucket, buffer_pool->allocator);
    }
    *out = node;
    return UTP_INTERNAL_ERROR_OK;
}

static void bucket_release(utp_packet_out_buffer_pool_t* buffer_pool, size_t bucket_index,
                           utp_packet_out_buffer_node_t* node)
{
    assert(buffer_pool != NULL);
    assert(buffer_pool->allocator != NULL);
    assert(bucket_index < buffer_pool->bucket_count);
    assert(node != NULL);
    assert(node->block != NULL);
    utp_packet_out_bucket_t* bucket = &buffer_pool->buckets[bucket_index];

    ++node->block->free_count;
    TAILQ_INSERT_HEAD(&bucket->free_buffers, node, link);
    --bucket->in_use_count;
    bucket_record_activity(bucket);
    if (bucket_has_new_sample(bucket)) {
        bucket_shrink(bucket, buffer_pool->allocator);
    }
}

utp_internal_error_t utp_packet_out_buffer_pool_init(utp_packet_out_buffer_pool_t* pool,
                                                     const utp_allocator_t*        allocator)
{
    const utp_allocator_t* resolved_allocator;

    if (pool == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    resolved_allocator = utp_allocator_resolve(allocator);
    if (resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(pool, 0, sizeof(*pool));
    pool->allocator    = resolved_allocator;
    pool->bucket_count = sizeof(k_packet_out_bucket_sizes) / sizeof(k_packet_out_bucket_sizes[0]);
    for (size_t index = 0u; index < pool->bucket_count; ++index) {
        bucket_init(&pool->buckets[index], k_packet_out_bucket_sizes[index]);
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_buffer_pool_cleanup(utp_packet_out_buffer_pool_t* pool)
{
    if (pool == NULL) {
        return;
    }
    for (size_t index = 0u; index < pool->bucket_count; ++index) {
        bucket_cleanup(&pool->buckets[index], pool->allocator);
    }
    pool->allocator    = NULL;
    pool->bucket_count = 0u;
}

utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t* pool, const utp_allocator_t* allocator)
{
    const utp_allocator_t* resolved_allocator;

    if (pool == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    resolved_allocator = utp_allocator_resolve(allocator);
    if (resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(pool, 0, sizeof(*pool));
    pool->allocator = resolved_allocator;
    TAILQ_INIT(&pool->free_structs);
    TAILQ_INIT(&pool->blocks);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_cleanup(utp_packet_out_pool_t* pool)
{
    utp_packet_out_block_t* block;

    if (pool == NULL) {
        return;
    }
    while ((block = TAILQ_FIRST(&pool->blocks)) != NULL) {
        TAILQ_REMOVE(&pool->blocks, block, link);
        utp_allocator_free(pool->allocator, block);
    }
    pool->allocator       = NULL;
    pool->allocated_count = 0u;
    TAILQ_INIT(&pool->free_structs);
    TAILQ_INIT(&pool->blocks);
}

static utp_internal_error_t packet_out_pool_grow(utp_packet_out_pool_t* pool)
{
    utp_packet_out_block_t* block;

    assert(pool != NULL);
    assert(pool->allocator != NULL);
    block = utp_allocator_alloc(pool->allocator, sizeof(*block));
    if (block == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    for (size_t index = 0u; index < UTP_PACKET_OUT_GROW_COUNT; ++index) {
        block->packets[index].loss_chain = &block->packets[index];
        TAILQ_INSERT_TAIL(&pool->free_structs, &block->packets[index], po_next);
    }
    TAILQ_INSERT_TAIL(&pool->blocks, block, link);
    pool->allocated_count += UTP_PACKET_OUT_GROW_COUNT;
    return UTP_INTERNAL_ERROR_OK;
}

static void reset_packet_out_for_acquire(utp_packet_out_t* pkt, utp_packet_out_buffer_node_t* node,
                                         const utp_packet_out_bucket_t* bucket, size_t bucket_index)
{
    assert(pkt != NULL);
    assert(node != NULL);
    assert(bucket != NULL);
    assert(bucket_index < sizeof(k_packet_out_bucket_sizes) / sizeof(k_packet_out_bucket_sizes[0]));
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
    pkt->buffer_node                 = node;
}

utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t* pool, utp_packet_out_buffer_pool_t* buffer_pool,
                                                 uint16_t requested_size, utp_packet_out_t** out)
{
    utp_packet_out_buffer_node_t* node;
    utp_packet_out_t*             packet;
    size_t                        bucket_index;
    utp_internal_error_t          error;

    if (out != NULL) {
        *out = NULL;
    }
    if (pool == NULL || buffer_pool == NULL || out == NULL || requested_size == 0u || pool->allocator == NULL ||
        buffer_pool->allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    bucket_index = choose_bucket(buffer_pool, requested_size);
    error        = bucket_acquire(buffer_pool, bucket_index, &node);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (TAILQ_EMPTY(&pool->free_structs)) {
        error = packet_out_pool_grow(pool);
        if (error != UTP_INTERNAL_ERROR_OK) {
            bucket_release(buffer_pool, bucket_index, node);
            return error;
        }
    }
    packet = TAILQ_FIRST(&pool->free_structs);
    TAILQ_REMOVE(&pool->free_structs, packet, po_next);
    reset_packet_out_for_acquire(packet, node, &buffer_pool->buckets[bucket_index], bucket_index);
    *out = packet;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_release(utp_packet_out_pool_t* pool, utp_packet_out_buffer_pool_t* buffer_pool,
                                 utp_packet_out_t* pkt)
{
    utp_packet_out_buffer_node_t* node;
    size_t                        bucket_index;

    assert(pool != NULL);
    assert(buffer_pool != NULL);
    assert(pkt != NULL);
    assert(pool->allocator != NULL);
    assert(buffer_pool->allocator != NULL);
    bucket_index = pkt->bucket_index;
    node         = pkt->buffer_node;
    assert(bucket_index < buffer_pool->bucket_count);
    assert(node != NULL);
    assert(node->block != NULL);
    assert(node->data == pkt->raw_data);
    assert(pkt->alloc_size == buffer_pool->buckets[bucket_index].size);
    reset_packet_out_for_acquire(pkt, node, &buffer_pool->buckets[bucket_index], bucket_index);
    TAILQ_INSERT_TAIL(&pool->free_structs, pkt, po_next);
    bucket_release(buffer_pool, bucket_index, node);
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
    if ((pkt->po_flags & UTP_PO_ENCRYPTED) != 0u) {
        if (pkt->encrypt_data_size < prefix_length) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        pkt->encrypt_data_size = (uint16_t)(pkt->encrypt_data_size - prefix_length);
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
