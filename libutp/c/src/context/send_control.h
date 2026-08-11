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

// Connection 持有的发送状态；Packet I/O 和拥塞策略仍由连接层控制。
typedef struct utp_send_control {
    utp_send_ledger_t           ledger;
    utp_send_history_t          send_history;
    utp_rtt_stats_t             rtt_stats;
    utp_congestion_t*           congestion;
    utp_pacer_t                 pacer;
    struct utp_packet_out_tailq scheduled_packets;
    struct utp_packet_out_tailq lost_packets;
    struct utp_packet_out_tailq discarded_packets;
    uint64_t                    largest_acked_packet_number;
    uint64_t                    largest_acked_sent_time_us;
    uint64_t                    last_sent_time_us;
    uint64_t                    largest_sent_at_cutback;
    uint64_t                    current_packet_number;
    uint64_t                    peer_max_ack_delay_us;
    uint64_t                    scheduled_byte_count;
    size_t                      scheduled_packet_count;
    size_t                      scheduled_packet_limit;
    size_t                      lost_packet_count;
    size_t                      discarded_packet_count;
    uint32_t                    reorder_threshold;
    uint32_t                    consecutive_rto_count;
    uint32_t                    handshake_retransmission_count;
    uint32_t                    tlp_count;
    bool                        connected;
    bool                        loss_pending;
    bool                        pacing_enabled;
    bool                        was_quiet;
    bool                        app_limited;
} utp_send_control_t;

typedef struct utp_send_control_ack_result {
    utp_send_ledger_ack_result_t ledger;
    uint64_t                     rtt_sample_us;
    bool                         rtt_sample_valid;
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
void                 utp_send_control_set_pacing_enabled(utp_send_control_t* control, bool enabled);
void                 utp_send_control_set_app_limited(utp_send_control_t* control, bool app_limited);
void                 utp_send_control_pacer_tick_in(utp_send_control_t* control, uint64_t now_us);
void                 utp_send_control_pacer_tick_out(utp_send_control_t* control);
bool                 utp_send_control_can_schedule_packet(const utp_send_control_t* control, uint64_t packet_size);
bool                 utp_send_control_can_transmit_packet(utp_send_control_t* control, uint64_t packet_size);
uint64_t             utp_send_control_pacing_deadline(const utp_send_control_t* control);
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

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONTEXT_SEND_CONTROL_H
