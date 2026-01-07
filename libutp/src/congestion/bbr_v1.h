/*************************************************************************
    > File Name: bbr.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:31 PM CST
 ************************************************************************/

#ifndef __CONGESTION_BBR_V1_H__
#define __CONGESTION_BBR_V1_H__

#include <list>

#include "congestion/congestion.h"
#include "congestion/rtt.h"
#include "congestion/bw_sampler.h"
#include "congestion/minmax.h"

namespace eular {
namespace utp {
class BbrV1 : public Congestion {
public:
    enum Mode {
        StartUp,
        Drain,
        ProbeBW,
        ProbeRTT
    };

    enum RecoveryState {
        NotInRecovery, // 未发生丢包
        Conservation, // packet conservation, 保守阶段, 主要控制住窗口, 尽量减少丢包
        Growth, // 增长阶段
    };

    enum Flags {
        // onBeginAck() has been called
        BBR_FLAG_IN_ACK                  = 1 << 0,
        BBR_FLAG_LAST_SAMPLE_APP_LIMITED = 1 << 1,
        BBR_FLAG_HAS_NON_APP_LIMITED     = 1 << 2,
        BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT = 1 << 3,
        BBR_FLAG_PROBE_RTT_DISABLED_IF_APP_LIMITED = 1 << 4,
        BBR_FLAG_PROBE_RTT_SKIPPED_IF_SIMILAR_RTT = 1 << 5,
        BBR_FLAG_EXIT_STARTUP_ON_LOSS    = 1 << 6,
        BBR_FLAG_IS_AT_FULL_BANDWIDTH    = 1 << 7,
        BBR_FLAG_EXITING_QUIESCENCE      = 1 << 8,
        BBR_FLAG_PROBE_RTT_ROUND_PASSED  = 1 << 9,
        BBR_FLAG_FLEXIBLE_APP_LIMITED    = 1 << 10,
        // If true, will not exit low gain mode until bytes_in_flight drops
        // below BDP or it's time for high gain mode.
        BBR_FLAG_DRAIN_TO_TARGET         = 1 << 11,
        // When true, expire the windowed ack aggregation values in STARTUP
        // when bandwidth increases more than 25%.
        BBR_FLAG_EXPIRE_ACK_AGG_IN_STARTUP = 1 << 12,
        // If true, use a CWND of 0.75*BDP during probe_rtt instead of 4 packets.
        BBR_FLAG_PROBE_RTT_BASED_ON_BDP  = 1 << 13,
        // When true, pace at 1.5x and disable packet conservation in STARTUP.
        BBR_FLAG_SLOWER_STARTUP          = 1 << 14,
        // When true, add the most recent ack aggregation measurement during STARTUP.
        BBR_FLAG_ENABLE_ACK_AGG_IN_STARTUP = 1 << 15,
        // When true, disables packet conservation in STARTUP.
        BBR_FLAG_RATE_BASED_STARTUP      = 1 << 16,
    };

    struct AckState {
        std::list<BWSample> sampleList{};
        uint64_t            ackTime{};
        uint64_t            maxPackNo{};
        uint64_t            ackedBytes{};
        uint64_t            lostBytes{};
        uint64_t            totalBytesAckedBefore{};
        uint64_t            inflightBytes{};
        bool                hasLosses{};
    };

    BbrV1() = default;
    ~BbrV1() = default;

