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
    uint8_t* data;       // 池分配的可变 UDP 数据缓冲
    uint16_t length;     // 当前有效数据长度
    uint16_t capacity;   // 数据缓冲容量
    uint16_t ref_count;  // 流重组持有的引用计数
    bool     in_use;     // 是否已从池中借出
} utp_packet_in_t;

typedef struct utp_packet_in_pool {
    const utp_allocator_t* allocator;        // 内存分配器，不拥有
    utp_packet_in_t*       packets;          // PacketIn 描述符数组
    uint8_t*               storage;          // 连续数据缓冲区
    size_t                 packet_capacity;  // 描述符数量
    uint16_t               buffer_capacity;  // 每个数据缓冲容量
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
