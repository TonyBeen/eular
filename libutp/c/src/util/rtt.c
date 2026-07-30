#include "util/rtt.h"

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

utp_internal_error_t utp_rtt_stats_update_from_ack(utp_rtt_stats_t *stats, uint64_t now_us, uint64_t sent_time_us,
                                                   uint64_t peer_ack_delay_us, uint64_t peer_max_ack_delay_us,
                                                   uint64_t *sample_rtt_us) {
    uint64_t             sample;
    uint64_t             ack_delay;
    utp_internal_error_t error;

    if (stats == NULL || sample_rtt_us == NULL || now_us <= sent_time_us) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    sample    = now_us - sent_time_us;
    ack_delay = peer_ack_delay_us < peer_max_ack_delay_us ? peer_ack_delay_us : peer_max_ack_delay_us;
    if (ack_delay != 0u && sample > ack_delay) {
        sample -= ack_delay;
    }
    if (sample == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_rtt_stats_update(stats, sample);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *sample_rtt_us = sample;
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_rtt_stats_srtt(const utp_rtt_stats_t *stats) { return stats == NULL ? 0u : stats->scaled_srtt >> 3u; }
uint64_t utp_rtt_stats_variance(const utp_rtt_stats_t *stats) { return stats == NULL ? 0u : stats->variance; }
uint64_t utp_rtt_stats_minimum(const utp_rtt_stats_t *stats) { return stats == NULL ? 0u : stats->minimum; }