    virtual void        onInit(RttStats *stats) override;
    virtual uint64_t    getPacingRate(int32_t inRecovery) override;
    virtual uint64_t    getCwnd() override;
    virtual void        onBeginAck(uint64_t ackTimeUs, uint64_t inflight) override;
    virtual void        onAck(PacketInfo *packetInfo, uint64_t nowUs, int32_t appLimited) override;
    virtual void        onLost(PacketInfo *packetInfo) override;
    virtual void        onPacketSent(PacketInfo *packetInfo, uint64_t inflight, int32_t isAppLimited) override;
    virtual void        wasQuiet(uint64_t nowUs, uint64_t inflight) override;
    virtual void        onEndAck(uint64_t inflight) override;

protected:
    void        setStartupValues(); // lsquic set_startup_values
    uint64_t    getMinRtt(); // lsquic get_min_rtt
    uint64_t    getProbeRttCwnd(); // lsquic get_probe_rtt_cwnd
    uint64_t    getTargetCwnd(float_t gain); // lsquic get_target_cwnd
    bool        inRecovery(); // lsquic in_recovery
    void        appLimited(uint64_t inflight); // lsquic bbr_app_limited
    bool        isPipeSufficientlyFull(uint64_t inflight); // lsquic is_pipe_sufficiently_full
    bool        updateBandwidthAndMinRtt(); // lsquic update_bandwidth_and_min_rtt
    void        updateRecoveryState(bool isRoundStart); // lsquic update_recovery_state
    void        calculatePacingRate(); // lsquic calculate_pacing_rate
    void        calculateCwnd(uint64_t bytesAcked, uint64_t excessAcked); // lsquic calculate_cwnd
    void        calculateRecoveryWindow(uint64_t bytesAcked, uint64_t bytestLost, uint64_t bytestInflight); // lsquic calculate_recovery_window
    uint64_t    updateAckAggregationBytes(uint64_t bytesAcked); // lsquic update_ack_aggregation_bytes
    void        updateGainCyclePhase(uint64_t bytestInflight); // lsquic update_gain_cycle_phase
    void        checkIsFullBwReached(); // lsquic check_is_full_bw_reached
    void        maybeExitStartupOrDrain(uint64_t now, uint64_t bytestInflight); // lsquic maybe_exit_startup_or_drain
    void        maybeEnterOrExitProbeRtt(uint64_t now, bool isRoundStart, bool minRttExpired, uint64_t bytestInflight); // lsquic maybe_enter_or_exit_probe_rtt
    bool        shouldExtendMinRttExpiry(); // lsquic should_extend_min_rtt_expiry

protected:
    void        onExitStartup(uint64_t now); // lsquic on_exit_startup
    void        setMode(Mode newMode); // lsquic set_mode
    void        enterProbeBWMode(uint64_t now); // lsquic enter_probe_bw_mode
    void        enterStartupMode(uint64_t now); // lsquic enter_startup_mode
    bool        isSlowStart(); // lsquic is_slow_start

private:
    Mode                m_mode;
    RecoveryState       m_recoveryState;
    uint64_t            m_flags;
    uint32_t            m_cycleCurrentOffset;
    RttStats*           m_rttStats = nullptr;
    BandwidthSampler    m_bwSampler;
    MinMax              m_maxBandwidth;
    MinMax              m_maxAckHeight; // 用于记录 ACK 聚合的最大值

    uint64_t            m_initCwnd;
    uint64_t            m_minCwnd;
    uint64_t            m_maxCwnd;
    uint64_t            m_cwnd;

    uint64_t            m_aggregationEpochStartTime; // 聚合开始时间戳
    uint64_t            m_aggregationEpochBytes; // 聚合周期内被确认的字节数

    uint64_t            m_lastSentPackNo;
    uint64_t            m_currentRoundTripEnd; // 标记“当前往返轮次”结束的包序号

    uint64_t            m_endRecoveryAt; // 标记“何时允许退出丢包恢复模式”的包号, 大于等于即可
    uint64_t            m_roundCount; // 被确认包计数
    BandWidth           m_pacingRate; // 当前计算的发包速率
    uint64_t            m_startupBytesLost; // 在 startup 模式下丢失的总字节数
    float_t             m_pacingGain; // 当前速率增益因子
    float_t             m_highGain; // startup 阶段使用的 pacing gain 值
    float_t             m_highCwndGain; // startup 阶段用于计算拥塞窗口 CWND 的增益系数
    float_t             m_drainGain; // drain 阶段使用的 pacing gain 值
    uint32_t            m_nStartupRtts; // startup 模式保持的最小 RTT 轮数, default 3
    uint32_t            m_roundWoBwGain; // 记录带宽未增长的轮次. 如果达到阈值m_nStartupRtts, 通常认为带宽已达上限, 需要从 startup 模式切换到 drain 模式
    float_t             m_cwndGain; // 用于计算拥塞窗口 CWND 的增益系数
    BandWidth           m_bwAtLastRound; // 上一轮的带宽估计值

    uint64_t            m_lastCycleStart; // 记录最近一次 pacing gain 周期开始的时间
    uint64_t            m_exitProbeRttAt; // 准备退出 PROBE_RTT 状态的预定时间点

    uint64_t            m_minRttSinceLastProbe; // 自上次 PROBE_RTT 之后观测到的最小 RTT 值
    uint64_t            m_minRtt; // 历史观测到的全局最小 RTT 值
    uint64_t            m_minRttTimestamp; // 记录 m_minRtt 上次更新的时间点
    uint64_t            m_recoveryWindow; // 在丢包恢复期间使用的拥塞窗口大小

    AckState            m_ackState;
};

} // namespace utp
} // namespace eular

#endif // __CONGESTION_BBR_V1_H__
