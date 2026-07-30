#ifndef EULAR_UTP_CONGESTION_BW_SAMPLER_H
#define EULAR_UTP_CONGESTION_BW_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_bw_packet_state {
    uint64_t total_bytes_sent;
    uint64_t total_bytes_acked;
    uint64_t total_bytes_lost;
    uint64_t sent_at_last_ack;
    uint64_t last_ack_sent_time_us;
    uint64_t last_ack_time_us;
    uint64_t packet_number;
    uint64_t sent_time_us;
    uint32_t packet_size;
    bool     app_limited;
    bool     valid;
} utp_bw_packet_state_t;

typedef struct utp_bw_sample {
    uint64_t bandwidth_bytes_per_second;
    uint64_t rtt_us;
    bool     is_app_limited;
} utp_bw_sample_t;

typedef struct utp_bw_sampler {
    uint64_t total_bytes_sent;
    uint64_t total_bytes_acked;
    uint64_t total_bytes_lost;
    uint64_t last_acked_total_sent;
    uint64_t last_acked_sent_time_us;
    uint64_t last_acked_time_us;
    uint64_t last_sent_packet_number;
    uint64_t app_limited_until_packet_number;
    bool     app_limited;
} utp_bw_sampler_t;

void utp_bw_sampler_init(utp_bw_sampler_t *sampler);
void utp_bw_sampler_on_packet_sent(utp_bw_sampler_t *sampler, utp_bw_packet_state_t *packet, uint64_t packet_number,
                                   uint32_t packet_size, uint64_t sent_time_us);
bool utp_bw_sampler_on_packet_acked(utp_bw_sampler_t *sampler, utp_bw_packet_state_t *packet, uint64_t packet_number,
                                    uint64_t ack_time_us, utp_bw_sample_t *sample);
void utp_bw_sampler_on_packet_lost(utp_bw_sampler_t *sampler, utp_bw_packet_state_t *packet);
void utp_bw_sampler_set_app_limited(utp_bw_sampler_t *sampler);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_BW_SAMPLER_H
