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
    struct event_base*          event_base;             // 由调用方创建并驱动, Context 仅借用该事件循环
    utp_log_sink_fn             log_sink;               // 为空时关闭日志, 回调由协议处理线程同步调用
    uint64_t                    context_id;             // 用于日志标签和 CID 初始值, 0 会自动跳过无效 CID
    utp_log_level_t             log_level;              // 最低输出级别, 低于该级别的日志会被过滤
    utp_stream_scheduler_mode_t stream_scheduler_mode;  // 新建连接采用的流发送调度策略

    // MTU 阶梯探测与黑洞检测配置
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

    // 0-RTT 会话票据与抗重放配置
    uint32_t                    zero_rtt_token_max_lifetime_seconds;
    uint32_t                    zero_rtt_replay_cache_capacity;

    // 被动握手响应的超时与重试配置
    uint16_t                    handshake_timeout;
    uint8_t                     handshake_max_retries;

    // 保活与 ACK 调度配置
    bool                        enable_keepalive;     // 是否启用空闲 Ping 探测
    uint32_t                    keepalive_interval;   // 探测间隔(ms)，0 时采用 max_idle_timeout
    uint32_t                    keepalive_timeout;    // 单次探测等待时间(ms)，0 时采用生效间隔
    uint16_t                    keepalive_probes;     // 连续未响应探测次数，0 按 1 次处理
    uint32_t                    max_idle_timeout;     // 本端通告的最大空闲时间(ms)，0 时采用 30000
    uint8_t                     ack_every_n_packets;  // ACK-eliciting 包计数阈值，0 时采用 4
    uint8_t                     ack_delay_exponent;   // ACK 延迟编码指数，范围 0..20
    uint16_t                    ack_delay;            // 最大 ACK 延迟(ms)，0 时采用 25

    // 本端在握手中通告的初始流控与流数量
    uint16_t                    initial_max_streams_bidi;             // 允许对端创建的双向流数，0 时采用 32
    uint16_t                    initial_max_streams_uni;              // 允许对端创建的单向流数，0 时采用 16
    uint64_t                    initial_max_data;                     // 连接级接收窗口，0 时采用 8 MiB
    uint64_t                    initial_max_stream_data_bidi_local;   // 对端发起双向流的接收窗口，0 时采用 256 KiB
    uint64_t                    initial_max_stream_data_bidi_remote;  // 本端发起双向流的接收窗口，0 时采用 256 KiB
} utp_context_options_t;

#endif  // EULAR_UTP_C_OPTION_H
