#include "internal/rtt.h"

#include <stddef.h>

utp_internal_error_t utp_rtt_stats_update(utp_rtt_stats_t *stats, uint64_t measured_rtt) {
    uint64_t srtt;
    uint64_t delta;

    if (stats == NULL || measured_rtt == 0u || measured_rtt > UINT64_MAX / 8u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (stats->scaled_srtt == 0u) {
        stats->scaled_srtt = measured_rtt << 3u;
        stats->variance    = measured_rtt >> 1u;
        stats->minimum     = measured_rtt;
        return UTP_INTERNAL_ERROR_OK;
    }
    srtt                = stats->scaled_srtt >> 3u;
    delta               = srtt > measured_rtt ? srtt - measured_rtt : measured_rtt - srtt;
    stats->scaled_srtt += measured_rtt - srtt;
    stats->variance     = stats->variance - (stats->variance >> 2u) + (delta >> 2u);
    if (measured_rtt < stats->minimum) {
        stats->minimum = measured_rtt;
    }
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_rtt_stats_srtt(const utp_rtt_stats_t *stats) { return stats == NULL ? 0u : stats->scaled_srtt >> 3u; }
uint64_t utp_rtt_stats_variance(const utp_rtt_stats_t *stats) { return stats == NULL ? 0u : stats->variance; }
uint64_t utp_rtt_stats_minimum(const utp_rtt_stats_t *stats) { return stats == NULL ? 0u : stats->minimum; }
