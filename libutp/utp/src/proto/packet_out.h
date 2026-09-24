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
#define UTP_PACKET_OUT_GROW_COUNT  32u

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
struct utp_packet_out_buffer_block;

typedef struct utp_frame_meta_info {
    void*    owner;        // 语义控制槽或帧所属对象，不拥有
    uint64_t value;        // 帧携带的语义值
    uint16_t offset;       // 帧在 raw_data 中的起始偏移
    uint16_t length;       // 帧编码长度
    uint32_t generation;   // 所属控制槽的版本号
    uint8_t  frame_type;   // 帧类型
    uint8_t  frame_flags;  // FIN、重传剥离等元数据标志
} utp_frame_meta_info_t;

typedef struct utp_packet_out_slice {
    uint16_t    offset;  // RAW slice 在 raw_data 中的偏移
    uint16_t    length;  // slice 长度
    const void* data;    // EXTERNAL slice 的借用数据指针
    uint8_t     source;  // RAW_OFFSET 或 EXTERNAL
} utp_packet_out_slice_t;

struct utp_packet_out;
TAILQ_HEAD(utp_packet_out_tailq, utp_packet_out);

typedef struct utp_packet_out {
    TAILQ_ENTRY(utp_packet_out) po_next;  // 发送队列链表节点

    uint64_t                           sent_time_us;   // 最近一次实际发送时刻，单位 us
    uint64_t                           packet_number;  // 当前发送尝试的包号
    uint64_t                           ack_number;     // 包构造时携带的最大 ACK 包号
    struct utp_packet_out*             loss_chain;     // 同一逻辑包的丢失重传链

    uint32_t                           frame_types;  // 包内帧类型位图
    uint16_t                           po_flags;     // 发送包协议状态标志
    uint16_t                           local_flags;  // 本地调度与跟踪标志

    uint16_t                           data_size;                   // 当前明文包长度
    uint16_t                           encrypt_data_size;           // 加密后 payload 长度
    uint16_t                           alloc_size;                  // raw_data 缓冲实际容量
    uint8_t                            packet_type;                 // utp_packet_type_t
    uint8_t                            slice_count;                 // 有效 slice 数量
    uint8_t                            frame_meta_count;            // 有效帧元数据数量
    uint32_t                           stream_data_size;            // 此包 STREAM 数据长度
    uint32_t                           path_validation_generation;  // 所属候选路径验证代次
    uint16_t                           transient_ack_size;          // 重传时应剥离的 ACK 前缀长度
    uint16_t                           control_prefix_size;         // 重传时可剥离的 transient 控制前缀长度
    // 半加密 0-RTT 中保持明文的 SESSION_TOKEN payload 长度。
    uint16_t                           early_plaintext_prefix_size;  // 0-RTT 保持明文的票据前缀长度
    uint32_t                           stream_id;                    // STREAM 帧所属流 ID
    uint64_t                           stream_offset;                // STREAM 数据起始偏移

    utp_packet_out_slice_t             slices[UTP_PACKET_OUT_MAX_SLICES];      // 零拷贝发送片段
    utp_frame_meta_info_t              frame_meta[UTP_PACKET_OUT_MAX_FRAMES];  // 帧生命周期元数据
    // 发送控制器持有的历史发送尝试链；每个包号都可被迟到 ACK 直接定位。
    struct utp_send_attempt_node*      attempts;       // 历史包号索引链
    uint32_t                           attempt_count;  // 历史发送次数

    utp_bw_packet_state_t              bw_packet_state;  // 带宽采样快照
    void*                              bw_state;         // 拥塞算法附加状态，不拥有

    uint8_t*                           raw_data;         // 包头和内联帧缓冲
    uint8_t*                           encrypt_data;     // AEAD 输出缓冲，可为空
    utp_address_t                      destination;      // 候选路径时的显式目标地址
    bool                               has_destination;  // 是否使用 destination 而非连接当前 peer

    /* 池记账字段:acquire 时记录该对象缓冲区来自哪个桶,release 时用于 O(1) 归还,
       不属于协议/发送语义,不出现在 docs 的字段表里。 */
    size_t                             bucket_index;  // raw_data 所属内存桶索引
    struct utp_packet_out_buffer_node* buffer_node;   // raw_data 对应的共享缓冲节点
} utp_packet_out_t;

