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
} utp_context_options_t;

#endif  // EULAR_UTP_C_OPTION_H
