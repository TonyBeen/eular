/*************************************************************************
    > File Name: config.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:59:29 PM CST
 ************************************************************************/

#ifndef __UTP_CONFIG_H__
#define __UTP_CONFIG_H__

#include <stdint.h>
#include <string>

#include <utp/platform.h>

namespace eular {
namespace utp {

enum PathMigrationMode : uint8_t {
    kPathMigrationConservative = 0, // 标准策略：验证完成前业务走旧路径
    kPathMigrationAggressive = 1,   // 激进策略：可在新路径提前发数据
};

enum StreamSchedulerMode : uint8_t {
    kStreamSchedulerDisabled = 0,
    kStreamSchedulerStrict = 1,
    kStreamSchedulerDrr = 2,
};

class Config {
public:
    // dplpmtud
    bool        enable_dplpmtud = true;     // 开启dplpmtud
    uint16_t    mtu_min = 1280;             // MTU下探的最小值
    uint16_t    mtu_max = 1500;             // MTU上探的最大值
    uint16_t    mtu_base = 1400;            // 默认mtu
    uint32_t    mtu_probe_interval = 300;   // 探测间隔时间(seconds), 5min
    uint16_t    mtu_probe_step = 16;        // mtu探测步长(增大此值可加快探测速度但会降低精度)
    uint16_t    mtu_probe_timeout = 2000;   // mtu探测超时时间(ms)
    uint8_t     mtu_blackhole_loss_threshold = 3; // 黑洞判定：连续大包丢失阈值(次数)
    uint16_t    mtu_blackhole_loss_window_ms = 3000; // 黑洞判定：连续丢失统计窗口(ms)
    uint16_t    mtu_blackhole_cooldown_ms = 5000; // 黑洞回退后冷静期(ms)

    // keepalive
    bool        enable_keepalive = true;    // 开启keepalive
    uint32_t    keepalive_interval = 0;     // 本地保活探测间隔(ms). 0表示使用 max_idle_timeout 作为本地基准；实际发送间隔会与对端TP(max_idle_timeout-RTT裕量)取较小值
    uint32_t    keepalive_timeout = 1500;   // keepalive超时时间(ms)
    uint16_t    keepalive_probes = 3;       // keepalive探测次数
    uint32_t    max_idle_timeout = 30000;   // 本地默认空闲保活阈值(ms), 仅当 keepalive_interval=0 时作为本地基准

    // token / 0-rtt
    uint32_t    zero_rtt_token_max_lifetime = 600; // 0-RTT token 最长时效(s)
    uint32_t    zero_rtt_replay_window = 10;       // 0-RTT 抗重放去重窗口(s)

    // path migration
    PathMigrationMode path_migration_mode = kPathMigrationConservative;

    // socket
    int32_t     recv_buf_size = 1024 * 1024;
    int32_t     send_buf_size = 1024 * 1024;

    // congestion control
    int32_t     cc_algorithm = 0;           // 拥塞控制算法, 0表示默认算法(bbr), 1表示BBR算法, 2表示Cubic算法
    uint32_t    clock_granularity_us = 1;   // pacer时钟粒度(us), 影响RTT的测量精度和拥塞控制的性能
    uint32_t    bbr_init_cwnd_mss = 32;     // BBR 初始拥塞窗口，单位 MSS
    uint32_t    bbr_min_cwnd_mss = 4;       // BBR 最小拥塞窗口，单位 MSS
    float       bbr_startup_high_gain = 2.885f; // BBR STARTUP 高增益
    float       bbr_cwnd_gain = 2.0f;       // BBR PROBE_BW 阶段 cwnd 增益
    float       bbr_startup_growth_target = 1.25f; // BBR 判断带宽持续增长的倍率阈值
    uint32_t    bbr_startup_full_bw_rounds = 3; // BBR STARTUP 无增长后退出轮数
    uint32_t    bbr_probe_rtt_ms = 200;     // BBR PROBE_RTT 最短驻留时长(ms)
    uint32_t    bbr_min_rtt_expiry_ms = 10000; // BBR min_rtt 过期时间(ms)
    double      cubic_beta = 0.7;           // CUBIC 丢包回退系数 beta，建议范围 (0, 1)
    double      cubic_c = 0.4;              // CUBIC 曲线常数 C，建议范围 (0, 2]
    uint32_t    cubic_init_cwnd_mss = 32;   // CUBIC 初始拥塞窗口，单位 MSS
    uint32_t    cubic_min_cwnd_mss = 4;     // CUBIC 最小拥塞窗口，单位 MSS

    // ack
    uint8_t     ack_every_n_packets = 10;  // 连续收到多少个 ack-eliciting 包后立即回 Ack
    uint32_t    time_threshold_ms = 3;      // 时间阈值 = 3 * rtt
    uint8_t     max_ack_range_size = 149;   // 一个Ack帧可容纳的AckRange的数量
    uint8_t     ack_delay_exponent = 3;     // ack延迟指数，ack延迟时间 = FrameAck::ack_delay << ack_dely_exponent us
    uint16_t    ack_delay = 150;            // 最大ack延迟时间(ms)

    // tp
    uint16_t    handshake_timeout = 3000;   // 等待 HandshakeDown 超时时间(ms)
    uint16_t    pending_handshake_retry_interval_ms = 200; // pending 阶段重发 Handshake 周期(ms)
    uint8_t     pending_handshake_max_retries = 6; // pending 阶段最多重发次数(0 表示不重发)
    uint16_t    pending_pre_handshake_buffer_packets = 8; // pending 阶段缓存未携带 HandshakeDone 包数量上限
    uint32_t    pending_pre_handshake_buffer_bytes = 32 * 1024; // pending 阶段缓存未携带 HandshakeDone 包字节上限
    uint16_t    init_max_streams_bidi = 64; // 初始双向流数量
    uint16_t    init_max_streams_uni = 32;  // 初始单向流数量

    // stream scheduler
    uint8_t     stream_default_priority = 4;           // 默认 stream 优先级 (0 最高, 7 最低)
    StreamSchedulerMode stream_scheduler_mode = kStreamSchedulerStrict; // 调度模式，默认 Strict
    uint16_t    stream_aging_threshold = 8;            // Strict+Aging：等待多少轮触发一次提升
    uint8_t     stream_aging_step = 1;                 // Strict+Aging：每次触发提升档位数
    uint16_t    stream_drr_quantum = 1200;             // DRR 基准量子 (bytes)
    uint32_t    stream_drr_deficit_cap = 64 * 1024;    // DRR deficit 上限 (bytes)
    uint32_t    stream_send_buffer_limit = 64 * 1024;  // 单个 stream 写缓冲区上限(bytes)
    bool        stream_enable_coalescing = true;       // 是否启用 tiny-write 聚合
    uint16_t    stream_min_payload_before_immediate_send = 1200; // 小于该阈值时可进入聚合等待(bytes)
    uint32_t    stream_coalesce_delay_us = 1000;       // tiny-write 聚合等待窗口(us)
    uint32_t    stream_unacked_data_limit = 256 * 1024; // 在途未确认 STREAM 数据上限(bytes, 含首次发送与重传)
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONFIG_H__
