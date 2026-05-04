/*************************************************************************
    > File Name: config.h
    > Author: eular
    > Brief: libutp 全局配置参数，涵盖拥塞控制、MTU 探测、保活、调度器等。
    > Created Time: Mon 08 Dec 2025 04:59:29 PM CST
 ************************************************************************/

#ifndef __UTP_CONFIG_H__
#define __UTP_CONFIG_H__

#include <stdint.h>
#include <string>
#include <vector>

#include <utp/platform.h>

namespace eular {
namespace utp {

/**
 * @enum PathMigrationMode
 * @brief 路径迁移模式
 */
enum PathMigrationMode : uint8_t {
    kPathMigrationConservative = 0, ///< 标准策略：路径验证完成前，业务数据走旧路径
    kPathMigrationAggressive = 1,   ///< 激进策略：可在新路径验证完成前提前发送业务数据
};

/**
 * @enum StreamSchedulerMode
 * @brief 流调度模式
 */
enum StreamSchedulerMode : uint8_t {
    kStreamSchedulerDisabled = 0,   ///< 禁用调度，按 ID 顺序尝试
    kStreamSchedulerStrict = 1,     ///< 严格优先级调度 (0 最高, 7 最低)
    kStreamSchedulerDrr = 2,        ///< 赤字轮询 (DRR) 调度，保证公平性
};

/**
 * @enum ConnectionSchedulerMode
 * @brief 连接级调度模式（多连接 flush 场景）
 */
enum ConnectionSchedulerMode : uint8_t {
    kConnectionSchedulerDisabled = 0,   ///< 禁用连接调度
    kConnectionSchedulerStrict = 1,     ///< 连接级严格优先级
    kConnectionSchedulerWdrr = 2,       ///< 加权赤字轮询 (WDRR)
};

/**
 * @class Config
 * @brief libutp 的配置类。用户可以在创建 Context 前修改此对象以调整协议行为。
 */
class Config {
public:
    // --- DPLPMTUD (数据路径包最大传输单元探测) ---
    bool        enable_dplpmtud = true;     ///< 是否开启 DPLPMTUD
    uint16_t    mtu_min = 1280;             ///< MTU 下探的最小值 (IPv6 最小要求为 1280)
    uint16_t    mtu_max = 1500;             ///< MTU 上探的最大值 (通常为以太网的 1500)
    uint16_t    mtu_base = 1400;            ///< 初始 MTU 估值
    uint32_t    mtu_probe_interval = 300;   ///< 探测间隔时间 (秒)，默认 5 分钟
    uint16_t    mtu_probe_step = 16;        ///< MTU 探测步长
    uint16_t    mtu_probe_timeout = 2000;   ///< 单次 MTU 探测超时时间 (ms)
    uint8_t     mtu_blackhole_loss_threshold = 3; ///< 判定黑洞的连续大包丢失阈值
    uint16_t    mtu_blackhole_loss_window_ms = 3000; ///< 黑洞判定时间窗口 (ms)
    uint16_t    mtu_blackhole_cooldown_ms = 5000; ///< 黑洞判定后的冷却期 (ms)

    // --- Keepalive (保活探测) ---
    bool        enable_keepalive = true;    ///< 是否开启保活
    uint32_t    keepalive_interval = 0;     ///< 保活探测间隔 (ms)。0 表示使用 max_idle_timeout 自动推算
    uint32_t    keepalive_timeout = 1500;   ///< 保活包超时时间 (ms)
    uint16_t    keepalive_probes = 3;       ///< 保活探测最大重试次数
    uint32_t    max_idle_timeout = 30000;   ///< 最大空闲超时阈值 (ms)

    // --- Token / 0-RTT ---
    uint32_t    zero_rtt_token_max_lifetime = 600; ///< 0-RTT 票据最长时效 (秒)
    uint32_t    zero_rtt_replay_window = 10;       ///< 0-RTT 抗重放窗口时间 (秒)

    // --- Path Migration (路径迁移) ---
    PathMigrationMode path_migration_mode = kPathMigrationConservative; ///< 路径迁移策略

    // --- Socket (内核缓冲区) ---
    int32_t     recv_buf_size = 1024 * 1024;    ///< UDP 接收缓冲区大小
    int32_t     send_buf_size = 1024 * 1024;    ///< UDP 发送缓冲区大小

