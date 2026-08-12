#ifndef EULAR_UTP_INTERNAL_PACKET_OUT_H
#define EULAR_UTP_INTERNAL_PACKET_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "congestion/bw_sampler.h"
#include "queue.h"
#include "socket/address.h"
#include "util/allocator.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_PACKET_OUT_MAX_FRAMES  8u
#define UTP_PACKET_OUT_MAX_SLICES  8u
#define UTP_PACKET_OUT_MAX_BUCKETS 8u

#define UTP_PO_HELLO             0x0001u
#define UTP_PO_ENCRYPTED         0x0002u
#define UTP_PO_RESET_PACKNO      0x0004u
#define UTP_PO_NO_ENCRYPT        0x0008u
#define UTP_PO_MTU_PROBE         0x0010u
#define UTP_PO_UNACKED           0x0020u
#define UTP_PO_SCHED             0x0040u
#define UTP_PO_LOST              0x0080u
#define UTP_PO_LOSS_RECORDED     0x0100u
#define UTP_PO_KEEP_PLAINTEXT    0x0200u
#define UTP_PO_PATH_VALIDATION   0x0400u
#define UTP_PO_EARLY_ENCRYPTED   0x1000u
#define UTP_PO_ZERO_RTT_RESPONSE 0x2000u

#define UTP_POL_LOSS             0x0001u
#define UTP_POL_LIMITED          0x0002u
#define UTP_POL_FACKED           0x0004u
#define UTP_POL_TRACK_ON_SEND    0x0008u
#define UTP_POL_NO_TRACK_ON_SEND 0x0010u

#define UTP_FRAME_META_FIN                     0x01u
#define UTP_FRAME_META_TRANSIENT_ON_RETRANSMIT 0x02u
#define UTP_FRAME_META_SEMANTIC_CONTROL        0x04u

#define UTP_PACKET_OUT_SLICE_RAW_OFFSET 0u
#define UTP_PACKET_OUT_SLICE_EXTERNAL   1u

struct utp_send_attempt_node;

typedef struct utp_frame_meta_info {
    void*    owner;
    uint64_t value;
    uint16_t offset;
    uint16_t length;
    uint32_t generation;
    uint8_t  frame_type;
    uint8_t  frame_flags;
} utp_frame_meta_info_t;

typedef struct utp_packet_out_slice {
    uint16_t    offset;
    uint16_t    length;
    const void* data;
    uint8_t     source;
} utp_packet_out_slice_t;

struct utp_packet_out;
TAILQ_HEAD(utp_packet_out_tailq, utp_packet_out);

typedef struct utp_packet_out {
    TAILQ_ENTRY(utp_packet_out) po_next;

    uint64_t                      sent_time_us;
    uint64_t                      packet_number;
    uint64_t                      ack_number;
    struct utp_packet_out*        loss_chain;

    uint32_t                      frame_types;
    uint16_t                      po_flags;
    uint16_t                      local_flags;

    uint16_t                      data_size;
    uint16_t                      encrypt_data_size;
    uint16_t                      alloc_size;
    uint8_t                       packet_type;
    uint8_t                       slice_count;
    uint8_t                       frame_meta_count;
    uint32_t                      stream_data_size;
    uint32_t                      path_validation_generation;
    uint16_t                      transient_ack_size;
    uint16_t                      control_prefix_size;
    // 半加密 0-RTT 中保持明文的 SESSION_TOKEN payload 长度。
    uint16_t                      early_plaintext_prefix_size;
    uint32_t                      stream_id;
    uint64_t                      stream_offset;

    utp_packet_out_slice_t        slices[UTP_PACKET_OUT_MAX_SLICES];
    utp_frame_meta_info_t         frame_meta[UTP_PACKET_OUT_MAX_FRAMES];
    // 发送控制器持有的历史发送尝试链；每个包号都可被迟到 ACK 直接定位。
    struct utp_send_attempt_node* attempts;
    uint32_t                      attempt_count;

    utp_bw_packet_state_t         bw_packet_state;
    void*                         bw_state;

    uint8_t*                      raw_data;
    uint8_t*                      encrypt_data;
    utp_address_t                 destination;
    bool                          has_destination;

    /* 池记账字段:acquire 时记录该对象缓冲区来自哪个桶,release 时用于 O(1) 归还,
       不属于协议/发送语义,不出现在 docs 的字段表里。 */
    size_t                        bucket_index;
} utp_packet_out_t;

typedef struct utp_packet_out_bucket_config {
    uint16_t size;
    size_t   count;
} utp_packet_out_bucket_config_t;

typedef struct utp_packet_out_buffer_node {
    TAILQ_ENTRY(utp_packet_out_buffer_node) link;
    uint8_t* data;
} utp_packet_out_buffer_node_t;
TAILQ_HEAD(utp_packet_out_buffer_node_tailq, utp_packet_out_buffer_node);

typedef struct utp_packet_out_bucket {
    uint16_t                                size;
    size_t                                  count;
    uint8_t*                                storage;
    utp_packet_out_buffer_node_t*           nodes;
    struct utp_packet_out_buffer_node_tailq free_buffers;
} utp_packet_out_bucket_t;

typedef struct utp_packet_out_pool {
    const utp_allocator_t*      allocator;
    utp_packet_out_t*           structs;
    size_t                      struct_capacity;
    struct utp_packet_out_tailq free_structs;
    utp_packet_out_bucket_t     buckets[UTP_PACKET_OUT_MAX_BUCKETS];
    size_t                      bucket_count;
} utp_packet_out_pool_t;

utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t* pool, const utp_allocator_t* allocator,
                                              size_t struct_capacity, const utp_packet_out_bucket_config_t* buckets,
                                              size_t bucket_count);
void                 utp_packet_out_pool_cleanup(utp_packet_out_pool_t* pool);
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t* pool, uint16_t requested_size,
                                                 utp_packet_out_t** out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t* pool, utp_packet_out_t* pkt);

utp_internal_error_t utp_packet_out_strip_prefix(utp_packet_out_t* pkt, uint16_t prefix_length);
utp_internal_error_t utp_packet_out_flatten(const utp_packet_out_t* pkt, uint8_t* buffer, size_t capacity,
                                            size_t* out_length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_PACKET_OUT_H
