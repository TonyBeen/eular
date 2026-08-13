#ifndef EULAR_UTP_CONGESTION_PACER_H
#define EULAR_UTP_CONGESTION_PACER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_PACER_BURST_TOKENS 10u

typedef struct utp_pacer {
    uint64_t next_scheduled_time_us;     // 下一包最早可调度时刻
    uint64_t last_delayed_time_us;       // 最近一次被节流的时刻
    uint64_t now_us;                     // 当前 pacer tick 时间
    uint32_t clock_granularity_us;       // 调度时钟粒度
    uint32_t burst_tokens;               // 剩余突发发送额度
    uint32_t scheduled_count;            // 当前 tick 已调度包数
    bool     last_schedule_delayed : 1;  // 最近一次调度是否被延后
    bool     delayed_on_tick_in : 1;     // 本 tick 开始时是否仍处于延后状态
} utp_pacer_t;

void     utp_pacer_init(utp_pacer_t* pacer, uint32_t clock_granularity_us);
void     utp_pacer_tick_in(utp_pacer_t* pacer, uint64_t now_us);
void     utp_pacer_tick_out(utp_pacer_t* pacer);
bool     utp_pacer_can_schedule(utp_pacer_t* pacer, uint64_t inflight_packet_count);
void     utp_pacer_packet_scheduled(utp_pacer_t* pacer, uint64_t inflight_packet_count, bool in_recovery,
                                    uint64_t transmit_interval_us);
void     utp_pacer_on_loss(utp_pacer_t* pacer);
bool     utp_pacer_delayed(const utp_pacer_t* pacer);
uint64_t utp_pacer_next_scheduled_time(const utp_pacer_t* pacer);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_PACER_H
