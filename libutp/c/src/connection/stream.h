#ifndef EULAR_UTP_CONNECTION_STREAM_H
#define EULAR_UTP_CONNECTION_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/stream.h>

#include "proto/frame.h"
#include "proto/packet_in.h"
#include "util/error.h"
#include "util/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_stream utp_stream_t;
struct utp_connection;

#define UTP_STREAM_TYPES                        4u
#define UTP_STREAM_MAX_WRITE_VIEWS              2u
#define UTP_STREAM_CLIENT_INITIATED             0u
#define UTP_STREAM_SERVER_INITIATED             1u
#define UTP_STREAM_UNIDIRECTIONAL               2u
#define UTP_STREAM_RECV_FRAGMENT_LIMIT          1024u
#define UTP_STREAM_SEND_ACK_RANGE_LIMIT         16u
#define UTP_STREAM_SEND_BUFFER_CAPACITY         32768u
#define UTP_STREAM_MAX_RECV_BUFFER_BYTES        (2u * 1024u * 1024u)
#define UTP_STREAM_RECV_REASSEMBLY_MEMORY_LIMIT (4u * 1024u * 1024u)
#define UTP_STREAM_RECV_MAX_GAP                 (2u * 1024u * 1024u)
#define UTP_STREAM_DEFAULT_FLOW_WINDOW          (2u * 1024u * 1024u)

typedef struct utp_stream_recv_account {
    size_t* connection_memory_bytes;    // 连接级已占用重组内存计数
    size_t* connection_fragment_count;  // 连接级已保留分片数计数
    size_t  connection_memory_limit;    // 连接级重组内存上限
    size_t  connection_fragment_limit;  // 连接级重组分片数上限
} utp_stream_recv_account_t;

typedef struct utp_stream_recv_fragment {
    utp_packet_in_t* packet;                     // 借用数据所在 PacketIn，可为空
    const uint8_t*   data_view;                  // 分片数据零拷贝视图
    size_t*          connection_memory_bytes;    // 所属连接的内存计数器
    size_t*          connection_fragment_count;  // 所属连接的分片计数器
    uint64_t         offset;                     // 分片在流内的起始偏移
    size_t           memory_cost;                // 对连接级内存预算的计费字节数
    size_t           length;                     // 分片总长度
    size_t           consumed;                   // 已被应用消费的前缀长度
    bool             fin : 1;                    // 分片末尾是否带 FIN
    bool             accounted : 1;              // 是否已计入连接级资源计数
} utp_stream_recv_fragment_t;

typedef struct utp_stream_send_ack_range {
    uint64_t start;  // 已确认数据区间起始偏移（含）
    uint64_t end;    // 已确认数据区间结束偏移（不含）
} utp_stream_send_ack_range_t;

typedef utp_on_stream_readable_fn utp_stream_read_cb_t;
typedef utp_on_stream_writable_fn utp_stream_write_cb_t;
typedef utp_on_stream_closed_fn   utp_stream_close_cb_t;

