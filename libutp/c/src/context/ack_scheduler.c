#include "context/ack_scheduler.h"

#include <assert.h>
#include <stddef.h>

utp_internal_error_t utp_ack_scheduler_init(utp_ack_scheduler_t* scheduler, uint8_t ack_eliciting_threshold,
                                            uint8_t reordering_threshold, uint32_t max_ack_delay_ms)
{
    assert(scheduler != NULL);
    if (ack_eliciting_threshold == 0u || reordering_threshold == 0u || max_ack_delay_ms == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    scheduler->ack_eliciting_threshold = ack_eliciting_threshold;
    scheduler->reordering_threshold    = reordering_threshold;
    scheduler->max_ack_delay_ms        = max_ack_delay_ms;
    scheduler->pending_count           = 0u;
    scheduler->deadline                = 0u;
    return UTP_INTERNAL_ERROR_OK;
}

utp_ack_schedule_decision_t utp_ack_scheduler_on_packet(utp_ack_scheduler_t* scheduler, uint64_t packet_number,
                                                        uint64_t largest_before, bool ack_eliciting,
                                                        bool has_handshake_done, uint64_t now)
{
    assert(scheduler != NULL);
    if (!ack_eliciting) {
        return UTP_ACK_SCHEDULE_NONE;
    }
    if (scheduler->pending_count != UINT32_MAX) {
        ++scheduler->pending_count;
    }
    bool reordered_gap =
        packet_number > largest_before && packet_number - largest_before - 1u >= scheduler->reordering_threshold;
    if ((has_handshake_done && scheduler->pending_count > 0u) ||
        scheduler->pending_count >= scheduler->ack_eliciting_threshold || reordered_gap) {
        // deadline == 0 means an immediate ACK while pending_count is non-zero.
        scheduler->deadline = 0u;
        return UTP_ACK_SCHEDULE_IMMEDIATE;
    }
    // Keep the first delayed-ACK deadline.  Extending it for every packet can
    // indefinitely postpone an ACK during a continuous receive burst.
    if (scheduler->deadline == 0u) {
        scheduler->deadline = now + (uint64_t)scheduler->max_ack_delay_ms * UINT64_C(1000);
    }
    return UTP_ACK_SCHEDULE_DELAYED;
}

void utp_ack_scheduler_on_ack_queued(utp_ack_scheduler_t* scheduler)
{
    assert(scheduler != NULL);
    scheduler->pending_count = 0u;
    scheduler->deadline      = 0u;
}

uint32_t utp_ack_scheduler_pending_count(const utp_ack_scheduler_t* scheduler)
{
    assert(scheduler != NULL);
    return scheduler->pending_count;
}

uint64_t utp_ack_scheduler_deadline(const utp_ack_scheduler_t* scheduler)
{
    assert(scheduler != NULL);
    return scheduler->deadline;
}
