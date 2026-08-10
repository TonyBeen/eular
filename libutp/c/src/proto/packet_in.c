#include "proto/packet_in.h"

#include <limits.h>

utp_internal_error_t utp_packet_in_pool_init(utp_packet_in_pool_t* pool, const utp_allocator_t* allocator,
                                             size_t packet_capacity, uint16_t buffer_capacity)
{
    if (pool == NULL || packet_capacity == 0u || buffer_capacity == 0u ||
        packet_capacity > SIZE_MAX / sizeof(*pool->packets) || packet_capacity > SIZE_MAX / buffer_capacity) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    const utp_allocator_t* resolved_allocator = utp_allocator_resolve(allocator);
    if (resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pool->allocator       = resolved_allocator;
    pool->packets         = NULL;
    pool->storage         = NULL;
    pool->packet_capacity = packet_capacity;
    pool->buffer_capacity = buffer_capacity;
    pool->packets         = utp_allocator_alloc(pool->allocator, packet_capacity * sizeof(*pool->packets));
    if (pool->packets == NULL) {
        utp_packet_in_pool_cleanup(pool);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    pool->storage = utp_allocator_alloc(pool->allocator, packet_capacity * (size_t)buffer_capacity);
    if (pool->storage == NULL) {
        utp_packet_in_pool_cleanup(pool);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    for (size_t index = 0u; index < packet_capacity; ++index) {
        pool->packets[index].data      = pool->storage + index * (size_t)buffer_capacity;
        pool->packets[index].length    = 0u;
        pool->packets[index].capacity  = buffer_capacity;
        pool->packets[index].ref_count = 0u;
        pool->packets[index].in_use    = false;
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_in_pool_cleanup(utp_packet_in_pool_t* pool)
{
    if (pool == NULL) {
        return;
    }
    if (pool->storage != NULL) {
        utp_allocator_free(pool->allocator, pool->storage);
    }
    if (pool->packets != NULL) {
        utp_allocator_free(pool->allocator, pool->packets);
    }
    pool->allocator       = NULL;
    pool->packets         = NULL;
    pool->storage         = NULL;
    pool->packet_capacity = 0u;
    pool->buffer_capacity = 0u;
}

utp_internal_error_t utp_packet_in_pool_acquire(utp_packet_in_pool_t* pool, utp_packet_in_t** out_packet)
{
    if (out_packet != NULL) {
        *out_packet = NULL;
    }
    if (pool == NULL || out_packet == NULL || pool->packets == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (size_t index = 0u; index < pool->packet_capacity; ++index) {
        if (!pool->packets[index].in_use) {
            pool->packets[index].length    = 0u;
            pool->packets[index].ref_count = 1u;
            pool->packets[index].in_use    = true;
            *out_packet                    = &pool->packets[index];
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    return UTP_INTERNAL_ERROR_LIMIT;
}

bool utp_packet_in_ref(utp_packet_in_t* packet)
{
    if (packet == NULL || !packet->in_use || packet->ref_count == UINT16_MAX) {
        return false;
    }
    ++packet->ref_count;
    return true;
}

void utp_packet_in_release(utp_packet_in_t* packet)
{
    if (packet == NULL || !packet->in_use || packet->ref_count == 0u) {
        return;
    }
    --packet->ref_count;
    if (packet->ref_count == 0u) {
        packet->length = 0u;
        packet->in_use = false;
    }
}
