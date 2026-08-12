#ifndef EULAR_UTP_C_OPTION_H
#define EULAR_UTP_C_OPTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/log.h>

#define UTP_BBR_PACING_GAIN_COUNT 8u

typedef enum utp_stream_scheduler_mode {
    UTP_STREAM_SCHEDULER_STRICT,
    UTP_STREAM_SCHEDULER_DRR,
} utp_stream_scheduler_mode_t;

// Context 内所有连接共用的拥塞控制算法选择。
typedef enum utp_congestion_algorithm {
    UTP_CONGESTION_DEFAULT = 0,
    UTP_CONGESTION_BBR     = 1,
    UTP_CONGESTION_CUBIC   = 2,
} utp_congestion_algorithm_t;

typedef struct utp_context_options {
    struct event_base*          event_base;                  // 由调用方创建并驱动, Context 仅借用该事件循环
    utp_log_sink_fn             log_sink;                    // 为空时关闭日志, 回调由协议处理线程同步调用
    uint64_t                    context_id;                  // 用于日志标签和 CID 初始值, 0 会自动跳过无效 CID
    utp_log_level_t             log_level;                   // 最低输出级别, 低于该级别的日志会被过滤
    utp_stream_scheduler_mode_t stream_scheduler_mode;       // 新建连接采用的流发送调度策略
    utp_congestion_algorithm_t  cc_algorithm;                // 新建连接采用的拥塞控制算法
    uint32_t                    clock_granularity_us;        // pacer 时钟粒度(us)，0 时采用 1
    uint32_t                    bbr_init_cwnd_mss;           // BBR 初始拥塞窗口(MSS)，0 时采用 16
    uint32_t                    bbr_min_cwnd_mss;            // BBR 最小拥塞窗口(MSS)，0 时采用 4
    double                      bbr_startup_high_gain;       // BBR STARTUP pacing/cwnd 增益
    double                      bbr_cwnd_gain;               // BBR PROBE_BW cwnd 增益
    double                      bbr_startup_growth_target;   // BBR STARTUP 带宽增长阈值
    uint32_t                    bbr_startup_full_bw_rounds;  // BBR 退出 STARTUP 所需轮数
    uint32_t                    bbr_probe_rtt_ms;            // BBR PROBE_RTT 最短持续时间(ms)，最小 50
    uint32_t                    bbr_min_rtt_expiry_ms;       // BBR min_rtt 过期时间(ms)，最小 1000
    double                      bbr_pacing_gains[UTP_BBR_PACING_GAIN_COUNT];  // BBR PROBE_BW 的 8 项 pacing 增益周期
    double                      cubic_beta;                                   // CUBIC 丢包回退系数，非法值时采用 0.7
    double                      cubic_c;                                      // CUBIC 曲线常数，非法值时采用 0.4
    uint32_t                    cubic_init_cwnd_mss;                          // CUBIC 初始拥塞窗口(MSS)，0 时采用 32
    uint32_t                    cubic_min_cwnd_mss;                           // CUBIC 最小拥塞窗口(MSS)，0 时采用 4

    // MTU 阶梯探测与黑洞检测配置
    bool                        enable_dplpmtud;               // 是否启用 DPLPMTUD
    uint16_t                    mtu_min;                       // 可服务的最小 MTU，默认 1280
    uint16_t                    mtu_max;                       // 探测的最大 MTU 上限，默认 1500
    uint16_t                    mtu_base;                      // 初始安全 MTU，默认 1400
    uint32_t                    mtu_probe_interval;            // MTU 探测间隔(秒)
    uint16_t                    mtu_probe_step;                // 阶梯探测步长(bytes)
    uint16_t                    mtu_probe_timeout;             // 单次探测超时(ms)
    uint8_t                     mtu_probe_retries;             // 单次探测额外重试次数
    uint8_t                     mtu_blackhole_loss_threshold;  // 判定 MTU 黑洞的连续大包丢失阈值
    uint16_t                    mtu_blackhole_loss_window_ms;  // 黑洞丢失统计时间窗口(ms)
    uint16_t                    mtu_blackhole_cooldown_ms;     // 黑洞降级后的冷却时间(ms)

    // 0-RTT 会话票据与抗重放配置
    uint32_t                    zero_rtt_token_max_lifetime_seconds;  // 会话票据最大有效期(秒)
    uint32_t                    zero_rtt_replay_cache_capacity;       // Context 级抗重放缓存容量，0 时采用默认值
    uint32_t                    stream_terminal_capacity;             // 每连接流终态记录容量，0 时采用 4096

    // 被动握手响应的超时与重试配置
    uint16_t                    handshake_timeout;      // 首轮握手超时(ms)，0 时采用 800
    uint8_t                     handshake_max_retries;  // 被动握手响应的最大重试次数

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

// Context 配置的完整默认值；创建 Context 前必须由调用方设置 event_base。
#define UTP_CONTEXT_OPTIONS_INIT                                                                                  \
    {                                                                                                             \
        NULL,                                       /* event_base: 调用方提供的事件循环 */                        \
        NULL,                                       /* log_sink: 默认关闭日志 */                                  \
        0u,                                         /* context_id: 自动生成 CID 起点 */                           \
        UTP_LOG_LEVEL_INFO,                         /* log_level: 最低日志级别 */                                 \
        UTP_STREAM_SCHEDULER_STRICT,                /* stream_scheduler_mode: Strict 调度 */                      \
        UTP_CONGESTION_DEFAULT,                     /* cc_algorithm: 默认 BBR */                                  \
        1u,                                         /* clock_granularity_us: pacer 时钟粒度 */                    \
        16u,                                        /* bbr_init_cwnd_mss: BBR 初始 cwnd */                        \
        4u,                                         /* bbr_min_cwnd_mss: BBR 最小 cwnd */                         \
        2.885,                                      /* bbr_startup_high_gain: BBR STARTUP 增益 */                 \
        2.0,                                        /* bbr_cwnd_gain: BBR PROBE_BW cwnd 增益 */                   \
        1.25,                                       /* bbr_startup_growth_target: BBR 带宽增长阈值 */             \
        3u,                                         /* bbr_startup_full_bw_rounds: BBR 退出 STARTUP 轮数 */       \
        200u,                                       /* bbr_probe_rtt_ms: BBR PROBE_RTT 最短持续时间 */            \
        10000u,                                     /* bbr_min_rtt_expiry_ms: BBR min_rtt 过期时间 */             \
        {1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}, /* bbr_pacing_gains: BBR PROBE_BW 增益周期 */                 \
        0.7,                                        /* cubic_beta: CUBIC 丢包回退系数 */                          \
        0.4,                                        /* cubic_c: CUBIC 曲线常数 */                                 \
        32u,                                        /* cubic_init_cwnd_mss: CUBIC 初始 cwnd */                    \
        4u,                                         /* cubic_min_cwnd_mss: CUBIC 最小 cwnd */                     \
        true,                                       /* enable_dplpmtud: 启用 MTU 探测 */                          \
        1280u,                                      /* mtu_min: MTU 下限 */                                       \
        1500u,                                      /* mtu_max: MTU 上限 */                                       \
        1400u,                                      /* mtu_base: 初始安全 MTU */                                  \
        300u,                                       /* mtu_probe_interval: 探测间隔(秒) */                        \
        16u,                                        /* mtu_probe_step: 探测步长 */                                \
        2000u,                                      /* mtu_probe_timeout: 单次探测超时(ms) */                     \
        1u,                                         /* mtu_probe_retries: 单次探测重试次数 */                     \
        3u,                                         /* mtu_blackhole_loss_threshold: 黑洞丢包阈值 */              \
        3000u,                                      /* mtu_blackhole_loss_window_ms: 黑洞判定窗口 */              \
        5000u,                                      /* mtu_blackhole_cooldown_ms: 黑洞冷却期 */                   \
        600u,                                       /* zero_rtt_token_max_lifetime_seconds: 票据最长有效期 */     \
        4096u,                                      /* zero_rtt_replay_cache_capacity: 抗重放缓存容量 */          \
        4096u,                                      /* stream_terminal_capacity: 每连接流终态容量 */              \
        800u,                                       /* handshake_timeout: 握手首轮超时(ms) */                     \
        2u,                                         /* handshake_max_retries: 握手重试次数 */                     \
        true,                                       /* enable_keepalive: 启用保活 */                              \
        0u,                                         /* keepalive_interval: 0 表示自动推算 */                      \
        1500u,                                      /* keepalive_timeout: 单次保活超时(ms) */                     \
        3u,                                         /* keepalive_probes: 最大连续探测次数 */                      \
        30000u,                                     /* max_idle_timeout: 最大空闲超时(ms) */                      \
        4u,                                         /* ack_every_n_packets: ACK 包计数阈值 */                     \
        3u,                                         /* ack_delay_exponent: ACK 延迟编码指数 */                    \
        25u,                                        /* ack_delay: 最大 ACK 延迟(ms) */                            \
        32u,                                        /* initial_max_streams_bidi: 初始最大双向流数 */              \
        16u,                                        /* initial_max_streams_uni: 初始最大单向流数 */               \
        UINT64_C(8) * 1024u * 1024u,                /* initial_max_data: 初始连接级接收窗口 */                    \
        UINT64_C(256) * 1024u,                      /* initial_max_stream_data_bidi_local: 对端双向流接收窗口 */  \
        UINT64_C(256) * 1024u                       /* initial_max_stream_data_bidi_remote: 本端双向流接收窗口 */ \
    }

#endif  // EULAR_UTP_C_OPTION_H
