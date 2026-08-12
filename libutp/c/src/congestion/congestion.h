#ifndef EULAR_UTP_CONGESTION_CONGESTION_H
#define EULAR_UTP_CONGESTION_CONGESTION_H

#include <stdint.h>

#include "util/rtt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_congestion_packet_info {
    uint64_t packet_number;  // 发送包号
    uint64_t sent_time_us;   // 实际发送时刻，单位 us
    uint32_t packet_size;    // 线上包大小，单位 bytes
    void*    state;          // 算法私有的每包状态，不拥有
} utp_congestion_packet_info_t;

typedef struct utp_congestion_ops {
    void (*on_init)(void* state, const utp_rtt_stats_t* rtt_stats);
    uint64_t (*get_cwnd)(void* state);
    uint64_t (*get_pacing_rate)(void* state, int32_t in_recovery);
    void (*on_begin_ack)(void* state, uint64_t now_us, uint64_t inflight_bytes);
    void (*on_packet_sent)(void* state, utp_congestion_packet_info_t* packet, uint64_t inflight_bytes,
                           int32_t app_limited);
    void (*on_ack)(void* state, utp_congestion_packet_info_t* packet, uint64_t now_us, int32_t app_limited);
    void (*on_lost)(void* state, utp_congestion_packet_info_t* packet);
    void (*was_quiet)(void* state, uint64_t now_us, uint64_t inflight_bytes);
    void (*on_end_ack)(void* state, uint64_t inflight_bytes);
    void (*on_loss)(void* state);
    void (*on_timeout)(void* state);
} utp_congestion_ops_t;

typedef struct utp_congestion {
    void*                       state;  // 具体拥塞控制算法状态，不拥有
    const utp_congestion_ops_t* ops;    // 算法操作表，不拥有
} utp_congestion_t;

uint64_t utp_congestion_get_cwnd(const utp_congestion_t* congestion);
uint64_t utp_congestion_get_pacing_rate(const utp_congestion_t* congestion, int32_t in_recovery);
void     utp_congestion_init(utp_congestion_t* congestion, const utp_rtt_stats_t* rtt_stats);
void     utp_congestion_on_begin_ack(utp_congestion_t* congestion, uint64_t now_us, uint64_t inflight_bytes);
void     utp_congestion_on_packet_sent(utp_congestion_t* congestion, utp_congestion_packet_info_t* packet,
                                       uint64_t inflight_bytes, int32_t app_limited);
void     utp_congestion_on_ack(utp_congestion_t* congestion, utp_congestion_packet_info_t* packet, uint64_t now_us,
                               int32_t app_limited);
void     utp_congestion_on_lost(utp_congestion_t* congestion, utp_congestion_packet_info_t* packet);
void     utp_congestion_was_quiet(utp_congestion_t* congestion, uint64_t now_us, uint64_t inflight_bytes);
void     utp_congestion_on_end_ack(utp_congestion_t* congestion, uint64_t inflight_bytes);
void     utp_congestion_on_loss(utp_congestion_t* congestion);
void     utp_congestion_on_timeout(utp_congestion_t* congestion);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_CONGESTION_H