    // --- Congestion Control (拥塞控制) ---
    int32_t     cc_algorithm = 0;                       ///< 算法选择: 0-默认(BBR), 1-BBR, 2-Cubic
    uint32_t    clock_granularity_us = 1;               ///< Pacer 时钟粒度 (us)
    uint32_t    bbr_init_cwnd_mss = 32;                 ///< BBR 初始拥塞窗口 (MSS)
    uint32_t    bbr_min_cwnd_mss = 4;                   ///< BBR 最小拥塞窗口 (MSS)
    float       bbr_startup_high_gain = 2.885f;         ///< BBR STARTUP 阶段增益
    float       bbr_cwnd_gain = 2.0f;                   ///< BBR PROBE_BW 阶段 cwnd 增益
    float       bbr_startup_growth_target = 1.25f;      ///< BBR 带宽增长判定倍率
    uint32_t    bbr_startup_full_bw_rounds = 3;         ///< BBR STARTUP 判定退出轮数
    uint32_t    bbr_probe_rtt_ms = 200;                 ///< BBR PROBE_RTT 时长 (ms)
    uint32_t    bbr_min_rtt_expiry_ms = 10000;          ///< BBR min_rtt 过期时间 (ms)
    float       bbr_probe_rtt_multiplier = 0.75f;       ///< BBR ProbeRTT 增益系数
    float       bbr_similar_min_rtt_threshold = 1.125f; ///< BBR 相似 RTT 判定阈值
    std::vector<float> bbr_pacing_gains = {1.25f, 0.75f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; ///< BBR Pacing 周期增益
    double      cubic_beta = 0.7;           ///< CUBIC 丢包回退系数
    double      cubic_c = 0.4;              ///< CUBIC 曲线常数
    uint32_t    cubic_init_cwnd_mss = 32;   ///< CUBIC 初始拥塞窗口 (MSS)
    uint32_t    cubic_min_cwnd_mss = 4;     ///< CUBIC 最小拥塞窗口 (MSS)

    // --- ACK 行为控制 ---
    uint8_t     ack_every_n_packets = 10;   ///< 每收到 N 个包发一次 ACK
    uint32_t    time_threshold_ms = 3;      ///< ACK 延迟时间阈值因子
    uint8_t     max_ack_range_size = 149;   ///< ACK 帧中最大 Range 数量
    uint8_t     ack_delay_exponent = 3;     ///< ACK 延迟指数
    uint16_t    ack_delay = 150;            ///< 最大 ACK 延迟 (ms)

    // --- Transport Parameters (传输参数) ---
    uint16_t    handshake_timeout = 3000;                       ///< 握手超时时间 (ms)
    uint16_t    pending_handshake_retry_interval_ms = 200;      ///< 握手重试周期 (ms)
    uint8_t     pending_handshake_max_retries = 3;              ///< 握手最大重试次数
    uint16_t    pending_pre_handshake_buffer_packets = 16;      ///< 握手前缓冲区包数上限
    uint32_t    pending_pre_handshake_buffer_bytes = 32 * 1024; ///< 握手前缓冲区字节上限
    uint16_t    init_max_streams_bidi = 64;                     ///< 初始最大双向流数
    uint16_t    init_max_streams_uni = 32;                      ///< 初始最大单向流数
    uint64_t    initial_max_data = 64ull * 1024ull * 1024ull;                        ///< 初始连接级流量控制窗口
    uint64_t    initial_max_stream_data_bidi_local = 16ull * 1024ull * 1024ull;      ///< 对端可向本端双向流发送的初始窗口
    uint64_t    initial_max_stream_data_bidi_remote = 16ull * 1024ull * 1024ull;     ///< 本端可向对端双向流发送的初始窗口

    // --- Stream Scheduler (流调度器) ---
    uint8_t     stream_default_priority = 4;           ///< 默认流优先级 (0-7)
    StreamSchedulerMode stream_scheduler_mode = kStreamSchedulerStrict; ///< 流调度模式
    uint16_t    stream_aging_threshold = 8;            ///< 优先级老化阈值
    uint8_t     stream_aging_step = 1;                 ///< 优先级老化步进
    uint16_t    stream_drr_quantum = 1200;             ///< DRR 基准量子
    uint32_t    stream_drr_deficit_cap = 64 * 1024;    ///< DRR 赤字上限
    uint32_t    stream_send_buffer_limit = 64 * 1024;  ///< 单流写缓冲区上限
    bool        stream_enable_coalescing = true;       ///< 是否启用小包聚合发送
    uint16_t    stream_min_payload_before_immediate_send = 1200; ///< 聚合触发阈值
    uint32_t    stream_coalesce_delay_us = 1000;       ///< 聚合等待时延 (us)
    uint32_t    stream_unacked_data_limit = 256 * 1024; ///< 在途未确认数据上限 (bytes)

    // --- Connection Scheduler (连接级 WDRR 调度器) ---
    ConnectionSchedulerMode connection_scheduler_mode = kConnectionSchedulerWdrr; ///< 连接调度模式
    uint32_t    connection_wdrr_quantum = 1200; ///< 连接级 WDRR 量子
    uint32_t    connection_wdrr_deficit_cap = 64 * 1024; ///< 连接级 WDRR 赤字上限
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONFIG_H__
