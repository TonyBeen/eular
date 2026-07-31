#include "congestion/pacer.h"

#include <stddef.h>

void utp_pacer_init(utp_pacer_t *pacer, uint32_t clock_granularity_us) {
    if (pacer != NULL) {
        pacer->next_scheduled_time_us = 0u;
        pacer->last_delayed_time_us   = 0u;
        pacer->now_us                 = 0u;
        pacer->clock_granularity_us   = clock_granularity_us;
        pacer->burst_tokens           = UTP_PACER_BURST_TOKENS;
        pacer->scheduled_count        = 0u;
        pacer->last_schedule_delayed  = false;
        pacer->delayed_on_tick_in     = false;
    }
}

void utp_pacer_tick_in(utp_pacer_t *pacer, uint64_t now_us) {
    if (pacer == NULL) {
        return;
    }
    if (now_us > pacer->now_us) {
        pacer->now_us = now_us;
    }
    if (pacer->last_schedule_delayed) {
        pacer->delayed_on_tick_in = true;
    }
    pacer->scheduled_count = 0u;
}

void utp_pacer_tick_out(utp_pacer_t *pacer) {
    if (pacer == NULL) {
        return;
    }
    if (pacer->delayed_on_tick_in && pacer->scheduled_count == 0u && pacer->now_us > pacer->next_scheduled_time_us) {
        pacer->last_schedule_delayed = false;
    }
    pacer->delayed_on_tick_in = false;
}

bool utp_pacer_can_schedule(utp_pacer_t *pacer, uint64_t inflight_packet_count) {
    if (pacer == NULL) {
        return false;
    }
    if (pacer->burst_tokens != 0u || inflight_packet_count == 0u) {
        return true;
    }
    if (pacer->next_scheduled_time_us > pacer->now_us + pacer->clock_granularity_us) {
        pacer->last_schedule_delayed = true;
        return false;
    }
    return true;
}

void utp_pacer_packet_scheduled(utp_pacer_t *pacer, uint64_t inflight_packet_count, bool in_recovery,
                                uint64_t transmit_interval_us) {
    uint64_t next_time;

    if (pacer == NULL) {
        return;
    }
    ++pacer->scheduled_count;
    if (inflight_packet_count == 0u && !in_recovery) {
        pacer->burst_tokens = UTP_PACER_BURST_TOKENS;
    }
    if (pacer->burst_tokens != 0u) {
        --pacer->burst_tokens;
        pacer->last_schedule_delayed  = false;
        pacer->next_scheduled_time_us = 0u;
        pacer->last_delayed_time_us   = 0u;
        return;
    }
    if (transmit_interval_us == 0u) {
        transmit_interval_us = 1u;
    }
    if (pacer->last_schedule_delayed) {
        next_time                     = pacer->next_scheduled_time_us + transmit_interval_us;
        pacer->next_scheduled_time_us = next_time < pacer->next_scheduled_time_us ? UINT64_MAX : next_time;
        if (pacer->next_scheduled_time_us <= pacer->now_us ||
            (pacer->last_delayed_time_us != 0u &&
             pacer->last_delayed_time_us + transmit_interval_us <= pacer->now_us)) {
            pacer->last_delayed_time_us = pacer->now_us;
        } else {
            pacer->last_schedule_delayed = false;
            pacer->last_delayed_time_us  = 0u;
        }
        return;
    }
    next_time = pacer->now_us + transmit_interval_us;
    if (next_time < pacer->now_us || pacer->next_scheduled_time_us > UINT64_MAX - transmit_interval_us) {
        pacer->next_scheduled_time_us = UINT64_MAX;
    } else if (pacer->next_scheduled_time_us + transmit_interval_us > next_time) {
        pacer->next_scheduled_time_us += transmit_interval_us;
    } else {
        pacer->next_scheduled_time_us = next_time;
    }
}

void utp_pacer_on_loss(utp_pacer_t *pacer) {
    if (pacer != NULL) {
        pacer->burst_tokens = 0u;
    }
}

bool utp_pacer_delayed(const utp_pacer_t *pacer) { return pacer != NULL && pacer->last_schedule_delayed; }

uint64_t utp_pacer_next_scheduled_time(const utp_pacer_t *pacer) {
    return pacer == NULL ? 0u : pacer->next_scheduled_time_us;
}
