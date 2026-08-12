#ifndef EULAR_UTP_INTERNAL_SEND_HISTORY_H
#define EULAR_UTP_INTERNAL_SEND_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "util/error.h"

typedef struct utp_send_history {
    uint64_t largest;                // 最大已发送包号
    uint64_t gap_warning_threshold;  // 触发跳号告警的阈值
    bool     gap_detected;           // 是否检测到异常跳号
} utp_send_history_t;

void                 utp_send_history_init(utp_send_history_t* history, uint64_t gap_warning_threshold);
utp_internal_error_t utp_send_history_update(utp_send_history_t* history, uint64_t packet_number);
uint64_t             utp_send_history_largest(const utp_send_history_t* history);
bool                 utp_send_history_gap_detected(const utp_send_history_t* history);

#endif  // EULAR_UTP_INTERNAL_SEND_HISTORY_H
