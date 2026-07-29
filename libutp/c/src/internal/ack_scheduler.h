#ifndef EULAR_UTP_INTERNAL_ACK_SCHEDULER_H
#define EULAR_UTP_INTERNAL_ACK_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "internal/error.h"

typedef enum utp_ack_schedule_decision {
    UTP_ACK_SCHEDULE_NONE = 0,
    UTP_ACK_SCHEDULE_DELAYED,
    UTP_ACK_SCHEDULE_IMMEDIATE
} utp_ack_schedule_decision_t;

typedef struct utp_ack_scheduler {
    uint32_t pending_count;
    uint64_t deadline;
    uint8_t  ack_eliciting_threshold;
    uint8_t  reordering_threshold;
    uint32_t max_ack_delay_ms;
} utp_ack_scheduler_t;

utp_internal_error_t        utp_ack_scheduler_init(utp_ack_scheduler_t *scheduler, uint8_t ack_eliciting_threshold,
                                                   uint8_t reordering_threshold, uint32_t max_ack_delay_ms);
utp_ack_schedule_decision_t utp_ack_scheduler_on_packet(utp_ack_scheduler_t *scheduler, uint64_t packet_number,
                                                        uint64_t largest_before, bool ack_eliciting,
                                                        bool has_handshake_done, uint64_t now);
void                        utp_ack_scheduler_on_ack_sent(utp_ack_scheduler_t *scheduler);
uint32_t                    utp_ack_scheduler_pending_count(const utp_ack_scheduler_t *scheduler);
uint64_t                    utp_ack_scheduler_deadline(const utp_ack_scheduler_t *scheduler);

#endif  // EULAR_UTP_INTERNAL_ACK_SCHEDULER_H
