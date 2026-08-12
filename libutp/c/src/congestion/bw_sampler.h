#ifndef EULAR_UTP_CONGESTION_BW_SAMPLER_H
#define EULAR_UTP_CONGESTION_BW_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_bw_packet_state {
    uint64_t total_bytes_sent;       // 发送此包时的累计发送字节
    uint64_t total_bytes_acked;      // 发送此包时的累计确认字节
    uint64_t total_bytes_lost;       // 发送此包时的累计丢失字节
    uint64_t sent_at_last_ack;       // 最近 ACK 对应的累计发送字节
    uint64_t last_ack_sent_time_us;  // 最近 ACK 所确认包的发送时刻
    uint64_t last_ack_time_us;       // 最近 ACK 到达时刻
    uint64_t packet_number;          // 此包包号
    uint64_t sent_time_us;           // 此包发送时刻
    uint32_t packet_size;            // 此包长度
    bool     app_limited;            // 发送时是否受应用数据限制
    bool     valid;                  // 是否已写入有效采样快照
} utp_bw_packet_state_t;

typedef struct utp_bw_sample {
    uint64_t bandwidth_bytes_per_second;  // 采样带宽
    uint64_t rtt_us;                      // 采样 RTT
    bool     is_app_limited;              // 此采样是否受应用限制
} utp_bw_sample_t;

typedef struct utp_bw_sampler {
    uint64_t total_bytes_sent;                 // 累计发送字节
    uint64_t total_bytes_acked;                // 累计确认字节
    uint64_t total_bytes_lost;                 // 累计丢失字节
    uint64_t last_acked_total_sent;            // 最近 ACK 时的累计发送字节
    uint64_t last_acked_sent_time_us;          // 最近 ACK 所确认包发送时刻
    uint64_t last_acked_time_us;               // 最近 ACK 到达时刻
    uint64_t last_sent_packet_number;          // 最近发送的包号
    uint64_t app_limited_until_packet_number;  // 应用受限标记的有效包号上界
    bool     app_limited;                      // 当前是否处于应用受限阶段
} utp_bw_sampler_t;

void utp_bw_sampler_init(utp_bw_sampler_t* sampler);
void utp_bw_sampler_on_packet_sent(utp_bw_sampler_t* sampler, utp_bw_packet_state_t* packet, uint64_t packet_number,
                                   uint32_t packet_size, uint64_t sent_time_us);
bool utp_bw_sampler_on_packet_acked(utp_bw_sampler_t* sampler, utp_bw_packet_state_t* packet, uint64_t packet_number,
                                    uint64_t ack_time_us, utp_bw_sample_t* sample);
void utp_bw_sampler_on_packet_lost(utp_bw_sampler_t* sampler, utp_bw_packet_state_t* packet);
void utp_bw_sampler_set_app_limited(utp_bw_sampler_t* sampler);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_BW_SAMPLER_H
