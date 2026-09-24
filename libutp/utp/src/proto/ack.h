#ifndef EULAR_UTP_INTERNAL_ACK_H
#define EULAR_UTP_INTERNAL_ACK_H

#include <stddef.h>
#include <stdint.h>

#include "util/error.h"
#include "util/receive_history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_ACK_FRAME_HEADER_SIZE  16u
#define UTP_ACK_FRAME_RANGE_SIZE   8u
#define UTP_ACK_MAX_RANGES         256u
#define UTP_ACK_MAX_DELAY_EXPONENT 20u

typedef struct utp_ack_range {
    uint64_t low;   // 确认区间起始包号（含）
    uint64_t high;  // 确认区间结束包号（含）
} utp_ack_range_t;

typedef struct utp_ack_info {
    uint64_t         largest_acked;   // 最大已确认包号
    uint64_t         ack_delay;       // 解码后的 ACK 延迟，单位 us
    utp_ack_range_t* ranges;          // 调用方提供的确认区间数组
    size_t           range_count;     // 当前确认区间数
    size_t           range_capacity;  // 区间数组容量
} utp_ack_info_t;

utp_internal_error_t utp_ack_from_receive_history(utp_ack_info_t* ack, const utp_receive_history_t* history,
                                                  uint64_t now, size_t max_ranges);
utp_internal_error_t utp_ack_encode(uint8_t* buffer, size_t capacity, const utp_ack_info_t* ack,
                                    uint8_t ack_delay_exponent, size_t* encoded_length);
utp_internal_error_t utp_ack_decode(utp_ack_info_t* ack, const uint8_t* frame, size_t frame_length,
                                    uint8_t ack_delay_exponent, size_t* consumed);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_ACK_H
