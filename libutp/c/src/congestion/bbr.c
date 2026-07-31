#include "congestion/bbr.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define BBR_MSS            UINT64_C(1460)
#define BBR_MAX_CWND       (UINT64_C(2000) * BBR_MSS)
#define BBR_MIN_RTT_EXPIRY UINT64_C(10000000)
#define BBR_PROBE_RTT_TIME UINT64_C(200000)
#define BBR_HIGH_GAIN      2.885

static const double k_bbr_gains[8] = {1.25, .75, 1., 1., 1., 1., 1., 1.};

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }
static uint64_t sat_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
static int32_t  in_recovery(const utp_bbr_t *b) { return b->recovery_state != UTP_BBR_NOT_IN_RECOVERY ? 1 : 0; }
static uint64_t min_rtt(const utp_bbr_t *b) {
    uint64_t r = b->min_rtt_us;
    if (r == 0u && b->rtt_stats != NULL) r = utp_rtt_stats_minimum(b->rtt_stats);
    return r == 0u ? 25000u : r;
}
static uint64_t target_cwnd(const utp_bbr_t *b, double gain) {
    uint64_t bw   = utp_minmax_get(&b->max_bandwidth);
    uint64_t rtt  = min_rtt(b);
    uint64_t bdp  = bw > UINT64_MAX / rtt ? UINT64_MAX : bw * rtt / UINT64_C(1000000);
    uint64_t cwnd = (uint64_t)(gain * (double)bdp);
    return max64(cwnd == 0u ? (uint64_t)(gain * (double)b->initial_cwnd) : cwnd, b->minimum_cwnd);
}
static uint64_t probe_rtt_cwnd(const utp_bbr_t *b) { return b->minimum_cwnd; }
static void     set_startup(utp_bbr_t *b) {
    b->mode        = UTP_BBR_STARTUP;
    b->pacing_gain = b->high_gain;
    b->cwnd_gain   = b->high_cwnd_gain;
}
static void enter_probe_bw(utp_bbr_t *b, uint64_t now) {
    b->mode                = UTP_BBR_PROBE_BW;
    b->cwnd_gain           = 2.;
    b->cycle_index         = 2u;
    b->pacing_gain         = k_bbr_gains[b->cycle_index];
    b->last_cycle_start_us = now;
}
static void update_pacing(utp_bbr_t *b) {
    uint64_t bw = utp_minmax_get(&b->max_bandwidth);
    uint64_t target;
    if (bw == 0u) {
        b->pacing_rate = b->initial_cwnd * UINT64_C(1000000) / min_rtt(b);
        return;
    }
    target = (uint64_t)((double)bw * b->pacing_gain);
    if (b->full_bandwidth_reached || target > b->pacing_rate) b->pacing_rate = target;
}
static void update_recovery(utp_bbr_t *b, int32_t round_start) {
    if (b->ack_has_losses) b->end_recovery_packet_number = b->last_sent_packet_number;
    if (b->recovery_state == UTP_BBR_NOT_IN_RECOVERY && b->ack_has_losses) {
        b->recovery_state                  = UTP_BBR_CONSERVATION;
        b->recovery_window                 = 0u;
        b->current_round_end_packet_number = b->last_sent_packet_number;
    } else if (b->recovery_state == UTP_BBR_CONSERVATION && round_start)
        b->recovery_state = UTP_BBR_GROWTH;
    else if (b->recovery_state == UTP_BBR_GROWTH && !b->ack_has_losses &&
             b->last_sent_packet_number > b->end_recovery_packet_number)
        b->recovery_state = UTP_BBR_NOT_IN_RECOVERY;
}
static void update_gain_cycle(utp_bbr_t *b, uint64_t inflight) {
    uint64_t target  = target_cwnd(b, 1.);
    int32_t  advance = b->ack_time_us - b->last_cycle_start_us >= min_rtt(b) ? 1 : 0;
    if (b->pacing_gain > 1. && !b->ack_has_losses && b->inflight_bytes < target_cwnd(b, b->pacing_gain)) advance = 0;
    if (b->pacing_gain < 1. && inflight <= target) advance = 1;
    if (advance) {
        b->cycle_index         = (b->cycle_index + 1u) % 8u;
        b->pacing_gain         = k_bbr_gains[b->cycle_index];
        b->last_cycle_start_us = b->ack_time_us;
    }
}
static void check_full_bw(utp_bbr_t *b) {
    uint64_t bw = utp_minmax_get(&b->max_bandwidth);
    if (b->last_sample_app_limited) return;
    if (bw >= (uint64_t)((double)b->bandwidth_at_last_round * 1.25)) {
        b->bandwidth_at_last_round       = bw;
        b->rounds_without_bandwidth_gain = 0u;
    } else if (++b->rounds_without_bandwidth_gain >= b->startup_round_limit)
        b->full_bandwidth_reached = true;
}
static void update_cwnd(utp_bbr_t *b, uint64_t excess) {
    uint64_t target;
    if (b->mode == UTP_BBR_PROBE_RTT) return;
    target = target_cwnd(b, b->cwnd_gain);
    if (b->full_bandwidth_reached)
        target = sat_add(target, utp_minmax_get(&b->max_ack_height));
    else
        target = sat_add(target, excess);
    if (b->full_bandwidth_reached)
        b->cwnd = min64(target, sat_add(b->cwnd, b->acked_bytes));
    else if (b->cwnd < target || b->sampler.total_bytes_acked < b->initial_cwnd)
        b->cwnd = sat_add(b->cwnd, b->acked_bytes);
    b->cwnd = min64(max64(b->cwnd, b->minimum_cwnd), b->max_cwnd);
}
static void update_recovery_window(utp_bbr_t *b, uint64_t inflight) {
    if (!in_recovery(b)) return;
    if (b->recovery_window == 0u) {
        b->recovery_window = max64(b->minimum_cwnd, sat_add(inflight, b->acked_bytes));
        return;
    }
    b->recovery_window = b->recovery_window > b->lost_bytes ? b->recovery_window - b->lost_bytes : BBR_MSS;
    if (b->recovery_state == UTP_BBR_GROWTH) b->recovery_window = sat_add(b->recovery_window, b->acked_bytes);
    b->recovery_window = max64(b->recovery_window, max64(b->minimum_cwnd, sat_add(inflight, b->acked_bytes)));
}
static uint64_t get_cwnd(void *s) {
    utp_bbr_t *b = s;
    if (b == NULL) return 0u;
    if (b->mode == UTP_BBR_PROBE_RTT) return probe_rtt_cwnd(b);
    return in_recovery(b) ? min64(b->cwnd, b->recovery_window) : b->cwnd;
}
static uint64_t get_rate(void *s, int32_t r) {
    utp_bbr_t *b = s;
    (void)r;
    return b == NULL ? 0u : b->pacing_rate;
}
static void on_init(void *s, const utp_rtt_stats_t *rtt) {
    utp_bbr_t *b = s;
    if (b != NULL) {
        b->rtt_stats = rtt;
        b->cwnd      = b->initial_cwnd;
        set_startup(b);
    }
}
static void on_sent(void *s, utp_congestion_packet_info_t *p, uint64_t in, int32_t app) {
    utp_bbr_t *b = s;
    if (b == NULL || p == NULL) return;
    utp_bw_sampler_on_packet_sent(&b->sampler, p->state, p->packet_number, p->packet_size, p->sent_time_us);
    b->last_sent_packet_number = p->packet_number;
    if (app && in < get_cwnd(b)) {
        b->app_limited_since_last_probe = true;
        utp_bw_sampler_set_app_limited(&b->sampler);
    }
}
static void begin_ack(void *s, uint64_t now, uint64_t in) {
    utp_bbr_t *b = s;
    if (b != NULL) {
        b->in_ack         = true;
        b->ack_time_us    = now;
        b->inflight_bytes = in;
        b->acked_bytes    = 0;
        b->lost_bytes     = 0;
        b->ack_has_losses = false;
        b->ack_has_sample = false;
    }
}
static void on_ack(void *s, utp_congestion_packet_info_t *p, uint64_t now, int32_t app) {
    utp_bbr_t      *b = s;
    utp_bw_sample_t x;
    (void)app;
    if (b == NULL || p == NULL) return;
    b->acked_bytes = sat_add(b->acked_bytes, p->packet_size);
    if (utp_bw_sampler_on_packet_acked(&b->sampler, p->state, p->packet_number, now, &x)) {
        b->ack_has_sample          = true;
        b->last_sample_app_limited = x.is_app_limited;
        if (!x.is_app_limited) b->has_non_app_limited_sample = true;
        if (!x.is_app_limited || x.bandwidth_bytes_per_second > utp_minmax_get(&b->max_bandwidth))
            utp_minmax_update_max(&b->max_bandwidth, b->round_count, x.bandwidth_bytes_per_second);
        if (b->min_rtt_us == 0u || x.rtt_us < b->min_rtt_us) b->min_rtt_us = x.rtt_us;
    }
}
static void on_lost(void *s, utp_congestion_packet_info_t *p) {
    utp_bbr_t *b = s;
    if (b != NULL && p != NULL) {
        utp_bw_sampler_on_packet_lost(&b->sampler, p->state);
        b->lost_bytes     = sat_add(b->lost_bytes, p->packet_size);
        b->ack_has_losses = true;
    }
}
static void end_ack(void *s, uint64_t in) {
    utp_bbr_t *b = s;
    uint64_t   expected, excess = 0;
    if (b == NULL || !b->in_ack) return;
    b->in_ack = false;
    if (b->acked_bytes > 0u) {
        int32_t round =
            b->current_round_end_packet_number == 0u || b->last_sent_packet_number > b->current_round_end_packet_number
                ? 1
                : 0;
        if (round) {
            ++b->round_count;
            b->current_round_end_packet_number = b->last_sent_packet_number;
        }
        update_recovery(b, round);
        if (b->mode == UTP_BBR_PROBE_BW) update_gain_cycle(b, in);
        if (round && !b->full_bandwidth_reached) check_full_bw(b);
        expected =
            utp_minmax_get(&b->max_bandwidth) * (b->ack_time_us - b->aggregation_epoch_start_us) / UINT64_C(1000000);
        if (b->aggregation_epoch_bytes <= expected) {
            b->aggregation_epoch_start_us = b->ack_time_us;
            b->aggregation_epoch_bytes    = b->acked_bytes;
        } else {
            b->aggregation_epoch_bytes = sat_add(b->aggregation_epoch_bytes, b->acked_bytes);
            excess                     = b->aggregation_epoch_bytes - expected;
            utp_minmax_update_max(&b->max_ack_height, b->round_count, excess);
        }
        if (b->mode == UTP_BBR_STARTUP && b->full_bandwidth_reached) {
            b->mode        = UTP_BBR_DRAIN;
            b->pacing_gain = b->drain_gain;
        }
        if (b->mode == UTP_BBR_DRAIN && in <= target_cwnd(b, 1.)) enter_probe_bw(b, b->ack_time_us);
        if (b->min_rtt_us != 0u && b->ack_time_us - b->min_rtt_timestamp_us > BBR_MIN_RTT_EXPIRY &&
            b->mode != UTP_BBR_PROBE_RTT) {
            b->mode              = UTP_BBR_PROBE_RTT;
            b->pacing_gain       = 1.;
            b->probe_rtt_exit_us = 0u;
        }
        if (b->mode == UTP_BBR_PROBE_RTT) {
            if (b->probe_rtt_exit_us == 0u && in < sat_add(probe_rtt_cwnd(b), BBR_MSS))
                b->probe_rtt_exit_us = sat_add(b->ack_time_us, BBR_PROBE_RTT_TIME);
            else if (b->probe_rtt_exit_us != 0u && b->ack_time_us >= b->probe_rtt_exit_us && round) {
                b->min_rtt_timestamp_us = b->ack_time_us;
                enter_probe_bw(b, b->ack_time_us);
            }
        }
    }
    update_pacing(b);
    update_cwnd(b, excess);
    update_recovery_window(b, in);
}
static const utp_congestion_ops_t ops = {on_init, get_cwnd, get_rate, begin_ack, on_sent, on_ack,
                                         on_lost, NULL,     end_ack,  NULL,      NULL};
