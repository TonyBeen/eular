#include "proto/packet_out.h"

#include <limits.h>
#include <string.h>

static void bucket_cleanup(utp_packet_out_bucket_t *bucket, const utp_allocator_t *allocator) {
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

static utp_internal_error_t bucket_init(utp_packet_out_bucket_t *bucket, const utp_allocator_t *allocator,
                                        uint16_t size, size_t count) {
    size_t i;

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
    for (i = 0u; i < count; ++i) {
        bucket->nodes[i].data = bucket->storage + i * (size_t)size;
        TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[i], link);
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_cleanup(utp_packet_out_pool_t *pool) {
    size_t i;

    if (pool == NULL) {
        return;
    }
    for (i = 0u; i < pool->bucket_count; ++i) {
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

utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool, const utp_allocator_t *allocator,
                                              size_t struct_capacity, const utp_packet_out_bucket_config_t *buckets,
                                              size_t bucket_count) {
    utp_packet_out_bucket_config_t sorted[UTP_PACKET_OUT_MAX_BUCKETS];
    size_t                         i;
    size_t                         j;

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
    for (i = 0u; i < bucket_count; ++i) {
        if (buckets[i].size == 0u || buckets[i].count == 0u) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }

    for (i = 0u; i < bucket_count; ++i) {
        sorted[i] = buckets[i];
    }
    for (i = 1u; i < bucket_count; ++i) {
        for (j = i; j > 0u && sorted[j - 1u].size > sorted[j].size; --j) {
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
    for (i = 0u; i < struct_capacity; ++i) {
        pool->structs[i].loss_chain = &pool->structs[i];
        TAILQ_INSERT_TAIL(&pool->free_structs, &pool->structs[i], po_next);
    }

    for (i = 0u; i < bucket_count; ++i) {
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

static size_t choose_bucket(const utp_packet_out_pool_t *pool, uint16_t requested_size) {
    size_t i;

    for (i = 0u; i < pool->bucket_count; ++i) {
        if (pool->buckets[i].size >= requested_size) {
            return i;
        }
    }
    return pool->bucket_count;
}

static void reset_packet_out_for_acquire(utp_packet_out_t *pkt, utp_packet_out_buffer_node_t *node,
                                         const utp_packet_out_bucket_t *bucket, size_t bucket_index) {
    uint16_t index;

    pkt->sent_time_us          = 0u;
    pkt->packet_number         = 0u;
    pkt->ack_number            = 0u;
    pkt->loss_chain            = pkt;
    pkt->frame_types           = 0u;
    pkt->po_flags              = 0u;
    pkt->local_flags           = 0u;
    pkt->data_size             = 0u;
    pkt->encrypt_data_size     = 0u;
    pkt->alloc_size            = bucket->size;
    pkt->packet_type           = 0u;
    pkt->slice_count           = 0u;
    pkt->frame_meta_count      = 0u;
    pkt->stream_data_size      = 0u;
    pkt->transient_ack_size    = 0u;
    pkt->stream_id             = 0u;
    pkt->stream_offset         = 0u;
    pkt->attempt_count         = 0u;
    pkt->bw_packet_state.valid = false;
    pkt->bw_state              = NULL;
    pkt->raw_data              = node->data;
    pkt->encrypt_data          = node->data;
    pkt->bucket_index          = bucket_index;
    for (index = 0u; index < UTP_PACKET_OUT_MAX_ATTEMPTS; ++index) {
        pkt->attempts[index].packet_number = 0u;
        pkt->attempts[index].sent_time_us  = 0u;
    }
}

utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size,
                                                 utp_packet_out_t **out) {
    size_t                        bucket_index;
    utp_packet_out_bucket_t      *bucket;
    utp_packet_out_buffer_node_t *node;
    utp_packet_out_t             *pkt;

    if (pool == NULL || out == NULL || requested_size == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    bucket_index = choose_bucket(pool, requested_size);
    if (bucket_index == pool->bucket_count) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    bucket = &pool->buckets[bucket_index];
    if (TAILQ_EMPTY(&bucket->free_buffers) || TAILQ_EMPTY(&pool->free_structs)) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }

    node = TAILQ_FIRST(&bucket->free_buffers);
    TAILQ_REMOVE(&bucket->free_buffers, node, link);

    pkt = TAILQ_FIRST(&pool->free_structs);
    TAILQ_REMOVE(&pool->free_structs, pkt, po_next);

    reset_packet_out_for_acquire(pkt, node, bucket, bucket_index);

    *out = pkt;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_release(utp_packet_out_pool_t *pool, utp_packet_out_t *pkt) {
    utp_packet_out_bucket_t *bucket;
    uint8_t                 *raw_data;
    uint8_t                 *encrypt_data;
    uint16_t                 alloc_size;
    size_t                   bucket_index;
    size_t                   node_index;

    if (pool == NULL || pkt == NULL) {
        return;
    }

    raw_data     = pkt->raw_data;
    encrypt_data = pkt->encrypt_data;
    alloc_size   = pkt->alloc_size;
    bucket_index = pkt->bucket_index;
    bucket       = &pool->buckets[bucket_index];

    pkt->sent_time_us          = 0u;
    pkt->packet_number         = 0u;
    pkt->ack_number            = 0u;
    pkt->loss_chain            = pkt;
    pkt->frame_types           = 0u;
    pkt->po_flags              = 0u;
    pkt->local_flags           = 0u;
    pkt->data_size             = 0u;
    pkt->encrypt_data_size     = 0u;
    pkt->alloc_size            = alloc_size;
    pkt->packet_type           = 0u;
    pkt->slice_count           = 0u;
    pkt->frame_meta_count      = 0u;
    pkt->stream_data_size      = 0u;
    pkt->transient_ack_size    = 0u;
    pkt->stream_id             = 0u;
    pkt->stream_offset         = 0u;
    pkt->attempt_count         = 0u;
    pkt->bw_packet_state.valid = false;
    pkt->bw_state              = NULL;
    pkt->raw_data              = raw_data;
    pkt->encrypt_data          = encrypt_data;
    pkt->bucket_index          = bucket_index;
    TAILQ_INSERT_TAIL(&pool->free_structs, pkt, po_next);

    node_index = (size_t)(raw_data - bucket->storage) / bucket->size;
    TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[node_index], link);
}

bool utp_packet_out_add_send_attempt(utp_packet_out_t *pkt, uint64_t packet_number, uint64_t sent_time_us) {
    if (pkt == NULL || packet_number == 0u || sent_time_us == 0u || pkt->attempt_count >= UTP_PACKET_OUT_MAX_ATTEMPTS) {
        return false;
    }
    pkt->attempts[pkt->attempt_count].packet_number = packet_number;
    pkt->attempts[pkt->attempt_count].sent_time_us  = sent_time_us;
    ++pkt->attempt_count;
    return true;
}

void utp_packet_out_clear_send_attempts(utp_packet_out_t *pkt) {
    if (pkt != NULL) {
        uint16_t index;

        for (index = 0u; index < UTP_PACKET_OUT_MAX_ATTEMPTS; ++index) {
            pkt->attempts[index].packet_number = 0u;
            pkt->attempts[index].sent_time_us  = 0u;
        }
        pkt->attempt_count = 0u;
    }
}

static utp_internal_error_t utp_packet_out_resolve_slice(const utp_packet_out_t       *pkt,
                                                         const utp_packet_out_slice_t *slice, const uint8_t **data) {
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

utp_internal_error_t utp_packet_out_flatten(const utp_packet_out_t *pkt, uint8_t *buffer, size_t capacity,
                                            size_t *out_length) {
    size_t  offset = 0u;
    uint8_t index;

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
    for (index = 0u; index < pkt->slice_count; ++index) {
        const uint8_t       *data;
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
