#ifndef EULAR_UTP_INTERNAL_ACK_SCHEDULER_H
#define EULAR_UTP_INTERNAL_ACK_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "util/error.h"

typedef enum utp_ack_schedule_decision {
    UTP_ACK_SCHEDULE_NONE = 0,
    UTP_ACK_SCHEDULE_DELAYED,
    UTP_ACK_SCHEDULE_IMMEDIATE
} utp_ack_schedule_decision_t;

typedef struct utp_ack_scheduler {
    uint32_t pending_count;            // 待确认 ACK-eliciting 包数
    uint64_t deadline;                 // 延迟 ACK 截止时刻，单位 us
    uint8_t  ack_eliciting_threshold;  // 立即 ACK 的累计包数阈值
    uint8_t  reordering_threshold;     // 乱序缺口立即 ACK 阈值
    uint32_t max_ack_delay_ms;         // 最大 ACK 延迟，单位 ms
} utp_ack_scheduler_t;

utp_internal_error_t        utp_ack_scheduler_init(utp_ack_scheduler_t* scheduler, uint8_t ack_eliciting_threshold,
                                                   uint8_t reordering_threshold, uint32_t max_ack_delay_ms);
utp_ack_schedule_decision_t utp_ack_scheduler_on_packet(utp_ack_scheduler_t* scheduler, uint64_t packet_number,
                                                        uint64_t largest_before, bool ack_eliciting,
                                                        bool has_handshake_done, uint64_t now);
void                        utp_ack_scheduler_on_ack_sent(utp_ack_scheduler_t* scheduler);
uint32_t                    utp_ack_scheduler_pending_count(const utp_ack_scheduler_t* scheduler);
uint64_t                    utp_ack_scheduler_deadline(const utp_ack_scheduler_t* scheduler);

#endif  // EULAR_UTP_INTERNAL_ACK_SCHEDULER_H