void                              utp_bbr_init(utp_bbr_t *b, const utp_bbr_config_t *c) {
    uint32_t i = c == NULL || c->initial_cwnd_mss == 0u ? 16u : c->initial_cwnd_mss,
             m = c == NULL || c->minimum_cwnd_mss == 0u ? 4u : c->minimum_cwnd_mss;
    if (b == NULL) return;
    memset(b, 0, sizeof(*b));
    b->initial_cwnd                = max64((uint64_t)i * BBR_MSS, (uint64_t)m * BBR_MSS);
    b->minimum_cwnd                = (uint64_t)m * BBR_MSS;
    b->max_cwnd                    = BBR_MAX_CWND;
    b->high_gain                   = BBR_HIGH_GAIN;
    b->high_cwnd_gain              = BBR_HIGH_GAIN;
    b->drain_gain                  = 1. / BBR_HIGH_GAIN;
    b->startup_round_limit         = 3u;
    b->min_rtt_since_last_probe_us = UINT64_MAX;
    b->congestion.state            = b;
    b->congestion.ops              = &ops;
    utp_bw_sampler_init(&b->sampler);
    utp_minmax_init(&b->max_bandwidth, 10u);
    utp_minmax_init(&b->max_ack_height, 10u);
    set_startup(b);
    b->cwnd = b->initial_cwnd;
}
utp_congestion_t *utp_bbr_as_congestion(utp_bbr_t *b) { return b == NULL ? NULL : &b->congestion; }
