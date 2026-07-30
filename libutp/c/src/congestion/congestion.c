#include "congestion/congestion.h"

#include <stddef.h>

static int utp_congestion_has_ops(const utp_congestion_t *congestion) {
    return congestion != NULL && congestion->ops != NULL;
}

uint64_t utp_congestion_get_cwnd(const utp_congestion_t *congestion) {
    return utp_congestion_has_ops(congestion) && congestion->ops->get_cwnd != NULL
               ? congestion->ops->get_cwnd(congestion->state)
               : 0u;
}

uint64_t utp_congestion_get_pacing_rate(const utp_congestion_t *congestion, int in_recovery) {
    return utp_congestion_has_ops(congestion) && congestion->ops->get_pacing_rate != NULL
               ? congestion->ops->get_pacing_rate(congestion->state, in_recovery)
               : 0u;
}

void utp_congestion_init(utp_congestion_t *congestion, const utp_rtt_stats_t *rtt_stats) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_init != NULL) {
        congestion->ops->on_init(congestion->state, rtt_stats);
    }
}

void utp_congestion_on_begin_ack(utp_congestion_t *congestion, uint64_t now_us, uint64_t inflight_bytes) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_begin_ack != NULL) {
        congestion->ops->on_begin_ack(congestion->state, now_us, inflight_bytes);
    }
}

void utp_congestion_on_packet_sent(utp_congestion_t *congestion, utp_congestion_packet_info_t *packet,
                                   uint64_t inflight_bytes, int app_limited) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_packet_sent != NULL) {
        congestion->ops->on_packet_sent(congestion->state, packet, inflight_bytes, app_limited);
    }
}

void utp_congestion_on_ack(utp_congestion_t *congestion, utp_congestion_packet_info_t *packet, uint64_t now_us,
                           int app_limited) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_ack != NULL) {
        congestion->ops->on_ack(congestion->state, packet, now_us, app_limited);
    }
}

void utp_congestion_on_lost(utp_congestion_t *congestion, utp_congestion_packet_info_t *packet) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_lost != NULL) {
        congestion->ops->on_lost(congestion->state, packet);
    }
}

void utp_congestion_was_quiet(utp_congestion_t *congestion, uint64_t now_us, uint64_t inflight_bytes) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->was_quiet != NULL) {
        congestion->ops->was_quiet(congestion->state, now_us, inflight_bytes);
    }
}

void utp_congestion_on_end_ack(utp_congestion_t *congestion, uint64_t inflight_bytes) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_end_ack != NULL) {
        congestion->ops->on_end_ack(congestion->state, inflight_bytes);
    }
}

void utp_congestion_on_loss(utp_congestion_t *congestion) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_loss != NULL) {
        congestion->ops->on_loss(congestion->state);
    }
}

void utp_congestion_on_timeout(utp_congestion_t *congestion) {
    if (utp_congestion_has_ops(congestion) && congestion->ops->on_timeout != NULL) {
        congestion->ops->on_timeout(congestion->state);
    }
}
