#ifndef EULAR_UTP_CONGESTION_BBR_H
#define EULAR_UTP_CONGESTION_BBR_H

#include <utp/option.h>

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
    uint32_t initial_cwnd_mss;                         // 初始拥塞窗口，单位 MSS
    uint32_t minimum_cwnd_mss;                         // 最小拥塞窗口，单位 MSS
    double   startup_high_gain;                        // STARTUP pacing 增益
    double   cwnd_gain;                                // PROBE_BW cwnd 增益
    double   startup_growth_target;                    // STARTUP 带宽增长阈值
    uint32_t startup_full_bandwidth_rounds;            // 判定满带宽所需轮数
    uint32_t probe_rtt_ms;                             // PROBE_RTT 持续时间
    uint32_t min_rtt_expiry_ms;                        // min RTT 有效期
    double   pacing_gains[UTP_BBR_PACING_GAIN_COUNT];  // PROBE_BW 增益周期
} utp_bbr_config_t;

typedef struct utp_bbr {
    utp_congestion_t         congestion;                               // 通用拥塞控制接口
    const utp_rtt_stats_t*   rtt_stats;                                // 连接 RTT 统计，不拥有
    utp_bw_sampler_t         sampler;                                  // 带宽采样器
    utp_minmax_t             max_bandwidth;                            // 窗口内最大带宽
    utp_minmax_t             max_ack_height;                           // ACK 聚合高度极值
    uint64_t                 cwnd;                                     // 当前拥塞窗口
    uint64_t                 initial_cwnd;                             // 初始窗口
    uint64_t                 minimum_cwnd;                             // 最小窗口
    uint64_t                 pacing_rate;                              // 当前 pacing 速率
    uint64_t                 acked_bytes;                              // 当前 ACK 批次确认字节
    uint64_t                 ack_max_bandwidth;                        // 当前 ACK 批次最高带宽
    uint64_t                 lost_bytes;                               // 当前 ACK 批次丢失字节
    uint64_t                 ack_time_us;                              // 当前 ACK 批次时间
    uint64_t                 inflight_bytes;                           // 当前飞行字节数
    uint64_t                 max_cwnd;                                 // 允许的窗口上界
    uint64_t                 recovery_window;                          // 恢复期窗口
    uint64_t                 aggregation_epoch_start_us;               // ACK 聚合 epoch 起点
    uint64_t                 aggregation_epoch_bytes;                  // ACK 聚合 epoch 字节数
    uint64_t                 last_sent_packet_number;                  // 最近发送包号
    uint64_t                 max_acked_packet_number;                  // 最大已确认包号
    uint64_t                 current_round_end_packet_number;          // 当前带宽轮次末尾包号
    uint64_t                 end_recovery_packet_number;               // 恢复期结束包号
    uint64_t                 round_count;                              // 已完成的带宽轮次数
    uint64_t                 bandwidth_at_last_round;                  // 上轮带宽估计
    uint64_t                 min_rtt_us;                               // 最小 RTT
    uint64_t                 min_rtt_timestamp_us;                     // 最小 RTT 采样时间
    uint64_t                 min_rtt_since_last_probe_us;              // 上次 PROBE_RTT 后最小 RTT
    uint64_t                 last_cycle_start_us;                      // PROBE_BW 增益周期开始时刻
    uint64_t                 probe_rtt_exit_us;                        // PROBE_RTT 可退出时刻
    uint64_t                 probe_rtt_time_us;                        // 进入 PROBE_RTT 的时刻
    uint64_t                 min_rtt_expiry_us;                        // min RTT 过期时刻
    uint32_t                 cycle_index;                              // 当前 pacing 增益下标
    uint32_t                 rounds_without_bandwidth_gain;            // 连续无带宽增长轮数
    uint32_t                 startup_round_limit;                      // STARTUP 满带宽轮数阈值
    double                   pacing_gain;                              // 当前 pacing 增益
    double                   cwnd_gain;                                // 当前 cwnd 增益
    double                   high_gain;                                // STARTUP 高增益
    double                   high_cwnd_gain;                           // STARTUP cwnd 增益
    double                   drain_gain;                               // DRAIN 增益
    double                   configured_cwnd_gain;                     // 用户配置 cwnd 增益
    double                   startup_growth_target;                    // 用户配置增长阈值
    double                   pacing_gains[UTP_BBR_PACING_GAIN_COUNT];  // pacing 增益周期
    bool                     in_ack : 1;                               // 是否正在 ACK 批处理
    bool                     last_sample_app_limited : 1;              // 最近采样是否应用受限
    bool                     has_non_app_limited_sample : 1;           // 是否已有非应用受限采样
    bool                     full_bandwidth_reached : 1;               // 是否已退出 STARTUP
    bool                     probe_rtt_round_passed : 1;               // PROBE_RTT 是否经过完整轮次
    bool                     ack_has_losses : 1;                       // 当前 ACK 批次是否含丢失
    bool                     ack_has_sample : 1;                       // 当前 ACK 批次是否有带宽样本
    bool                     app_limited_since_last_probe : 1;         // 上次探测后是否应用受限
    bool                     min_rtt_expired_in_ack : 1;               // 当前 ACK 是否发现 min RTT 过期
    utp_bbr_recovery_state_t recovery_state;                           // BBR 恢复子状态
    utp_bbr_mode_t           mode;                                     // BBR 主状态机阶段
} utp_bbr_t;

void              utp_bbr_init(utp_bbr_t* bbr, const utp_bbr_config_t* config);
utp_congestion_t* utp_bbr_as_congestion(utp_bbr_t* bbr);

#endif  // EULAR_UTP_CONGESTION_BBR_H
