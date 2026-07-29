#ifndef EULAR_UTP_INTERNAL_RTT_H
#define EULAR_UTP_INTERNAL_RTT_H

#include <stdint.h>

#include "internal/error.h"

typedef struct utp_rtt_stats {
    uint64_t scaled_srtt;
    uint64_t variance;
    uint64_t minimum;
} utp_rtt_stats_t;

utp_internal_error_t utp_rtt_stats_update(utp_rtt_stats_t *stats, uint64_t measured_rtt);
uint64_t             utp_rtt_stats_srtt(const utp_rtt_stats_t *stats);
uint64_t             utp_rtt_stats_variance(const utp_rtt_stats_t *stats);
uint64_t             utp_rtt_stats_minimum(const utp_rtt_stats_t *stats);

#endif  // EULAR_UTP_INTERNAL_RTT_H