struct utp_stream {
    // 仅由 Connection 创建的流会设置 hash_node 和 connection。
    utp_hash_node_t             hash_node;                                       // Connection 流表节点
    struct utp_connection*      connection;                                      // 所属连接，不拥有
    uint64_t*                   connection_consumed_total;                       // 连接级已消费字节累计指针
    uint32_t                    stream_id;                                       // 协议流 ID
    uint64_t                    send_buffer_offset;                              // 发送环形缓冲逻辑起始偏移
    uint64_t                    next_send_offset;                                // 下一段待发送数据的流偏移
    uint64_t                    recv_offset;                                     // 下一字节连续读取偏移
    uint64_t                    local_max_stream_offset_received;                // 已观察到的最大接收末尾偏移
    uint64_t                    local_stream_offset_consumed;                    // 已从连接接收窗口退休的流偏移
    uint64_t                    local_max_stream_offset_sent;                    // 已实际写入 UDP 的最大发送末尾偏移
    uint64_t                    peer_final_size;                                 // FIN 或 RESET 声明的对端最终偏移
    uint64_t                    peer_max_stream_data;                            // 对端通告的发送额度
    uint64_t                    local_max_stream_data_advertised;                // 本端通告的接收额度
    uint64_t                    last_max_stream_data_sent_us;                    // 上次发送 MAX_STREAM_DATA 的时刻
    uint64_t                    last_stream_data_blocked_sent_us;                // 上次发送 STREAM_DATA_BLOCKED 的时刻
    uint32_t                    drr_deficit;                                     // DRR 当前可用配额
    size_t                      send_buffer_length;                              // 发送环形缓冲有效长度
    size_t                      send_buffer_start;                               // 发送环形缓冲物理起始下标
    size_t                      send_in_flight_bytes;                            // 已构造但尚未确认的发送字节
    size_t                      recv_buffered_bytes;                             // 等待应用读取的连续或乱序字节
    size_t                      recv_pinned_memory_bytes;                        // 被 PacketIn 引用固定的接收内存
    size_t                      recv_fragment_count;                             // 接收重组分片数
    size_t                      recv_accounted_fragment_count;                   // 已进入连接级预算的分片数
    size_t                      send_ack_range_count;                            // 已确认发送区间数
    utp_stream_recv_fragment_t  recv_fragments[UTP_STREAM_RECV_FRAGMENT_LIMIT];  // 按偏移排序的接收分片
    utp_stream_send_ack_range_t send_ack_ranges[UTP_STREAM_SEND_ACK_RANGE_LIMIT];  // 已确认发送区间
    uint8_t                     send_buffer[UTP_STREAM_SEND_BUFFER_CAPACITY];      // 有界环形发送缓冲
    uint8_t                     priority;                                          // 用户设置的 0 至 7 优先级
    uint8_t                     strict_wait_rounds;                                // Strict 调度等待轮数，用于老化
    utp_stream_read_cb_t        read_cb;                                           // 可读通知回调
    utp_stream_write_cb_t       write_cb;                                          // 可写通知回调
    utp_stream_close_cb_t       close_cb;                                          // 双向关闭通知回调
    void*                       read_cb_data;                                      // 可读回调用户数据
    void*                       write_cb_data;                                     // 可写回调用户数据
    void*                       close_cb_data;                                     // 关闭回调用户数据
    bool                        used : 1;                                          // 是否已初始化为有效流
    bool                        local_fin_queued : 1;                              // 本端 FIN 已请求发送
    bool                        local_fin_sent : 1;                                // 本端 FIN 已构造发送
    bool                        local_fin_transmitted : 1;                         // 本端 FIN 已实际写入 UDP
    bool                        peer_fin : 1;                                      // 已接收到对端 FIN
    bool                        local_read_shutdown : 1;                           // 本地已停止读取
    bool                        local_write_reset : 1;                             // 本地写方向已被 RESET 终止
    bool                        peer_reset : 1;                                    // 对端已 RESET 其写方向
    bool                        peer_stop_sending_received : 1;                    // 是否收到过对端 STOP_SENDING
    bool                        peer_final_size_known : 1;                         // 对端最终偏移是否已经确定
    bool                        stream_limit_released : 1;                         // 对端流额度是否已归还
    bool                        notifying_readable : 1;                            // 正在执行可读回调，防止重入
    bool                        notifying_writable : 1;                            // 正在执行可写回调，防止重入
    bool                        closed_notified : 1;                               // 关闭回调是否已通知
    bool                        defer_user_notifications : 1;                      // incoming 回调前暂缓状态通知
};

/** @brief 初始化由 Connection 管理的流状态。 */
void                 utp_stream_init(utp_stream_t* stream, uint32_t stream_id);
/** @brief 释放流持有的 PacketIn 引用及重组状态。 */
void                 utp_stream_cleanup(utp_stream_t* stream);
/** @brief 判断本端是否拥有该流的发送方向。 */
bool                 utp_stream_local_can_send(const utp_stream_t* stream);
/** @brief 判断本端是否拥有该流的接收方向。 */
bool                 utp_stream_local_can_receive(const utp_stream_t* stream);
/** @brief 应用对端 RESET_STREAM，仅终止本地读方向并释放接收缓存。 */
utp_internal_error_t utp_stream_on_peer_reset(utp_stream_t* stream, uint64_t final_size);
/** @brief 关闭本地读方向并释放接收缓存。 */
utp_internal_error_t utp_stream_shutdown_read_internal(utp_stream_t* stream);
/** @brief 将接收偏移从连接级流控窗口退休。 */
utp_internal_error_t utp_stream_retire_receive_offset(utp_stream_t* stream, uint64_t offset);
/** @brief 在终止状态提交和 incoming 回调完成后触发关闭通知。 */
void                 utp_stream_notify_state_internal(utp_stream_t* stream);
/** @brief 返回发送缓冲区逻辑末尾偏移。 */
utp_internal_error_t utp_stream_send_buffered_end_offset(const utp_stream_t* stream, uint64_t* out_offset);
/** @brief 将数据复制写入流发送环形缓冲。 */
utp_internal_error_t utp_stream_write_internal(utp_stream_t* stream, const uint8_t* data, size_t length);
/** @brief 正常关闭本地写方向：先发送剩余数据，再携带 FIN；读方向保持可用。 */
utp_internal_error_t utp_stream_close_internal(utp_stream_t* stream);
/** @brief 异常中止本地写方向，丢弃待发数据和 FIN 状态。 */
utp_internal_error_t utp_stream_abort_write_internal(utp_stream_t* stream);
/** @brief 请求发送 RESET_STREAM，立即终止流，不属于正常 FIN 关闭。 */
utp_internal_error_t utp_stream_reset_internal(utp_stream_t* stream, uint16_t error_code);
/** @brief 获取内部发送环形缓冲的可写视图。 */
utp_internal_error_t utp_stream_acquire_write_views_internal(utp_stream_t* stream, utp_stream_write_view_t* views,
                                                             size_t view_capacity, size_t* out_view_count,
                                                             size_t* out_capacity);
