#include "context/send_control.h"

#include <limits.h>
#include <string.h>

#include "proto/proto.h"

#define UTP_SEND_CONTROL_DEFAULT_REORDER_THRESHOLD   3u
#define UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US UINT64_C(60000000)
#define UTP_SEND_CONTROL_DEFAULT_RTO_US              UINT64_C(500000)
#define UTP_SEND_CONTROL_MIN_RTO_US                  UINT64_C(200000)
#define UTP_SEND_CONTROL_INITIAL_RTT_US              UINT64_C(333333)
#define UTP_SEND_CONTROL_MAX_RTO_BACKOFFS            10u
#define UTP_SEND_CONTROL_MAX_TLP_COUNT               2u
#define UTP_SEND_CONTROL_PACER_GRANULARITY_US        1000u

static uint64_t utp_send_control_packet_size(const utp_packet_out_t *packet) {
    return (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size : packet->data_size;
}

static void utp_send_control_packet_info(const utp_packet_out_t *packet, utp_congestion_packet_info_t *info) {
    info->packet_number = packet->packet_number;
    info->sent_time_us  = packet->sent_time_us;
    info->packet_size   = (uint32_t)utp_send_control_packet_size(packet);
    info->state         = packet->bw_state == NULL ? (void *)&packet->bw_packet_state : packet->bw_state;
}

static uint64_t utp_send_control_pacing_interval(const utp_send_control_t *control, uint64_t packet_size) {
    uint64_t rate;
    uint64_t numerator;

    rate = utp_congestion_get_pacing_rate(control->congestion, 0);
    if (rate == 0u || packet_size == 0u || packet_size > UINT64_MAX / UINT64_C(1000000)) {
        return 1u;
    }
    numerator = packet_size * UINT64_C(1000000);
    return numerator / rate + (numerator % rate == 0u ? 0u : 1u);
}

utp_internal_error_t utp_send_control_init(utp_send_control_t *control, size_t packet_limit,
                                           uint32_t retransmittable_frame_mask, uint64_t gap_warning_threshold,
                                           uint64_t peer_max_ack_delay_us) {
    utp_internal_error_t error;

    if (control == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(control, 0, sizeof(*control));
    error = utp_send_ledger_init(&control->ledger, packet_limit, retransmittable_frame_mask);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_send_history_init(&control->send_history, gap_warning_threshold);
    TAILQ_INIT(&control->scheduled_packets);
    TAILQ_INIT(&control->lost_packets);
    TAILQ_INIT(&control->discarded_packets);
    control->peer_max_ack_delay_us  = peer_max_ack_delay_us;
    control->scheduled_packet_limit = packet_limit;
    control->reorder_threshold      = UTP_SEND_CONTROL_DEFAULT_REORDER_THRESHOLD;
    utp_pacer_init(&control->pacer, UTP_SEND_CONTROL_PACER_GRANULARITY_US);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_send_control_cleanup(utp_send_control_t *control) {
    utp_packet_out_t *packet;

    if (control == NULL) {
        return;
    }
    while ((packet = TAILQ_FIRST(&control->scheduled_packets)) != NULL) {
        TAILQ_REMOVE(&control->scheduled_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_SCHED;
    }
    while ((packet = TAILQ_FIRST(&control->lost_packets)) != NULL) {
        TAILQ_REMOVE(&control->lost_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_LOST;
    }
    while ((packet = TAILQ_FIRST(&control->discarded_packets)) != NULL) {
        TAILQ_REMOVE(&control->discarded_packets, packet, po_next);
    }
    utp_send_ledger_cleanup(&control->ledger);
    memset(control, 0, sizeof(*control));
}

utp_internal_error_t utp_send_control_on_packet_sent(utp_send_control_t *control, utp_packet_out_t *packet) {
    utp_internal_error_t         error;
    utp_congestion_packet_info_t info;
    uint64_t                     packet_size;
    uint64_t                     inflight_before;
    uint64_t                     packet_count_before;

    if (control == NULL || packet == NULL || packet->sent_time_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & (UTP_PO_SCHED | UTP_PO_UNACKED | UTP_PO_LOST)) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if ((packet->local_flags & UTP_POL_NO_TRACK_ON_SEND) != 0u) {
        control->last_sent_time_us = packet->sent_time_us;
        if (packet->packet_number > control->current_packet_number) {
            control->current_packet_number = packet->packet_number;
        }
        (void)utp_packet_out_add_send_attempt(packet, packet->packet_number, packet->sent_time_us);
        return UTP_INTERNAL_ERROR_OK;
    }
    error = utp_send_ledger_track(&control->ledger, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_send_history_update(&control->send_history, packet->packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        (void)utp_send_ledger_remove(&control->ledger, packet);
        return error;
    }
    packet_size         = utp_send_control_packet_size(packet);
    inflight_before     = utp_send_ledger_bytes_in_flight(&control->ledger) - packet_size;
    packet_count_before = utp_send_ledger_packet_count(&control->ledger) - 1u;
    if (control->congestion != NULL) {
        utp_send_control_packet_info(packet, &info);
        utp_congestion_on_packet_sent(control->congestion, &info, inflight_before, control->app_limited ? 1 : 0);
        packet->bw_state = info.state;
    }
    if (control->pacing_enabled) {
        utp_pacer_packet_scheduled(&control->pacer, packet_count_before, false,
                                   utp_send_control_pacing_interval(control, packet_size));
    }
    (void)utp_packet_out_add_send_attempt(packet, packet->packet_number, packet->sent_time_us);
    control->last_sent_time_us = packet->sent_time_us;
    if (packet->packet_number > control->current_packet_number) {
        control->current_packet_number = packet->packet_number;
    }
    packet->po_flags    &= (uint16_t)~(UTP_PO_LOST | UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO);
    packet->local_flags &= (uint16_t)~UTP_POL_FACKED;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_allocate_packet_number(utp_send_control_t *control, uint64_t *packet_number) {
    if (control == NULL || packet_number == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (control->current_packet_number >= UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    ++control->current_packet_number;
    *packet_number = control->current_packet_number;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_schedule_packet(utp_send_control_t *control, utp_packet_out_t *packet,
                                                      bool track_on_send) {
    uint64_t packet_size;

    if (control == NULL || packet == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & (UTP_PO_SCHED | UTP_PO_UNACKED | UTP_PO_LOST)) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (control->scheduled_packet_count >= control->scheduled_packet_limit) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    packet_size = utp_send_control_packet_size(packet);
    if (packet_size > UINT64_MAX - control->scheduled_byte_count) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }

    TAILQ_INSERT_TAIL(&control->scheduled_packets, packet, po_next);
    packet->po_flags |= UTP_PO_SCHED;
    if (track_on_send) {
        packet->local_flags |= UTP_POL_TRACK_ON_SEND;
        packet->local_flags &= (uint16_t)~UTP_POL_NO_TRACK_ON_SEND;
    } else {
        packet->local_flags &= (uint16_t)~UTP_POL_TRACK_ON_SEND;
        packet->local_flags |= UTP_POL_NO_TRACK_ON_SEND;
    }
    control->scheduled_byte_count += packet_size;
    ++control->scheduled_packet_count;
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t *utp_send_control_next_scheduled(utp_send_control_t *control) {
    utp_packet_out_t *packet;
    uint64_t          packet_size;

    if (control == NULL || (packet = TAILQ_FIRST(&control->scheduled_packets)) == NULL) {
        return NULL;
    }
    packet_size = utp_send_control_packet_size(packet);
    if (control->scheduled_packet_count == 0u || packet_size > control->scheduled_byte_count) {
        return NULL;
    }

    TAILQ_REMOVE(&control->scheduled_packets, packet, po_next);
    packet->po_flags &= (uint16_t)~UTP_PO_SCHED;
    --control->scheduled_packet_count;
    control->scheduled_byte_count -= packet_size;
    return packet;
}

static bool utp_send_control_packet_is_retransmittable(const utp_send_control_t *control,
                                                       const utp_packet_out_t   *packet) {
    return (packet->frame_types & control->ledger.retransmittable_frame_mask) != 0u;
}

static bool utp_send_control_is_fack_lost(const utp_send_control_t *control, const utp_packet_out_t *packet) {
    return control->largest_acked_packet_number > control->reorder_threshold &&
           packet->packet_number < control->largest_acked_packet_number - control->reorder_threshold;
}

static bool utp_send_control_is_time_lost(const utp_send_control_t *control, const utp_packet_out_t *packet) {
    uint64_t srtt;

    srtt = utp_rtt_stats_srtt(&control->rtt_stats);
    return srtt != 0u && control->largest_acked_sent_time_us > srtt &&
           packet->sent_time_us < control->largest_acked_sent_time_us - srtt;
}

static utp_internal_error_t utp_send_control_mark_packet_lost(utp_send_control_t *control, utp_packet_out_t *packet,
                                                              bool fack_lost) {
    utp_internal_error_t         error;
    utp_congestion_packet_info_t info;

    if (control->congestion != NULL && (packet->po_flags & UTP_PO_MTU_PROBE) == 0u) {
        utp_send_control_packet_info(packet, &info);
        utp_congestion_on_lost(control->congestion, &info);
        packet->bw_state = info.state;
    }

    error = utp_send_ledger_remove(&control->ledger, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if ((packet->po_flags & UTP_PO_MTU_PROBE) != 0u || !utp_send_control_packet_is_retransmittable(control, packet)) {
        TAILQ_INSERT_TAIL(&control->discarded_packets, packet, po_next);
        ++control->discarded_packet_count;
        return UTP_INTERNAL_ERROR_OK;
    }
    packet->po_flags |= UTP_PO_LOST | UTP_PO_LOSS_RECORDED | UTP_PO_RESET_PACKNO;
    if (fack_lost) {
        packet->local_flags |= UTP_POL_FACKED;
    }
    packet->loss_chain = packet;
    TAILQ_INSERT_TAIL(&control->lost_packets, packet, po_next);
    ++control->lost_packet_count;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_detect_losses(utp_send_control_t *control) {
    utp_packet_out_t    *packet;
    utp_packet_out_t    *next;
    utp_internal_error_t error;
    uint64_t             largest_lost_packet_number = 0u;

    if (control == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (packet = TAILQ_FIRST(&control->ledger.unacked_packets); packet != NULL; packet = next) {
        bool fack_lost;
        bool time_lost;

        next = TAILQ_NEXT(packet, po_next);
        if (packet->packet_number > control->largest_acked_packet_number ||
            (packet->po_flags & UTP_PO_LOSS_RECORDED) != 0u) {
            continue;
        }
        fack_lost = utp_send_control_is_fack_lost(control, packet);
        time_lost = utp_send_control_is_time_lost(control, packet);
        if (!fack_lost && !time_lost) {
            continue;
        }

        if (utp_send_control_packet_is_retransmittable(control, packet) &&
            (packet->po_flags & UTP_PO_MTU_PROBE) == 0u && packet->packet_number > largest_lost_packet_number) {
            largest_lost_packet_number = packet->packet_number;
        }

        error = utp_send_control_mark_packet_lost(control, packet, fack_lost);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if (largest_lost_packet_number > control->largest_sent_at_cutback) {
        utp_congestion_on_loss(control->congestion);
        if (control->pacing_enabled) {
            utp_pacer_on_loss(&control->pacer);
        }
        control->largest_sent_at_cutback = utp_send_history_largest(&control->send_history);
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t *utp_send_control_next_lost(utp_send_control_t *control) {
    utp_packet_out_t *packet;

    if (control == NULL || (packet = TAILQ_FIRST(&control->lost_packets)) == NULL) {
        return NULL;
    }
    TAILQ_REMOVE(&control->lost_packets, packet, po_next);
    packet->po_flags &= (uint16_t)~UTP_PO_LOST;
    --control->lost_packet_count;
    return packet;
}

utp_packet_out_t *utp_send_control_next_discarded(utp_send_control_t *control) {
    utp_packet_out_t *packet;

    if (control == NULL || (packet = TAILQ_FIRST(&control->discarded_packets)) == NULL) {
        return NULL;
    }
    TAILQ_REMOVE(&control->discarded_packets, packet, po_next);
    --control->discarded_packet_count;
    return packet;
}

void utp_send_control_set_connected(utp_send_control_t *control, bool connected) {
    if (control != NULL) {
        control->connected = connected;
    }
}

void utp_send_control_set_loss_pending(utp_send_control_t *control, bool pending) {
    if (control != NULL) {
        control->loss_pending = pending;
    }
}

void utp_send_control_set_congestion(utp_send_control_t *control, utp_congestion_t *congestion) {
    if (control != NULL) {
        control->congestion = congestion;
        utp_congestion_init(congestion, &control->rtt_stats);
    }
}

void utp_send_control_set_pacing_enabled(utp_send_control_t *control, bool enabled) {
    if (control != NULL) {
        control->pacing_enabled = enabled;
        utp_pacer_init(&control->pacer, UTP_SEND_CONTROL_PACER_GRANULARITY_US);
    }
}

void utp_send_control_set_app_limited(utp_send_control_t *control, bool app_limited) {
    if (control != NULL) {
        control->app_limited = app_limited;
    }
}

void utp_send_control_pacer_tick_in(utp_send_control_t *control, uint64_t now_us) {
    if (control != NULL && control->pacing_enabled) {
        utp_pacer_tick_in(&control->pacer, now_us);
    }
}

void utp_send_control_pacer_tick_out(utp_send_control_t *control) {
    if (control != NULL && control->pacing_enabled) {
        utp_pacer_tick_out(&control->pacer);
    }
}

bool utp_send_control_can_schedule_packet(const utp_send_control_t *control, uint64_t packet_size) {
    uint64_t used;
    uint64_t cwnd;

    if (control == NULL || control->congestion == NULL || packet_size == 0u) {
        return false;
    }
    used = utp_send_ledger_bytes_in_flight(&control->ledger);
    if (used > UINT64_MAX - control->scheduled_byte_count) {
        return false;
    }
    used += control->scheduled_byte_count;
    if (used == 0u) {
        return true;
    }
    cwnd = utp_congestion_get_cwnd(control->congestion);
    return used < cwnd && packet_size <= cwnd - used;
}

bool utp_send_control_can_transmit_packet(utp_send_control_t *control, uint64_t packet_size) {
    uint64_t inflight;
    uint64_t cwnd;

    if (control == NULL || control->congestion == NULL || packet_size == 0u) {
        return false;
    }
    inflight = utp_send_ledger_bytes_in_flight(&control->ledger);
    cwnd     = utp_congestion_get_cwnd(control->congestion);
    if (inflight != 0u && (inflight >= cwnd || packet_size > cwnd - inflight)) {
        return false;
    }
    return !control->pacing_enabled ||
           utp_pacer_can_schedule(&control->pacer, utp_send_ledger_packet_count(&control->ledger));
}

static bool utp_send_control_has_unacked_handshake_packet(const utp_send_control_t *control) {
    utp_packet_out_t *packet;

    if (control->connected) {
        return false;
    }
    TAILQ_FOREACH(packet, &control->ledger.unacked_packets, po_next) {
        if ((packet->po_flags & UTP_PO_HELLO) != 0u) {
            return true;
        }
    }
    return false;
}

utp_send_control_retransmission_mode_t utp_send_control_retransmission_mode(const utp_send_control_t *control) {
    if (control == NULL || TAILQ_EMPTY(&control->ledger.unacked_packets)) {
        return UTP_SEND_CONTROL_RETRANSMISSION_RTO;
    }
    if (utp_send_control_has_unacked_handshake_packet(control)) {
        return UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE;
    }
    if (control->loss_pending) {
        return UTP_SEND_CONTROL_RETRANSMISSION_LOSS;
    }
    if (control->tlp_count < UTP_SEND_CONTROL_MAX_TLP_COUNT) {
        return UTP_SEND_CONTROL_RETRANSMISSION_TLP;
    }
    return UTP_SEND_CONTROL_RETRANSMISSION_RTO;
}

uint64_t utp_send_control_calculate_handshake_delay(utp_send_control_t *control) {
    uint64_t delay;
    uint32_t exponent;

    if (control == NULL) {
        return 0u;
    }
    delay = utp_rtt_stats_srtt(&control->rtt_stats);
    if (delay == 0u) {
        delay = 150000u;
    } else {
        delay += delay / 2u;
        if (delay < 10000u) {
            delay = 10000u;
        }
    }
    exponent = control->handshake_retransmission_count > 8u ? 8u : control->handshake_retransmission_count;
    if (control->handshake_retransmission_count != UINT32_MAX) {
        ++control->handshake_retransmission_count;
    }
    while (exponent-- != 0u) {
        if (delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US / 2u) {
            return UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US;
        }
        delay *= 2u;
    }
    return delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US ? UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US : delay;
}

uint64_t utp_send_control_calculate_tlp_delay(const utp_send_control_t *control) {
    uint64_t srtt;
    uint64_t delay;

    if (control == NULL) {
        return 0u;
    }
    srtt = utp_rtt_stats_srtt(&control->rtt_stats);
    if (srtt == 0u) {
        srtt = UTP_SEND_CONTROL_INITIAL_RTT_US;
    }
    if (utp_send_ledger_packet_count(&control->ledger) > 1u) {
        delay = 10000u;
    } else if (srtt > UINT64_MAX - srtt / 2u) {
        delay = UINT64_MAX;
    } else {
        delay = srtt + srtt / 2u;
        if (delay > UINT64_MAX - control->peer_max_ack_delay_us) {
            delay = UINT64_MAX;
        } else {
            delay += control->peer_max_ack_delay_us;
        }
    }
    if (srtt <= UINT64_MAX / 2u && delay < srtt * 2u) {
        delay = srtt * 2u;
    }
    return delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US ? UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US : delay;
}

uint64_t utp_send_control_calculate_rto(const utp_send_control_t *control) {
    uint64_t base_delay;
    uint64_t srtt;
    uint64_t variance;
    uint64_t factor;
    uint32_t exponent;

    if (control == NULL) {
        return 0u;
    }
    srtt     = utp_rtt_stats_srtt(&control->rtt_stats);
    variance = utp_rtt_stats_variance(&control->rtt_stats);
    if (srtt == 0u) {
        base_delay = UTP_SEND_CONTROL_DEFAULT_RTO_US;
    } else if (variance > (UINT64_MAX - srtt) / 4u) {
        base_delay = UINT64_MAX;
    } else {
        base_delay = srtt + 4u * variance;
        if (base_delay < UTP_SEND_CONTROL_MIN_RTO_US) {
            base_delay = UTP_SEND_CONTROL_MIN_RTO_US;
        }
    }
    exponent = control->consecutive_rto_count > UTP_SEND_CONTROL_MAX_RTO_BACKOFFS ? UTP_SEND_CONTROL_MAX_RTO_BACKOFFS
                                                                                  : control->consecutive_rto_count;
    factor   = UINT64_C(1) << exponent;
    if (base_delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US / factor) {
        return UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US;
    }
    base_delay *= factor;
    return base_delay > UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US ? UTP_SEND_CONTROL_MAX_RETRANSMISSION_DELAY_US
                                                                     : base_delay;
}

typedef enum utp_send_control_expire_filter {
    UTP_SEND_CONTROL_EXPIRE_ALL,
    UTP_SEND_CONTROL_EXPIRE_HANDSHAKE_ONLY,
    UTP_SEND_CONTROL_EXPIRE_LAST_RETRANSMITTABLE
} utp_send_control_expire_filter_t;

static utp_internal_error_t utp_send_control_expire_unacked(utp_send_control_t              *control,
                                                            utp_send_control_expire_filter_t filter) {
    utp_packet_out_t    *packet;
    utp_packet_out_t    *next;
    utp_internal_error_t error;

    if (filter == UTP_SEND_CONTROL_EXPIRE_LAST_RETRANSMITTABLE) {
        TAILQ_FOREACH_REVERSE(packet, &control->ledger.unacked_packets, utp_packet_out_tailq, po_next) {
            if (utp_send_control_packet_is_retransmittable(control, packet) &&
                (packet->po_flags & UTP_PO_LOSS_RECORDED) == 0u) {
                return utp_send_control_mark_packet_lost(control, packet, false);
            }
        }
        return UTP_INTERNAL_ERROR_OK;
    }

    for (packet = TAILQ_FIRST(&control->ledger.unacked_packets); packet != NULL; packet = next) {
        next = TAILQ_NEXT(packet, po_next);
        if ((packet->po_flags & UTP_PO_LOSS_RECORDED) != 0u ||
            (filter == UTP_SEND_CONTROL_EXPIRE_HANDSHAKE_ONLY && (packet->po_flags & UTP_PO_HELLO) == 0u) ||
            ((packet->po_flags & UTP_PO_MTU_PROBE) == 0u &&
             !utp_send_control_packet_is_retransmittable(control, packet))) {
            continue;
        }
        error = utp_send_control_mark_packet_lost(control, packet, false);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_control_on_retransmission_timeout(utp_send_control_t *control) {
    if (control == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (TAILQ_EMPTY(&control->ledger.unacked_packets)) {
        control->loss_pending = false;
        return UTP_INTERNAL_ERROR_OK;
    }

    switch (utp_send_control_retransmission_mode(control)) {
        case UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE:
            return utp_send_control_expire_unacked(control, UTP_SEND_CONTROL_EXPIRE_HANDSHAKE_ONLY);
        case UTP_SEND_CONTROL_RETRANSMISSION_LOSS:
            control->loss_pending = false;
            return utp_send_control_detect_losses(control);
        case UTP_SEND_CONTROL_RETRANSMISSION_TLP:
            if (control->tlp_count != UINT32_MAX) {
                ++control->tlp_count;
            }
            return utp_send_control_expire_unacked(control, UTP_SEND_CONTROL_EXPIRE_LAST_RETRANSMITTABLE);
        case UTP_SEND_CONTROL_RETRANSMISSION_RTO:
            if (control->consecutive_rto_count != UINT32_MAX) {
                ++control->consecutive_rto_count;
            }
            utp_congestion_on_timeout(control->congestion);
            return utp_send_control_expire_unacked(control, UTP_SEND_CONTROL_EXPIRE_ALL);
    }
    return UTP_INTERNAL_ERROR_STATE;
}

utp_internal_error_t utp_send_control_on_ack(utp_send_control_t *control, const utp_ack_info_t *ack, uint64_t now_us,
                                             struct utp_packet_out_tailq   *acknowledged_packets,
                                             utp_send_control_ack_result_t *result) {
    utp_internal_error_t error;
    utp_packet_out_t    *packet;

    if (control == NULL || ack == NULL || acknowledged_packets == NULL || result == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    result->ledger.largest_acknowledged_packet_number = 0u;
    result->ledger.largest_acknowledged_sent_time_us  = 0u;
    result->ledger.acknowledged_bytes                 = 0u;
    result->ledger.acknowledged_packet_count          = 0u;
    result->rtt_sample_us                             = 0u;
    result->rtt_sample_valid                          = false;
    {
        uint64_t inflight_before_ack = utp_send_ledger_bytes_in_flight(&control->ledger);

        error = utp_send_ledger_acknowledge(&control->ledger, ack, utp_send_history_largest(&control->send_history),
                                            acknowledged_packets, &result->ledger);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (control->was_quiet) {
            control->was_quiet = false;
            utp_congestion_was_quiet(control->congestion, now_us, inflight_before_ack);
        }
        utp_congestion_on_begin_ack(control->congestion, now_us, inflight_before_ack);
    }
    TAILQ_FOREACH(packet, acknowledged_packets, po_next) {
        utp_congestion_packet_info_t info;

        utp_send_control_packet_info(packet, &info);
        utp_congestion_on_ack(control->congestion, &info, now_us, control->app_limited ? 1 : 0);
        packet->bw_state = info.state;
    }
    utp_congestion_on_end_ack(control->congestion, utp_send_ledger_bytes_in_flight(&control->ledger));
    if (ack->largest_acked > control->largest_acked_packet_number) {
        control->largest_acked_packet_number = ack->largest_acked;
    }
    if (result->ledger.largest_acknowledged_packet_number != 0u) {
        uint64_t acknowledged_sent_time_us = result->ledger.largest_acknowledged_sent_time_us;

        control->largest_acked_sent_time_us = acknowledged_sent_time_us;
        if (now_us > acknowledged_sent_time_us) {
            error =
                utp_rtt_stats_update_from_ack(&control->rtt_stats, now_us, acknowledged_sent_time_us, ack->ack_delay,
                                              control->peer_max_ack_delay_us, &result->rtt_sample_us);
            result->rtt_sample_valid = error == UTP_INTERNAL_ERROR_OK;
        }
    }
    if (result->ledger.acknowledged_packet_count != 0u) {
        control->consecutive_rto_count          = 0u;
        control->handshake_retransmission_count = 0u;
        control->tlp_count                      = 0u;
        control->loss_pending                   = false;
    }
    if (utp_send_ledger_retransmittable_packet_count(&control->ledger) == 0u) {
        control->was_quiet = true;
    }
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_send_control_largest_sent(const utp_send_control_t *control) {
    return control == NULL ? 0u : utp_send_history_largest(&control->send_history);
}

uint64_t utp_send_control_largest_acked(const utp_send_control_t *control) {
    return control == NULL ? 0u : control->largest_acked_packet_number;
}

size_t utp_send_control_unacked_packet_count(const utp_send_control_t *control) {
    return control == NULL ? 0u : utp_send_ledger_packet_count(&control->ledger);
}

size_t utp_send_control_scheduled_packet_count(const utp_send_control_t *control) {
    return control == NULL ? 0u : control->scheduled_packet_count;
}

uint64_t utp_send_control_scheduled_bytes(const utp_send_control_t *control) {
    return control == NULL ? 0u : control->scheduled_byte_count;
}

size_t utp_send_control_lost_packet_count(const utp_send_control_t *control) {
    return control == NULL ? 0u : control->lost_packet_count;
}

size_t utp_send_control_discarded_packet_count(const utp_send_control_t *control) {
    return control == NULL ? 0u : control->discarded_packet_count;
}

uint64_t utp_send_control_srtt(const utp_send_control_t *control) {
    return control == NULL ? 0u : utp_rtt_stats_srtt(&control->rtt_stats);
}
