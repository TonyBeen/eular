#ifndef EULAR_UTP_INTERNAL_PACKET_IN_H
#define EULAR_UTP_INTERNAL_PACKET_IN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/allocator.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_packet_in {
    uint8_t* data;
    uint16_t length;
    uint16_t capacity;
    uint16_t ref_count;
    bool     in_use;
} utp_packet_in_t;

typedef struct utp_packet_in_pool {
    const utp_allocator_t* allocator;
    utp_packet_in_t*       packets;
    uint8_t*               storage;
    size_t                 packet_capacity;
    uint16_t               buffer_capacity;
} utp_packet_in_pool_t;

utp_internal_error_t utp_packet_in_pool_init(utp_packet_in_pool_t* pool, const utp_allocator_t* allocator,
                                             size_t packet_capacity, uint16_t buffer_capacity);
void                 utp_packet_in_pool_cleanup(utp_packet_in_pool_t* pool);
utp_internal_error_t utp_packet_in_pool_acquire(utp_packet_in_pool_t* pool, utp_packet_in_t** out_packet);
bool                 utp_packet_in_ref(utp_packet_in_t* packet);
void                 utp_packet_in_release(utp_packet_in_t* packet);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_PACKET_IN_H