/** @brief 提交通过可写视图写入的字节数。 */
utp_internal_error_t utp_stream_commit_write_views_internal(utp_stream_t* stream, size_t length);
/** @brief 判断流是否存在待发送数据或待发送 FIN。 */
bool                 utp_stream_has_send_work(const utp_stream_t* stream);
/** @brief 构造包含数据副本的完整 STREAM 帧。 */
utp_internal_error_t utp_stream_build_frame(utp_stream_t* stream, uint8_t* payload, size_t capacity,
                                            size_t* out_payload_length, uint32_t* out_stream_data_size,
                                            uint64_t* out_stream_offset, bool* out_fin);
/** @brief 构造零拷贝 STREAM 头和借用的数据视图。 */
utp_internal_error_t utp_stream_build_frame_view(utp_stream_t* stream, uint8_t* header, size_t capacity,
                                                 size_t* out_header_length, const uint8_t** out_data,
                                                 uint32_t* out_stream_data_size, uint64_t* out_stream_offset,
                                                 bool* out_fin);
/** @brief 在给定数据上限内构造零拷贝 STREAM 视图。 */
utp_internal_error_t utp_stream_build_frame_view_limited(utp_stream_t* stream, uint8_t* header, size_t capacity,
                                                         size_t max_data_length, size_t* out_header_length,
                                                         const uint8_t** out_data, uint32_t* out_stream_data_size,
                                                         uint64_t* out_stream_offset, bool* out_fin);
/** @brief 提交已排队 STREAM 帧，推进发送偏移和 FIN 状态。 */
utp_internal_error_t utp_stream_commit_built_frame(utp_stream_t* stream, uint32_t stream_data_size, bool fin);
/** @brief 回滚尚未写入 UDP 的已构造 STREAM 帧。 */
utp_internal_error_t utp_stream_abandon_built_frame(utp_stream_t* stream, uint64_t stream_offset,
                                                    uint32_t stream_data_size, bool fin);
/** @brief 处理连续发送数据被确认。 */
utp_internal_error_t utp_stream_on_packet_acked(utp_stream_t* stream, uint32_t stream_data_size);
/** @brief 处理任意偏移区间被确认。 */
utp_internal_error_t utp_stream_on_packet_acked_range(utp_stream_t* stream, uint64_t stream_offset,
                                                      uint32_t stream_data_size);
/** @brief 单调更新对端公布的流级发送额度。 */
void                 utp_stream_update_peer_max_stream_data(utp_stream_t* stream, uint64_t maximum_stream_data);
/** @brief 接收普通缓冲区承载的 STREAM 帧。 */
utp_internal_error_t utp_stream_on_frame(utp_stream_t* stream, const utp_frame_stream_t* frame);
/** @brief 接收 PacketIn 承载的 STREAM 帧并借用其数据。 */
utp_internal_error_t utp_stream_on_frame_packet(utp_stream_t* stream, const utp_frame_stream_t* frame,
                                                utp_packet_in_t* packet);
/** @brief 接收已关联连接级内存账本的零拷贝 STREAM 帧。 */
utp_internal_error_t utp_stream_on_frame_packet_accounted(utp_stream_t* stream, const utp_frame_stream_t* frame,
                                                          utp_packet_in_t*                 packet,
                                                          const utp_stream_recv_account_t* account);
/** @brief 获取可读的零拷贝重组片段视图。 */
utp_internal_error_t utp_stream_acquire_read_view_internal(utp_stream_t* stream, utp_stream_read_view_t* out_view);
/** @brief 提交已消费的零拷贝片段范围。 */
utp_internal_error_t utp_stream_commit_read_view_internal(utp_stream_t* stream, uint64_t offset, size_t length);
/** @brief 将连续重组数据复制给调用方。 */
utp_internal_error_t utp_stream_read_internal(utp_stream_t* stream, uint8_t* buffer, size_t capacity,
                                              size_t* out_length, bool* out_fin);
/** @brief 返回连续可读取的字节数。 */
size_t               utp_stream_readable_bytes(const utp_stream_t* stream);
/** @brief 返回仍由发送账本引用的流数据字节数。 */
size_t               utp_stream_send_in_flight_bytes(const utp_stream_t* stream);
/** @brief 判断流是否完全关闭或被重置。 */
bool                 utp_stream_is_closed(const utp_stream_t* stream);
/** @brief 设置可读回调；已有连续数据或 FIN 时立即同步通知。 */
void                 utp_stream_set_read_callback(utp_stream_t* stream, utp_stream_read_cb_t callback, void* user_data);
/** @brief 设置可写回调；当前可写时立即同步通知。 */
void utp_stream_set_write_callback(utp_stream_t* stream, utp_stream_write_cb_t callback, void* user_data);
/** @brief 设置双向关闭回调。 */
void utp_stream_set_close_callback(utp_stream_t* stream, utp_stream_close_cb_t callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONNECTION_STREAM_H
