#ifndef EULAR_UTP_CONTEXT_SEND_CONTROL_H
#define EULAR_UTP_CONTEXT_SEND_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include "congestion/congestion.h"
#include "congestion/pacer.h"
#include "context/send_ledger.h"
#include "util/rtt.h"
#include "util/send_history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_SEND_ATTEMPT_MAX_LEVEL 8u

typedef struct utp_send_attempt_node {
    struct utp_send_attempt_node* packet_next;                       // 同一逻辑包的下一发送尝试
    struct utp_send_attempt_node* packet_prev;                       // 同一逻辑包的前一发送尝试
    struct utp_send_attempt_node* free_next;                         // 空闲节点链表下一项
    utp_packet_out_t*             packet;                            // 所属逻辑包，不拥有
    uint64_t                      packet_number;                     // 此尝试包号
    uint64_t                      sent_time_us;                      // 此尝试发送时刻
    struct utp_send_attempt_node* next[UTP_SEND_ATTEMPT_MAX_LEVEL];  // 包号跳表前向指针
} utp_send_attempt_node_t;

typedef struct utp_send_attempt_block {
    struct utp_send_attempt_block* next;      // 下一扩容块
    size_t                         capacity;  // 此块节点数量
    utp_send_attempt_node_t*       nodes;     // 节点数组所有权
} utp_send_attempt_block_t;

// Connection 持有的发送状态；Packet I/O 和拥塞策略仍由连接层控制。
typedef struct utp_send_control {
    utp_send_ledger_t           ledger;                                    // 未确认包账本
    utp_send_history_t          send_history;                              // 已发送包号历史
    utp_rtt_stats_t             rtt_stats;                                 // RTT 统计
    utp_congestion_t*           congestion;                                // 当前拥塞算法，不拥有
    utp_pacer_t                 pacer;                                     // pacing 调度状态
    struct utp_packet_out_tailq scheduled_packets;                         // 等待实际发送的包队列
    struct utp_packet_out_tailq lost_packets;                              // 待重新构造并重传的包队列
    struct utp_packet_out_tailq discarded_packets;                         // 等待连接层释放的失效包队列
    utp_send_attempt_block_t*   attempt_blocks;                            // 发送尝试索引分块所有权
    utp_send_attempt_node_t*    attempt_head[UTP_SEND_ATTEMPT_MAX_LEVEL];  // 包号跳表头
    utp_send_attempt_node_t*    attempt_free;                              // 可复用索引节点
    uint64_t                    largest_acked_packet_number;               // 最大已确认包号
    uint64_t                    largest_acked_sent_time_us;                // 对应发送时刻
    uint64_t                    last_sent_time_us;                         // 最近实际发送时刻
    uint64_t                    largest_sent_at_cutback;                   // 拥塞回退时最大已发送包号
    uint64_t                    current_packet_number;                     // 下一个待分配包号
    uint64_t                    peer_max_ack_delay_us;                     // 对端通告最大 ACK 延迟
    uint64_t                    scheduled_byte_count;                      // 待发送队列总字节数
    size_t                      scheduled_packet_count;                    // 待发送队列包数
    size_t                      scheduled_packet_limit;                    // 待发送队列容量上限
    size_t                      lost_packet_count;                         // 待重传丢失包数
    size_t                      discarded_packet_count;                    // 待连接层回收包数
    size_t                      attempt_count;                             // 活跃发送尝试索引数
    size_t                      attempt_capacity;                          // 已分配发送尝试索引容量
    size_t                      attempt_block_size;                        // 下一扩容块大小
    uint8_t                     attempt_level;                             // 当前跳表层高
    uint32_t                    reorder_threshold;                         // 包阈值丢失检测阈值
    uint32_t                    consecutive_rto_count;                     // 连续 RTO 次数
    uint32_t                    handshake_retransmission_count;            // 握手重传次数
    uint32_t                    tlp_count;                                 // Tail Loss Probe 次数
    bool                        connected;                                 // 是否进入普通可靠发送阶段
    bool                        loss_pending;                              // 是否待执行丢失检测
    bool                        pacing_enabled;                            // 是否启用 pacer
    bool                        was_quiet;                                 // 上次发送前是否空闲
    bool                        app_limited;                               // 当前是否应用数据受限
} utp_send_control_t;

