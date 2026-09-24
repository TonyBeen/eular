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

typedef struct utp_packet_in_pool utp_packet_in_pool_t;

typedef struct utp_packet_in {
    uint8_t*                    data;       // 池分配的可变 UDP 数据缓冲
    uint16_t                    length;     // 当前有效数据长度
    uint16_t                    capacity;   // 数据缓冲容量
    uint16_t                    ref_count;  // 流重组持有的引用计数
    bool                        in_use;     // 是否已从池中借出
    struct utp_packet_in*       free_next;  // 空闲链表中的下一项
    utp_packet_in_pool_t*       pool;       // 所属 PacketIn 池，不拥有
    struct utp_packet_in_block* block;      // 所属稳定分配块，不拥有
} utp_packet_in_t;

typedef struct utp_packet_in_block {
    struct utp_packet_in_block* next;             // 下一个稳定分配块
    utp_packet_in_t*            packets;          // 本块 PacketIn 描述符数组
    uint8_t*                    storage;          // 本块连续数据缓冲区
    size_t                      packet_capacity;  // 本块描述符数量
    size_t                      free_count;       // 本块当前空闲描述符数量
} utp_packet_in_block_t;

struct utp_packet_in_pool {
    const utp_allocator_t* allocator;          // 内存分配器，不拥有
    utp_packet_in_block_t* blocks;             // 已分配块链表
    utp_packet_in_t*       free_packets;       // 可借用 PacketIn 空闲链表
    size_t                 packet_capacity;    // 当前已分配描述符总数
    size_t                 free_count;         // 当前空闲描述符总数
    size_t                 grow_capacity;      // 每次扩容的描述符数量
    size_t                 block_capacity;     // 每个稳定分配块的描述符数量
    size_t                 max_free_capacity;  // 空闲描述符缓存水位
    uint16_t               buffer_capacity;    // 每个数据缓冲容量
    bool                   dynamic;            // 是否允许按扩容策略增长
};

utp_internal_error_t utp_packet_in_pool_init(utp_packet_in_pool_t* pool, const utp_allocator_t* allocator,
                                             size_t packet_capacity, uint16_t buffer_capacity);
utp_internal_error_t utp_packet_in_pool_init_dynamic(utp_packet_in_pool_t* pool, const utp_allocator_t* allocator,
                                                     size_t grow_capacity, size_t block_capacity,
                                                     size_t max_free_capacity, uint16_t buffer_capacity);
void                 utp_packet_in_pool_cleanup(utp_packet_in_pool_t* pool);
utp_internal_error_t utp_packet_in_pool_reserve(utp_packet_in_pool_t* pool, size_t packet_count);
utp_internal_error_t utp_packet_in_pool_acquire_many(utp_packet_in_pool_t* pool, utp_packet_in_t** out_packets,
                                                     size_t packet_count);
utp_internal_error_t utp_packet_in_pool_acquire(utp_packet_in_pool_t* pool, utp_packet_in_t** out_packet);
bool                 utp_packet_in_ref(utp_packet_in_t* packet);
void                 utp_packet_in_release(utp_packet_in_t* packet);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_PACKET_IN_H
