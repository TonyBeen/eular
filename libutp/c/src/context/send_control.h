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

// Connection-owned send state. Packet I/O and congestion policy remain in the connection layer.
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
// Schedules packet for one eventual send. Packets remain caller-owned until sent or explicitly released.
utp_internal_error_t utp_send_control_schedule_packet(utp_send_control_t* control, utp_packet_out_t* packet,
                                                      bool track_on_send);
// Schedules a newly built packet at the front of the send queue.
utp_internal_error_t utp_send_control_schedule_packet_front(utp_send_control_t* control, utp_packet_out_t* packet,
                                                            bool track_on_send);
// Restores an unsent packet to the head of the scheduled queue without changing its tracking policy.
utp_internal_error_t utp_send_control_reschedule_packet(utp_send_control_t* control, utp_packet_out_t* packet);
// Removes and returns the oldest scheduled packet, or NULL when no packet is ready.
utp_packet_out_t* utp_send_control_next_scheduled(utp_send_control_t* control);
// Returns the oldest scheduled packet without changing queue ownership.
utp_packet_out_t* utp_send_control_peek_scheduled(const utp_send_control_t* control);
// Scans tracked packets using the current ACK state and prepares each detected loss for retransmission or release.
utp_internal_error_t utp_send_control_detect_losses(utp_send_control_t* control);
// Returns the oldest detected retransmission candidate. The packet remains marked lost until a successful resend.
utp_packet_out_t* utp_send_control_next_lost(utp_send_control_t* control);
// Restores a retransmission candidate to the head of the loss queue when admission is unavailable.
utp_internal_error_t utp_send_control_reschedule_lost(utp_send_control_t* control, utp_packet_out_t* packet);
// Returns a packet that the connection must release to its packet pool. No release occurs in send-control.
utp_packet_out_t* utp_send_control_next_discarded(utp_send_control_t* control);
// Removes an in-flight MTU probe without placing it on any retransmission queue.
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
// Applies the currently selected retransmission mode without allocating or performing I/O.
utp_internal_error_t utp_send_control_on_retransmission_timeout(utp_send_control_t* control);
utp_internal_error_t utp_send_control_on_ack(utp_send_control_t* control, const utp_ack_info_t* ack, uint64_t now_us,
                                             struct utp_packet_out_tailq*   acknowledged_packets,
                                             utp_send_control_ack_result_t* result);
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