typedef struct utp_send_control_ack_result {
    utp_send_ledger_ack_result_t ledger;                            // 账本确认结果
    uint64_t                     rtt_acknowledged_packet_number;    // RTT 采样对应包号
    uint64_t                     rtt_acknowledged_sent_time_us;     // RTT 采样对应发送时刻
    uint64_t                     rtt_sample_us;                     // 计算出的 RTT 样本
    bool                         rtt_acknowledged_current_attempt;  // 是否确认最新重传尝试
    bool                         rtt_sample_valid;                  // RTT 样本是否有效
} utp_send_control_ack_result_t;

typedef enum utp_send_control_retransmission_mode {
    UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE = 0,
    UTP_SEND_CONTROL_RETRANSMISSION_LOSS      = 1,
    UTP_SEND_CONTROL_RETRANSMISSION_TLP       = 2,
    UTP_SEND_CONTROL_RETRANSMISSION_RTO       = 3
} utp_send_control_retransmission_mode_t;

utp_internal_error_t utp_send_control_init(utp_send_control_t* control, size_t packet_limit,
                                           uint32_t retransmittable_frame_mask, uint64_t gap_warning_threshold,
                                           uint64_t peer_max_ack_delay_us);
void                 utp_send_control_cleanup(utp_send_control_t* control);
utp_internal_error_t utp_send_control_on_packet_sent(utp_send_control_t* control, utp_packet_out_t* packet);
/** @brief 为下一次实际发送预留历史索引，避免发送后丢失迟到 ACK 的映射。 */
bool                 utp_send_control_can_record_attempt(utp_send_control_t* control, const utp_packet_out_t* packet);
/** @brief 删除 PacketOut 的所有历史发送尝试索引，释放前必须调用。 */
void                 utp_send_control_forget_packet_attempts(utp_send_control_t* control, utp_packet_out_t* packet);
utp_internal_error_t utp_send_control_allocate_packet_number(utp_send_control_t* control, uint64_t* packet_number);
// pending 阶段已经使用的包号属于同一发送方向；晋升后从 next_packet_number 继续分配。
utp_internal_error_t utp_send_control_adopt_next_packet_number(utp_send_control_t* control,
                                                               uint64_t            next_packet_number);
// 将包排队等待一次发送；成功发送或显式释放前，PacketOut 所有权仍属于调用方。
utp_internal_error_t utp_send_control_schedule_packet(utp_send_control_t* control, utp_packet_out_t* packet,
                                                      bool track_on_send);
// 将新构造的包插入发送队列头部。
utp_internal_error_t utp_send_control_schedule_packet_front(utp_send_control_t* control, utp_packet_out_t* packet,
                                                            bool track_on_send);
// 将未发送包恢复到队首，不改变其确认跟踪策略。
utp_internal_error_t utp_send_control_reschedule_packet(utp_send_control_t* control, utp_packet_out_t* packet);
// 移除并返回最早排队包；当前没有就绪包时返回 NULL。
utp_packet_out_t*    utp_send_control_next_scheduled(utp_send_control_t* control);
// 查看最早排队包，但不改变队列所有权。
utp_packet_out_t*    utp_send_control_peek_scheduled(const utp_send_control_t* control);
// 根据当前 ACK 状态扫描跟踪包，将判定丢失的包转入重传或释放队列。
utp_internal_error_t utp_send_control_detect_losses(utp_send_control_t* control);
// 返回最早的重传候选；重新发送成功前仍保持 lost 状态。
utp_packet_out_t*    utp_send_control_next_lost(utp_send_control_t* control);
// 拥塞控制暂不允许发送时，将重传候选恢复到丢失队列头部。
utp_internal_error_t utp_send_control_reschedule_lost(utp_send_control_t* control, utp_packet_out_t* packet);
// 返回应由 Connection 归还对象池的包；send-control 自身不执行释放。
utp_packet_out_t*    utp_send_control_next_discarded(utp_send_control_t* control);
// 移除飞行中的 MTU 探测包，不放入普通重传队列。
utp_internal_error_t utp_send_control_take_mtu_probe(utp_send_control_t* control, uint64_t packet_number,
                                                     utp_packet_out_t** out_packet);
