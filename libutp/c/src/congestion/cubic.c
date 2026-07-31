/*
 * CUBIC is based on the control flow and constants in lsquic_cubic.c.
 * Copyright (c) 2017 - 2026 LiteSpeed Technologies Inc.  See LICENSE.
 * Protocol-visible parameters and recovery behavior follow libutp's C++ implementation.
 */

#include "congestion/cubic.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define UTP_CUBIC_MAX_CWND         (UINT64_C(2000) * UTP_CUBIC_DEFAULT_MSS)
#define UTP_CUBIC_DEFAULT_BETA     0.7
#define UTP_CUBIC_DEFAULT_C        0.4
#define UTP_CUBIC_DEFAULT_MIN_MSS  4u
#define UTP_CUBIC_DEFAULT_INIT_MSS 32u
#define UTP_CUBIC_FALLBACK_RTT_US  UINT64_C(25000)
#define UTP_CUBIC_MIN_RTT_US       UINT64_C(1000)

static uint64_t utp_cubic_min(uint64_t first, uint64_t second) { return first < second ? first : second; }

static uint64_t utp_cubic_max(uint64_t first, uint64_t second) { return first > second ? first : second; }

static uint64_t utp_cubic_mss_to_bytes(uint32_t mss) {
    uint64_t value = mss == 0u ? UINT64_C(1) : mss;

    if (value > UTP_CUBIC_MAX_CWND / UTP_CUBIC_DEFAULT_MSS) {
        return UTP_CUBIC_MAX_CWND;
    }
    return value * UTP_CUBIC_DEFAULT_MSS;
}

static uint64_t utp_cubic_smoothed_rtt(const utp_cubic_t* cubic) {
    uint64_t srtt = UTP_CUBIC_FALLBACK_RTT_US;

    if (cubic->rtt_stats != NULL && utp_rtt_stats_srtt(cubic->rtt_stats) != 0u) {
        srtt = utp_cubic_max(UTP_CUBIC_MIN_RTT_US, utp_rtt_stats_srtt(cubic->rtt_stats));
    }
    return srtt;
}

static void utp_cubic_reset_epoch(utp_cubic_t* cubic) {
    cubic->epoch_start_us    = 0u;
    cubic->origin_point_cwnd = 0u;
    cubic->k                 = 0.0;
    cubic->acked_bytes       = 0u;
}

static void utp_cubic_ensure_epoch(utp_cubic_t* cubic, uint64_t now_us) {
    double cwnd_packets;
    double delta_packets;
    double max_packets;

    if (cubic->epoch_start_us != 0u) {
        return;
    }
    cubic->epoch_start_us = now_us;
    if (cubic->last_max_cwnd > cubic->cwnd) {
        cubic->origin_point_cwnd = cubic->last_max_cwnd;
        max_packets              = (double)cubic->last_max_cwnd / (double)UTP_CUBIC_DEFAULT_MSS;
        cwnd_packets             = (double)cubic->cwnd / (double)UTP_CUBIC_DEFAULT_MSS;
        delta_packets            = max_packets > cwnd_packets ? max_packets - cwnd_packets : 0.0;
        cubic->k                 = cbrt(delta_packets / cubic->cubic_c);
    } else {
        cubic->origin_point_cwnd = cubic->cwnd;
        cubic->k                 = 0.0;
    }
}

static uint64_t utp_cubic_target_cwnd(const utp_cubic_t* cubic, uint64_t now_us) {
    double elapsed_seconds;
    double dt;
    double origin_packets;
    double target_packets;
    double target_bytes;

    if (cubic->epoch_start_us == 0u || now_us < cubic->epoch_start_us) {
        return cubic->cwnd;
    }
    elapsed_seconds  = (double)(now_us - cubic->epoch_start_us) / 1000000.0;
    elapsed_seconds += (double)utp_cubic_smoothed_rtt(cubic) / 1000000.0;
    dt               = elapsed_seconds - cubic->k;
    origin_packets   = (double)cubic->origin_point_cwnd / (double)UTP_CUBIC_DEFAULT_MSS;
    target_packets   = cubic->cubic_c * dt * dt * dt + origin_packets;
    if (target_packets < 0.0) {
        target_packets = 0.0;
    }
    target_bytes = target_packets * (double)UTP_CUBIC_DEFAULT_MSS;
    if (target_bytes >= (double)UINT64_MAX) {
        return UTP_CUBIC_MAX_CWND;
    }
    return utp_cubic_max(cubic->minimum_cwnd, (uint64_t)target_bytes);
}

