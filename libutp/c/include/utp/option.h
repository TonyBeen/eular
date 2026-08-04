#ifndef EULAR_UTP_C_OPTION_H
#define EULAR_UTP_C_OPTION_H

#include <stdbool.h>
#include <stdint.h>

#include <utp/log.h>

typedef enum utp_stream_scheduler_mode {
    UTP_STREAM_SCHEDULER_STRICT,
    UTP_STREAM_SCHEDULER_DRR,
} utp_stream_scheduler_mode_t;

typedef struct utp_context_options {
    struct event_base*          event_base;
    utp_log_sink_fn             log_sink;
    uint64_t                    context_id;
    utp_log_level_t             log_level;
    utp_stream_scheduler_mode_t stream_scheduler_mode;

    // mtu probing and blackhole detection options
    bool                        enable_dplpmtud;
    uint16_t                    mtu_min;
    uint16_t                    mtu_max;
    uint16_t                    mtu_base;
    uint32_t                    mtu_probe_interval;
    uint16_t                    mtu_probe_step;
    uint16_t                    mtu_probe_timeout;
    uint8_t                     mtu_probe_retries;
    uint8_t                     mtu_blackhole_loss_threshold;
    uint16_t                    mtu_blackhole_loss_window_ms;
    uint16_t                    mtu_blackhole_cooldown_ms;
} utp_context_options_t;

#endif  // EULAR_UTP_C_OPTION_H