void                 utp_send_control_set_connected(utp_send_control_t* control, bool connected);
void                 utp_send_control_set_loss_pending(utp_send_control_t* control, bool pending);
void                 utp_send_control_set_congestion(utp_send_control_t* control, utp_congestion_t* congestion);
void     utp_send_control_set_pacing_enabled(utp_send_control_t* control, bool enabled, uint32_t clock_granularity_us);
void     utp_send_control_set_app_limited(utp_send_control_t* control, bool app_limited);
void     utp_send_control_pacer_tick_in(utp_send_control_t* control, uint64_t now_us);
void     utp_send_control_pacer_tick_out(utp_send_control_t* control);
bool     utp_send_control_can_schedule_packet(const utp_send_control_t* control, uint64_t packet_size);
bool     utp_send_control_can_transmit_packet(utp_send_control_t* control, uint64_t packet_size);
uint64_t utp_send_control_pacing_deadline(const utp_send_control_t* control);
utp_send_control_retransmission_mode_t utp_send_control_retransmission_mode(const utp_send_control_t* control);
uint64_t                               utp_send_control_calculate_handshake_delay(utp_send_control_t* control);
uint64_t                               utp_send_control_calculate_tlp_delay(const utp_send_control_t* control);
uint64_t                               utp_send_control_calculate_rto(const utp_send_control_t* control);
// 执行当前选定的重传模式，不分配内存，也不执行 I/O。
utp_internal_error_t                   utp_send_control_on_retransmission_timeout(utp_send_control_t* control);
utp_internal_error_t utp_send_control_on_ack(utp_send_control_t* control, const utp_ack_info_t* ack, uint64_t now_us,
                                             struct utp_packet_out_tailq*   acknowledged_packets,
                                             utp_send_control_ack_result_t* result);
/** @brief 处理握手 ACK，并使用不受普通 max_ack_delay 限制的精确握手处理耗时。 */
utp_internal_error_t utp_send_control_on_handshake_ack(utp_send_control_t* control, const utp_ack_info_t* ack,
                                                       uint64_t now_us, uint64_t handshake_delay_us,
                                                       struct utp_packet_out_tailq*   acknowledged_packets,
                                                       utp_send_control_ack_result_t* result);
// 收到有效 Handshake 后退休仍残留的 Initial；其中的显式 ACK 已在此调用前单独形成 RTT 样本。
utp_internal_error_t utp_send_control_retire_handshake_packets(utp_send_control_t* control, uint64_t now_us,
                                                               struct utp_packet_out_tailq* retired_packets);
uint64_t             utp_send_control_largest_sent(const utp_send_control_t* control);
uint64_t             utp_send_control_largest_acked(const utp_send_control_t* control);
size_t               utp_send_control_unacked_packet_count(const utp_send_control_t* control);
size_t               utp_send_control_scheduled_packet_count(const utp_send_control_t* control);
uint64_t             utp_send_control_scheduled_bytes(const utp_send_control_t* control);
size_t               utp_send_control_lost_packet_count(const utp_send_control_t* control);
size_t               utp_send_control_discarded_packet_count(const utp_send_control_t* control);
uint64_t             utp_send_control_srtt(const utp_send_control_t* control);
/** @brief 返回当前拥塞控制器给出的 pacing 带宽估计，单位 bytes/s。 */
uint64_t             utp_send_control_bandwidth_estimate(const utp_send_control_t* control);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONTEXT_SEND_CONTROL_H