static uint64_t utp_cubic_cubic_increment(const utp_cubic_t* cubic, uint64_t acked_bytes, uint64_t now_us) {
    uint64_t target;
    uint64_t cwnd;
    uint64_t distance;

    if (acked_bytes == 0u) {
        return 0u;
    }
    target = utp_cubic_target_cwnd(cubic, now_us);
    cwnd   = utp_cubic_max(cubic->cwnd, UINT64_C(1));
    if (target <= cwnd) {
        if (acked_bytes > UINT64_MAX / UTP_CUBIC_DEFAULT_MSS) {
            return UTP_CUBIC_MAX_CWND - cwnd;
        }
        return utp_cubic_max(UINT64_C(1), (acked_bytes * UTP_CUBIC_DEFAULT_MSS) / (cwnd * UINT64_C(100)));
    }
    distance = target - cwnd;
    if (acked_bytes > UINT64_MAX / distance) {
        return UTP_CUBIC_MAX_CWND - cwnd;
    }
    return utp_cubic_max(UINT64_C(1), (acked_bytes * distance) / cwnd);
}

static uint64_t utp_cubic_reno_increment(const utp_cubic_t* cubic, uint64_t acked_bytes) {
    uint64_t cwnd = utp_cubic_max(cubic->cwnd, UINT64_C(1));

    if (acked_bytes == 0u) {
        return 0u;
    }
    if (acked_bytes > UINT64_MAX / UTP_CUBIC_DEFAULT_MSS) {
        return UTP_CUBIC_MAX_CWND - cwnd;
    }
    return utp_cubic_max(UINT64_C(1), (acked_bytes * UTP_CUBIC_DEFAULT_MSS) / cwnd);
}

static uint64_t utp_cubic_get_cwnd(void* state) { return state == NULL ? 0u : ((const utp_cubic_t*)state)->cwnd; }

static uint64_t utp_cubic_get_pacing_rate(void* state, int32_t in_recovery) {
    const utp_cubic_t* cubic = state;
    uint64_t           srtt;
    uint64_t           base_rate;
    uint64_t           gain_percent;

    if (cubic == NULL) {
        return 0u;
    }
    srtt          = utp_cubic_smoothed_rtt(cubic);
    base_rate     = cubic->cwnd > UINT64_MAX / UINT64_C(1000000) ? UINT64_MAX : cubic->cwnd * UINT64_C(1000000);
    base_rate    /= srtt;
    gain_percent  = in_recovery != 0 ? UINT64_C(100) : UINT64_C(125);
    return base_rate > UINT64_MAX / gain_percent ? UINT64_MAX : (base_rate * gain_percent) / UINT64_C(100);
}

static void utp_cubic_on_init(void* state, const utp_rtt_stats_t* rtt_stats) {
    utp_cubic_t* cubic = state;

    if (cubic == NULL) {
        return;
    }
    cubic->rtt_stats     = rtt_stats;
    cubic->cwnd          = cubic->initial_cwnd;
    cubic->ssthresh      = UTP_CUBIC_MAX_CWND;
    cubic->last_max_cwnd = 0u;
    utp_cubic_reset_epoch(cubic);
}

static void utp_cubic_on_ack(void* state, utp_congestion_packet_info_t* packet, uint64_t now_us, int32_t app_limited) {
    utp_cubic_t* cubic = state;
    uint64_t     acked_bytes;
    uint64_t     cubic_increment;
    uint64_t     reno_increment;
    uint64_t     increment;

    (void)app_limited;
    if (cubic == NULL || packet == NULL) {
        return;
    }
    acked_bytes        = packet->packet_size == 0u ? UINT64_C(1) : packet->packet_size;
    cubic->acked_bytes = cubic->acked_bytes > UINT64_MAX - acked_bytes ? UINT64_MAX : cubic->acked_bytes + acked_bytes;
    if (cubic->cwnd < cubic->ssthresh) {
        cubic->cwnd = utp_cubic_min(UTP_CUBIC_MAX_CWND, cubic->cwnd + acked_bytes);
        return;
    }
    utp_cubic_ensure_epoch(cubic, now_us);
    cubic_increment = utp_cubic_cubic_increment(cubic, acked_bytes, now_us);
    reno_increment  = utp_cubic_reno_increment(cubic, acked_bytes);
    increment       = utp_cubic_max(cubic_increment, reno_increment);
    cubic->cwnd     = increment > UTP_CUBIC_MAX_CWND - cubic->cwnd ? UTP_CUBIC_MAX_CWND : cubic->cwnd + increment;
}

