#include "proto/packet_in.h"

#include <assert.h>
#include <limits.h>

static void utp_packet_in_pool_free_block(utp_packet_in_pool_t* pool, utp_packet_in_block_t* block)
{
    assert(pool != NULL);
    assert(block != NULL);
    utp_allocator_free(pool->allocator, block->storage);
    utp_allocator_free(pool->allocator, block->packets);
    utp_allocator_free(pool->allocator, block);
}

static void                 utp_packet_in_pool_trim(utp_packet_in_pool_t* pool);

static utp_internal_error_t utp_packet_in_pool_add_block(utp_packet_in_pool_t* pool, size_t packet_capacity)
{
    assert(pool != NULL);
    assert(pool->allocator != NULL);
    assert(pool->buffer_capacity != 0u);
    if (packet_capacity == 0u || packet_capacity > SIZE_MAX / sizeof(utp_packet_in_t) ||
        packet_capacity > SIZE_MAX / pool->buffer_capacity || pool->packet_capacity > SIZE_MAX - packet_capacity) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_packet_in_block_t* block = utp_allocator_alloc(pool->allocator, sizeof(*block));
    if (block == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    block->next            = NULL;
    block->packets         = utp_allocator_alloc(pool->allocator, packet_capacity * sizeof(*block->packets));
    block->storage         = NULL;
    block->packet_capacity = packet_capacity;
    block->free_count      = 0u;
    if (block->packets == NULL) {
        utp_allocator_free(pool->allocator, block);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    block->storage = utp_allocator_alloc(pool->allocator, packet_capacity * (size_t)pool->buffer_capacity);
    if (block->storage == NULL) {
        utp_allocator_free(pool->allocator, block->packets);
        utp_allocator_free(pool->allocator, block);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    for (size_t index = 0u; index < packet_capacity; ++index) {
        utp_packet_in_t* packet = &block->packets[index];

        packet->data       = block->storage + index * (size_t)pool->buffer_capacity;
        packet->length     = 0u;
        packet->capacity   = pool->buffer_capacity;
        packet->ref_count  = 0u;
        packet->in_use     = false;
        packet->pool       = pool;
        packet->block      = block;
        packet->free_next  = pool->free_packets;
        pool->free_packets = packet;
        ++block->free_count;
        ++pool->free_count;
    }
    block->next            = pool->blocks;
    pool->blocks           = block;
    pool->packet_capacity += packet_capacity;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_packet_in_pool_grow(utp_packet_in_pool_t* pool)
{
    assert(pool != NULL);
    assert(pool->grow_capacity != 0u);
    assert(pool->block_capacity != 0u);
    size_t               remaining = pool->grow_capacity;
    utp_internal_error_t error     = UTP_INTERNAL_ERROR_OK;

    while (remaining > 0u) {
        const size_t block_capacity = remaining < pool->block_capacity ? remaining : pool->block_capacity;

        error = utp_packet_in_pool_add_block(pool, block_capacity);
        if (error != UTP_INTERNAL_ERROR_OK) {
            break;
        }
        remaining -= block_capacity;
    }
    return error;
}

utp_internal_error_t utp_packet_in_pool_init_dynamic(utp_packet_in_pool_t* pool, const utp_allocator_t* allocator,
                                                     size_t grow_capacity, size_t block_capacity,
                                                     size_t max_free_capacity, uint16_t buffer_capacity)
{
    if (pool == NULL || grow_capacity == 0u || block_capacity == 0u || grow_capacity < block_capacity ||
        grow_capacity % block_capacity != 0u || max_free_capacity == 0u || buffer_capacity == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    const utp_allocator_t* resolved_allocator = utp_allocator_resolve(allocator);
    if (resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pool->allocator                  = resolved_allocator;
    pool->blocks                     = NULL;
    pool->free_packets               = NULL;
    pool->packet_capacity            = 0u;
    pool->free_count                 = 0u;
    pool->grow_capacity              = grow_capacity;
    pool->block_capacity             = block_capacity;
    pool->max_free_capacity          = max_free_capacity;
    pool->buffer_capacity            = buffer_capacity;
    pool->dynamic                    = true;
    const utp_internal_error_t error = utp_packet_in_pool_grow(pool);

    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_packet_in_pool_cleanup(pool);
    } else {
        utp_packet_in_pool_trim(pool);
    }
    return error;
}

utp_internal_error_t utp_packet_in_pool_init(utp_packet_in_pool_t* pool, const utp_allocator_t* allocator,
                                             size_t packet_capacity, uint16_t buffer_capacity)
{
    if (pool == NULL || packet_capacity == 0u || buffer_capacity == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    const utp_allocator_t* resolved_allocator = utp_allocator_resolve(allocator);
    if (resolved_allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pool->allocator                  = resolved_allocator;
    pool->blocks                     = NULL;
    pool->free_packets               = NULL;
    pool->packet_capacity            = 0u;
    pool->free_count                 = 0u;
    pool->grow_capacity              = packet_capacity;
    pool->block_capacity             = packet_capacity;
    pool->max_free_capacity          = packet_capacity;
    pool->buffer_capacity            = buffer_capacity;
    pool->dynamic                    = false;
    const utp_internal_error_t error = utp_packet_in_pool_add_block(pool, packet_capacity);

    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_packet_in_pool_cleanup(pool);
    }
    return error;
}

void utp_packet_in_pool_cleanup(utp_packet_in_pool_t* pool)
{
    if (pool == NULL) {
        return;
    }
    utp_packet_in_block_t* block = pool->blocks;
    while (block != NULL) {
        utp_packet_in_block_t* next = block->next;

        utp_packet_in_pool_free_block(pool, block);
        block = next;
    }
    pool->allocator         = NULL;
    pool->blocks            = NULL;
    pool->free_packets      = NULL;
    pool->packet_capacity   = 0u;
    pool->free_count        = 0u;
    pool->grow_capacity     = 0u;
    pool->block_capacity    = 0u;
    pool->max_free_capacity = 0u;
    pool->buffer_capacity   = 0u;
    pool->dynamic           = false;
}

utp_internal_error_t utp_packet_in_pool_reserve(utp_packet_in_pool_t* pool, size_t packet_count)
{
    if (pool == NULL || pool->allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    while (pool->free_count < packet_count) {
        if (!pool->dynamic) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        const utp_internal_error_t error = utp_packet_in_pool_grow(pool);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_packet_in_pool_remove_block_free_packets(utp_packet_in_pool_t* pool, const utp_packet_in_block_t* block)
{
    assert(pool != NULL);
    assert(block != NULL);
    utp_packet_in_t** link = &pool->free_packets;

    while (*link != NULL) {
        if ((*link)->block == block) {
            *link = (*link)->free_next;
        } else {
            link = &(*link)->free_next;
        }
    }
}

static void utp_packet_in_pool_trim(utp_packet_in_pool_t* pool)
{
    assert(pool != NULL);
    utp_packet_in_block_t** link = &pool->blocks;

    while (pool->free_count > pool->max_free_capacity && *link != NULL) {
        utp_packet_in_block_t* block = *link;

        if (block->free_count != block->packet_capacity) {
            link = &block->next;
            continue;
        }
        utp_packet_in_pool_remove_block_free_packets(pool, block);
        *link                  = block->next;
        pool->packet_capacity -= block->packet_capacity;
        pool->free_count      -= block->free_count;
        utp_packet_in_pool_free_block(pool, block);
    }
}

utp_internal_error_t utp_packet_in_pool_acquire_many(utp_packet_in_pool_t* pool, utp_packet_in_t** out_packets,
                                                     size_t packet_count)
{
    if (out_packets != NULL) {
        for (size_t index = 0u; index < packet_count; ++index) {
            out_packets[index] = NULL;
        }
    }
    if (pool == NULL || out_packets == NULL || packet_count == 0u || pool->allocator == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    const utp_internal_error_t error = utp_packet_in_pool_reserve(pool, packet_count);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (size_t index = 0u; index < packet_count; ++index) {
        utp_packet_in_t* packet = pool->free_packets;

        pool->free_packets = packet->free_next;
        --pool->free_count;
        --packet->block->free_count;
        packet->free_next  = NULL;
        packet->length     = 0u;
        packet->ref_count  = 1u;
        packet->in_use     = true;
        out_packets[index] = packet;
    }
    if (pool->dynamic) {
        utp_packet_in_pool_trim(pool);
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_packet_in_pool_acquire(utp_packet_in_pool_t* pool, utp_packet_in_t** out_packet)
{
    return utp_packet_in_pool_acquire_many(pool, out_packet, 1u);
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
        packet->length             = 0u;
        packet->in_use             = false;
        packet->free_next          = packet->pool->free_packets;
        packet->pool->free_packets = packet;
        ++packet->pool->free_count;
        ++packet->block->free_count;
        if (packet->pool->dynamic) {
            utp_packet_in_pool_trim(packet->pool);
        }
    }
}
