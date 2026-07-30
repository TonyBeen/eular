#ifndef EULAR_UTP_CONGESTION_BBR_H
#define EULAR_UTP_CONGESTION_BBR_H

#include "congestion/bw_sampler.h"
#include "congestion/congestion.h"
#include "congestion/minmax.h"

typedef enum utp_bbr_mode { UTP_BBR_STARTUP, UTP_BBR_DRAIN, UTP_BBR_PROBE_BW, UTP_BBR_PROBE_RTT } utp_bbr_mode_t;

typedef enum utp_bbr_recovery_state {
    UTP_BBR_NOT_IN_RECOVERY,
    UTP_BBR_CONSERVATION,
    UTP_BBR_GROWTH
} utp_bbr_recovery_state_t;

typedef struct utp_bbr_config {
    uint32_t initial_cwnd_mss;
    uint32_t minimum_cwnd_mss;
} utp_bbr_config_t;

typedef struct utp_bbr {
    utp_congestion_t         congestion;
    const utp_rtt_stats_t   *rtt_stats;
    utp_bw_sampler_t         sampler;
    utp_minmax_t             max_bandwidth;
    utp_minmax_t             max_ack_height;
    uint64_t                 cwnd;
    uint64_t                 initial_cwnd;
    uint64_t                 minimum_cwnd;
    uint64_t                 pacing_rate;
    uint64_t                 acked_bytes;
    uint64_t                 lost_bytes;
    uint64_t                 ack_time_us;
    uint64_t                 inflight_bytes;
    uint64_t                 max_cwnd;
    uint64_t                 recovery_window;
    uint64_t                 aggregation_epoch_start_us;
    uint64_t                 aggregation_epoch_bytes;
    uint64_t                 last_sent_packet_number;
    uint64_t                 current_round_end_packet_number;
    uint64_t                 end_recovery_packet_number;
    uint64_t                 round_count;
    uint64_t                 bandwidth_at_last_round;
    uint64_t                 min_rtt_us;
    uint64_t                 min_rtt_timestamp_us;
    uint64_t                 min_rtt_since_last_probe_us;
    uint64_t                 last_cycle_start_us;
    uint64_t                 probe_rtt_exit_us;
    uint32_t                 cycle_index;
    uint32_t                 rounds_without_bandwidth_gain;
    uint32_t                 startup_round_limit;
    double                   pacing_gain;
    double                   cwnd_gain;
    double                   high_gain;
    double                   high_cwnd_gain;
    double                   drain_gain;
    bool                     in_ack;
    bool                     last_sample_app_limited;
    bool                     has_non_app_limited_sample;
    bool                     full_bandwidth_reached;
    bool                     probe_rtt_round_passed;
    bool                     ack_has_losses;
    bool                     ack_has_sample;
    bool                     app_limited_since_last_probe;
    utp_bbr_recovery_state_t recovery_state;
    utp_bbr_mode_t           mode;
} utp_bbr_t;

void              utp_bbr_init(utp_bbr_t *bbr, const utp_bbr_config_t *config);
utp_congestion_t *utp_bbr_as_congestion(utp_bbr_t *bbr);

#endif  // EULAR_UTP_CONGESTION_BBR_H