static void utp_cubic_on_lost(void* state, utp_congestion_packet_info_t* packet) {
    utp_cubic_t* cubic = state;

    (void)packet;
    if (cubic == NULL) {
        return;
    }
    if (cubic->cwnd < cubic->last_max_cwnd) {
        cubic->last_max_cwnd = (uint64_t)((double)cubic->cwnd * (2.0 - cubic->beta) / 2.0);
    } else {
        cubic->last_max_cwnd = cubic->cwnd;
    }
    cubic->cwnd     = utp_cubic_max((uint64_t)((double)cubic->cwnd * cubic->beta), cubic->minimum_cwnd);
    cubic->ssthresh = cubic->cwnd;
    utp_cubic_reset_epoch(cubic);
}

static void utp_cubic_was_quiet(void* state, uint64_t now_us, uint64_t inflight_bytes) {
    utp_cubic_t* cubic = state;

    (void)now_us;
    if (cubic != NULL && inflight_bytes == 0u) {
        utp_cubic_reset_epoch(cubic);
    }
}

static void utp_cubic_on_timeout(void* state) {
    utp_cubic_t* cubic = state;

    if (cubic == NULL) {
        return;
    }
    cubic->last_max_cwnd = cubic->cwnd;
    cubic->ssthresh      = utp_cubic_max((uint64_t)((double)cubic->cwnd * cubic->beta), cubic->minimum_cwnd);
    cubic->cwnd          = cubic->minimum_cwnd;
    utp_cubic_reset_epoch(cubic);
}

static const utp_congestion_ops_t k_utp_cubic_ops = {
    utp_cubic_on_init,    utp_cubic_get_cwnd, utp_cubic_get_pacing_rate, NULL, NULL,
    utp_cubic_on_ack,     utp_cubic_on_lost,  utp_cubic_was_quiet,       NULL, NULL,
    utp_cubic_on_timeout,
};

void utp_cubic_init(utp_cubic_t* cubic, const utp_cubic_config_t* config) {
    uint64_t minimum_cwnd;
    uint64_t initial_cwnd;

    if (cubic == NULL) {
        return;
    }
    memset(cubic, 0, sizeof(*cubic));
    cubic->beta = config != NULL && config->beta > 0.0 && config->beta < 1.0 ? config->beta : UTP_CUBIC_DEFAULT_BETA;
    cubic->cubic_c =
        config != NULL && config->cubic_c > 0.0 && config->cubic_c <= 2.0 ? config->cubic_c : UTP_CUBIC_DEFAULT_C;
    minimum_cwnd = utp_cubic_mss_to_bytes(config == NULL ? UTP_CUBIC_DEFAULT_MIN_MSS : config->minimum_cwnd_mss);
    initial_cwnd = utp_cubic_mss_to_bytes(config == NULL ? UTP_CUBIC_DEFAULT_INIT_MSS : config->initial_cwnd_mss);
    cubic->minimum_cwnd     = utp_cubic_min(minimum_cwnd, UTP_CUBIC_MAX_CWND);
    cubic->initial_cwnd     = utp_cubic_min(utp_cubic_max(initial_cwnd, cubic->minimum_cwnd), UTP_CUBIC_MAX_CWND);
    cubic->cwnd             = cubic->initial_cwnd;
    cubic->ssthresh         = UTP_CUBIC_MAX_CWND;
    cubic->congestion.state = cubic;
    cubic->congestion.ops   = &k_utp_cubic_ops;
}

utp_congestion_t* utp_cubic_as_congestion(utp_cubic_t* cubic) { return cubic == NULL ? NULL : &cubic->congestion; }

const utp_congestion_t* utp_cubic_as_const_congestion(const utp_cubic_t* cubic) {
    return cubic == NULL ? NULL : &cubic->congestion;
}
