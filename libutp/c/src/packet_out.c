#include "internal/packet_out.h"

#include <limits.h>
#include <string.h>

static void bucket_cleanup(utp_packet_out_bucket_t *bucket, const utp_allocator_t *allocator) {
    if (bucket->nodes != NULL) {
        utp_allocator_free(allocator, bucket->nodes);
    }
    if (bucket->storage != NULL) {
        utp_allocator_free(allocator, bucket->storage);
    }
    memset(bucket, 0, sizeof(*bucket));
}

static utp_internal_error_t bucket_init(utp_packet_out_bucket_t *bucket, const utp_allocator_t *allocator,
                                        uint16_t size, size_t count) {
    size_t i;

    memset(bucket, 0, sizeof(*bucket));
    TAILQ_INIT(&bucket->free_buffers);
    bucket->size  = size;
    bucket->count = count;

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
    memset(pool, 0, sizeof(*pool));
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
    memset(pool, 0, sizeof(*pool));

    if (struct_capacity == 0u || struct_capacity > SIZE_MAX / sizeof(utp_packet_out_t) || buckets == NULL ||
        bucket_count == 0u || bucket_count > UTP_PACKET_OUT_MAX_BUCKETS) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < bucket_count; ++i) {
        if (buckets[i].size == 0u || buckets[i].count == 0u) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }

    memcpy(sorted, buckets, bucket_count * sizeof(*buckets));
    for (i = 1u; i < bucket_count; ++i) {
        for (j = i; j > 0u && sorted[j - 1u].size > sorted[j].size; --j) {
            utp_packet_out_bucket_config_t tmp = sorted[j];

            sorted[j]     = sorted[j - 1u];
            sorted[j - 1u] = tmp;
        }
    }

    pool->allocator = utp_allocator_resolve(allocator);
    if (pool->allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pool->structs   = utp_allocator_alloc(pool->allocator, struct_capacity * sizeof(utp_packet_out_t));
    if (pool->structs == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    memset(pool->structs, 0, struct_capacity * sizeof(utp_packet_out_t));
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

utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size,
                                                 utp_packet_out_t **out) {
    size_t                         bucket_index;
    utp_packet_out_bucket_t       *bucket;
    utp_packet_out_buffer_node_t  *node;
    utp_packet_out_t              *pkt;

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

    memset(pkt, 0, sizeof(*pkt));
    pkt->loss_chain   = pkt;
    pkt->raw_data     = node->data;
    pkt->encrypt_data = node->data;
    pkt->alloc_size   = bucket->size;
    pkt->bucket_index = bucket_index;

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

    memset(pkt, 0, sizeof(*pkt));
    pkt->loss_chain   = pkt;
    pkt->raw_data     = raw_data;
    pkt->encrypt_data = encrypt_data;
    pkt->alloc_size   = alloc_size;
    pkt->bucket_index = bucket_index;
    TAILQ_INSERT_TAIL(&pool->free_structs, pkt, po_next);

    node_index = (size_t)(raw_data - bucket->storage) / bucket->size;
    TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[node_index], link);
}
