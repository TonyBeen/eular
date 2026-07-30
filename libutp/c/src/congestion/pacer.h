#ifndef EULAR_UTP_CONGESTION_PACER_H
#define EULAR_UTP_CONGESTION_PACER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_PACER_BURST_TOKENS 10u

typedef struct utp_pacer {
    uint64_t next_scheduled_time_us;
    uint64_t last_delayed_time_us;
    uint64_t now_us;
    uint32_t clock_granularity_us;
    uint32_t burst_tokens;
    uint32_t scheduled_count;
    bool     last_schedule_delayed;
    bool     delayed_on_tick_in;
} utp_pacer_t;

void     utp_pacer_init(utp_pacer_t *pacer, uint32_t clock_granularity_us);
void     utp_pacer_tick_in(utp_pacer_t *pacer, uint64_t now_us);
void     utp_pacer_tick_out(utp_pacer_t *pacer);
bool     utp_pacer_can_schedule(utp_pacer_t *pacer, uint64_t inflight_packet_count);
void     utp_pacer_packet_scheduled(utp_pacer_t *pacer, uint64_t inflight_packet_count, bool in_recovery,
                                    uint64_t transmit_interval_us);
void     utp_pacer_on_loss(utp_pacer_t *pacer);
bool     utp_pacer_delayed(const utp_pacer_t *pacer);
uint64_t utp_pacer_next_scheduled_time(const utp_pacer_t *pacer);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_PACER_H
