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
    uint64_t low;
    uint64_t high;
} utp_ack_range_t;

typedef struct utp_ack_info {
    uint64_t         largest_acked;
    uint64_t         ack_delay;
    utp_ack_range_t *ranges;
    size_t           range_count;
    size_t           range_capacity;
} utp_ack_info_t;

utp_internal_error_t utp_ack_from_receive_history(utp_ack_info_t *ack, const utp_receive_history_t *history,
                                                  uint64_t now, size_t max_ranges);
utp_internal_error_t utp_ack_encode(uint8_t *buffer, size_t capacity, const utp_ack_info_t *ack,
                                    uint8_t ack_delay_exponent, size_t *encoded_length);
utp_internal_error_t utp_ack_decode(utp_ack_info_t *ack, const uint8_t *frame, size_t frame_length,
                                    uint8_t ack_delay_exponent, size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_ACK_H
