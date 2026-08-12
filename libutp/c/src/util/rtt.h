#ifndef EULAR_UTP_INTERNAL_RTT_H
#define EULAR_UTP_INTERNAL_RTT_H

#include <stdint.h>

#include "util/error.h"

typedef struct utp_rtt_stats {
    uint64_t scaled_srtt;  // 左移三位的平滑 RTT，单位 us
    uint64_t variance;     // RTT 方差，单位 us
    uint64_t minimum;      // 历史最小 RTT，单位 us
} utp_rtt_stats_t;

utp_internal_error_t utp_rtt_stats_update(utp_rtt_stats_t* stats, uint64_t measured_rtt);
utp_internal_error_t utp_rtt_stats_update_from_ack(utp_rtt_stats_t* stats, uint64_t now_us, uint64_t sent_time_us,
                                                   uint64_t peer_ack_delay_us, uint64_t peer_max_ack_delay_us,
                                                   uint64_t* sample_rtt_us);
uint64_t             utp_rtt_stats_srtt(const utp_rtt_stats_t* stats);
uint64_t             utp_rtt_stats_variance(const utp_rtt_stats_t* stats);
uint64_t             utp_rtt_stats_minimum(const utp_rtt_stats_t* stats);

#endif  // EULAR_UTP_INTERNAL_RTT_H
