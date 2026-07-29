#include "internal/ack_scheduler.h"

#include <string.h>

utp_internal_error_t utp_ack_scheduler_init(utp_ack_scheduler_t *scheduler, uint8_t ack_eliciting_threshold,
                                            uint8_t reordering_threshold, uint32_t max_ack_delay_ms) {
    if (scheduler == NULL || ack_eliciting_threshold == 0u || reordering_threshold == 0u || max_ack_delay_ms == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->ack_eliciting_threshold = ack_eliciting_threshold;
    scheduler->reordering_threshold    = reordering_threshold;
    scheduler->max_ack_delay_ms        = max_ack_delay_ms;
    return UTP_INTERNAL_ERROR_OK;
}

utp_ack_schedule_decision_t utp_ack_scheduler_on_packet(utp_ack_scheduler_t *scheduler, uint64_t packet_number,
                                                        uint64_t largest_before, bool ack_eliciting,
                                                        bool has_handshake_done, uint64_t now) {
    bool reordered_gap;

    if (scheduler == NULL || !ack_eliciting) {
        return UTP_ACK_SCHEDULE_NONE;
    }
    if (scheduler->pending_count != UINT32_MAX) {
        ++scheduler->pending_count;
    }
    reordered_gap =
        packet_number > largest_before && packet_number - largest_before - 1u >= scheduler->reordering_threshold;
    if ((has_handshake_done && scheduler->pending_count > 0u) ||
        scheduler->pending_count >= scheduler->ack_eliciting_threshold || reordered_gap) {
        return UTP_ACK_SCHEDULE_IMMEDIATE;
    }
    scheduler->deadline = now + (uint64_t)scheduler->max_ack_delay_ms * UINT64_C(1000);
    return UTP_ACK_SCHEDULE_DELAYED;
}

void utp_ack_scheduler_on_ack_sent(utp_ack_scheduler_t *scheduler) {
    if (scheduler != NULL) {
        scheduler->pending_count = 0u;
        scheduler->deadline      = 0u;
    }
}

uint32_t utp_ack_scheduler_pending_count(const utp_ack_scheduler_t *scheduler) {
    return scheduler == NULL ? 0u : scheduler->pending_count;
}

uint64_t utp_ack_scheduler_deadline(const utp_ack_scheduler_t *scheduler) {
    return scheduler == NULL ? 0u : scheduler->deadline;
}