typedef struct utp_packet_out_buffer_node {
    TAILQ_ENTRY(utp_packet_out_buffer_node) link;  // 空闲缓冲链表节点
    struct utp_packet_out_buffer_block* block;     // 所属扩容块，不拥有
    uint8_t*                            data;      // 桶内缓冲地址
} utp_packet_out_buffer_node_t;
TAILQ_HEAD(utp_packet_out_buffer_node_tailq, utp_packet_out_buffer_node);

typedef struct utp_packet_out_buffer_block {
    TAILQ_ENTRY(utp_packet_out_buffer_block) link;  // 此桶的扩容块链表节点
    size_t                       free_count;
    utp_packet_out_buffer_node_t nodes[UTP_PACKET_OUT_GROW_COUNT];
    uint8_t*                     storage;  // 指向同次分配的尾部存储，不拥有
} utp_packet_out_buffer_block_t;
TAILQ_HEAD(utp_packet_out_buffer_block_tailq, utp_packet_out_buffer_block);

typedef struct utp_packet_out_bucket {
    uint16_t                                 size;  // 单块缓冲容量
    size_t                                   allocated_count;
    size_t                                   in_use_count;
    size_t                                   sample_calls;
    size_t                                   sample_max_in_use;
    size_t                                   sample_max_average;
    struct utp_packet_out_buffer_node_tailq  free_buffers;  // 当前可借出的空闲缓冲队列
    struct utp_packet_out_buffer_block_tailq blocks;        // 所有扩容块
} utp_packet_out_bucket_t;

typedef struct utp_packet_out_buffer_pool {
    const utp_allocator_t*  allocator;                            // Context 所有，不拥有
    utp_packet_out_bucket_t buckets[UTP_PACKET_OUT_MAX_BUCKETS];  // 固定大小分桶
    size_t                  bucket_count;                         // 固定为五档
} utp_packet_out_buffer_pool_t;

typedef struct utp_packet_out_block {
    TAILQ_ENTRY(utp_packet_out_block) link;  // Connection 私有描述符块
    utp_packet_out_t packets[UTP_PACKET_OUT_GROW_COUNT];
} utp_packet_out_block_t;
TAILQ_HEAD(utp_packet_out_block_tailq, utp_packet_out_block);

typedef struct utp_packet_out_pool {
    const utp_allocator_t*            allocator;        // Connection 所有，不拥有
    size_t                            allocated_count;  // 已分配 PacketOut 数量
    struct utp_packet_out_tailq       free_structs;     // 当前可借出的 PacketOut 队列
    struct utp_packet_out_block_tailq blocks;           // 所有描述符扩容块
} utp_packet_out_pool_t;

utp_internal_error_t utp_packet_out_buffer_pool_init(utp_packet_out_buffer_pool_t* pool,
                                                     const utp_allocator_t*        allocator);
void                 utp_packet_out_buffer_pool_cleanup(utp_packet_out_buffer_pool_t* pool);
utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t* pool, const utp_allocator_t* allocator);
void                 utp_packet_out_pool_cleanup(utp_packet_out_pool_t* pool);
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t* pool, utp_packet_out_buffer_pool_t* buffer_pool,
                                                 uint16_t requested_size, utp_packet_out_t** out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t* pool, utp_packet_out_buffer_pool_t* buffer_pool,
                                                 utp_packet_out_t* pkt);

utp_internal_error_t utp_packet_out_strip_prefix(utp_packet_out_t* pkt, uint16_t prefix_length);
utp_internal_error_t utp_packet_out_flatten(const utp_packet_out_t* pkt, uint8_t* buffer, size_t capacity,
                                            size_t* out_length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_PACKET_OUT_H
