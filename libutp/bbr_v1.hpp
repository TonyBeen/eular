/*************************************************************************
    > File Name: bbr_v1.h
    > Author: eular
    > Brief:
    > Created Time: Fri 05 Dec 2025 02:52:53 PM CST
 ************************************************************************/

#pragma once

#include <cstdint>
#include <deque>
#include <algorithm>
#include <limits>

/**
 * 一个简化版的 BBRv1 拥塞控制实现，参考 lsquic_bbr.c / Chromium BBRv1。
 * 主要包含：
 *  - STARTUP / DRAIN / PROBE_BW / PROBE_RTT 四种模式
 *  - 基于带宽估计与 min_rtt 的 BDP 计算
 *  - cwnd / pacing_rate 更新逻辑
 *
 * 时间单位约定：
 *  - 所有时间（now, rtt 等）统一用微秒（microseconds）表示。
 * 带宽单位约定：
 *  - bits/s 或 bytes/s 在这里内部统一用 bytes/s。
 *
 * 你需要在上层：
 *  - 驱动 onPacketSent / onPacketAcked / onPacketLost / onRttUpdate / onAckedBatchEnd
 *  - 调用 getCongestionWindow / getPacingRate 获取发送限制
 */

class BbrV1
{
public:
    // 默认常量，基本照抄 lsquic_bbr.c 中的定义
    static constexpr uint32_t kDefaultTCPMSS                     = 1460;
    static constexpr uint32_t kInitialCongestionWindowPackets    = 32;
    static constexpr uint64_t kInitialCongestionWindowBytes      =
            uint64_t(kInitialCongestionWindowPackets) * kDefaultTCPMSS;
    static constexpr uint32_t kDefaultMinimumCongestionWindow    =
            4 * kDefaultTCPMSS;
    static constexpr uint32_t kDefaultMaxCongestionWindowPackets = 2000;
    static constexpr uint64_t kDefaultMaxCongestionWindowBytes   =
            uint64_t(kDefaultMaxCongestionWindowPackets) * kDefaultTCPMSS;

    // min_rtt 过期时间：10s
    static constexpr uint64_t kMinRttExpiryUs = 10ull * 1000 * 1000; // 10s
    // Probe RTT 持续时间：200ms
    static constexpr uint64_t kProbeRttTimeUs = 200ull * 1000;       // 200ms

    // STARTUP 阶段的高增益：约等于 2/ln(2) 或 4*ln(2)，这里使用 2.885f 版本
    static constexpr float kDefaultHighGain   = 2.885f;
    // STARTUP 退出判定: 若 N 个 RTT 内带宽增长低于 1.25x，则认为到达 full bandwidth
    static constexpr float kStartupGrowthTarget = 1.25f;
    static constexpr uint32_t kRoundTripsWithoutGrowthBeforeExitingStartup = 3;

    // PROBE_BW 模式下的 pacing gain 周期数组（Chromium / lsquic 一致）
    static constexpr float kPacingGainCycle[8] = {
        1.25f, 0.75f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f
    };
    static constexpr size_t kPacingGainCycleLength = 8;

    // cwnd 增益（PROBE_BW 模式下）
    static constexpr float kCwndGain = 2.0f;

    enum Mode {
        STARTUP,
        DRAIN,
        PROBE_BW,
        PROBE_RTT,
    };

    BbrV1()
    {
        reset();
    }

    /**
     * 重置 BBR 状态，类似 lsquic_bbr_init / init_bbr
     * 上层在新建连接或切换到 BBR 时调用一次。
     */
    void reset()
    {
        // 模式和轮次
        mode_ = STARTUP;
        round_count_ = 0;
        current_round_trip_end_ = invalidPacketNumber();

        // cwnd 初始值
        init_cwnd_ = kInitialCongestionWindowBytes;
        cwnd_      = kInitialCongestionWindowBytes;
        max_cwnd_  = kDefaultMaxCongestionWindowBytes;
        min_cwnd_  = kDefaultMinimumCongestionWindow;

        // gain 参数
        high_gain_      = kDefaultHighGain;
        high_cwnd_gain_ = kDefaultHighGain;
        drain_gain_     = 1.0f / kDefaultHighGain;

        pacing_gain_ = 1.0f;
        cwnd_gain_   = 1.0f;

        // 带宽与 RTT
        max_bandwidth_bytes_per_sec_ = 0;
        min_rtt_us_                  = 0;
        min_rtt_timestamp_us_        = 0;
        min_rtt_since_last_probe_us_ = std::numeric_limits<uint64_t>::max();

        // 其他状态
        last_cycle_start_us_     = 0;
        cycle_current_offset_    = 0;

        is_at_full_bandwidth_    = false;
        round_wo_bw_gain_        = 0;
        bw_at_last_round_bytes_per_sec_ = 0;

        exit_probe_rtt_at_us_    = 0;
        probe_rtt_round_passed_  = false;
        exiting_quiescence_      = false;

        last_sample_app_limited_ = false;
        has_non_app_limited_     = false;
        app_limited_since_last_probe_rtt_ = false;

        last_sent_packet_        = invalidPacketNumber();

        // recovery 相关
        recovery_state_          = RecoveryState::NOT_IN_RECOVERY;
        end_recovery_at_         = 0;
        recovery_window_         = 0;

        // pacing rate 缓存
        pacing_rate_bytes_per_sec_ = 0;

        // ACK 批次状态
        ack_state_ = AckState{};
    }

    // ========== 上层需要驱动的接口 ==========

    /**
     * 发送一个包时调用，类似 lsquic_bbr_sent / bw_sampler_packet_sent
     *
     * @param packet_number  发送包的序号（单调递增）
     * @param bytes          此包大小（字节）
     * @param now_us         当前时间（微秒）
     * @param bytes_in_flight 发送前 in-flight 字节数
     * @param app_limited    当前是否应用层无数据导致的限速（而非网络瓶颈）
     */
    void onPacketSent(uint64_t packet_number,
                      uint32_t bytes,
                      uint64_t now_us,
                      uint64_t bytes_in_flight,
                      bool app_limited)
    {
        // 记录发送事件，用于后面估算带宽
        SentPacket sp;
        sp.packet_number = packet_number;
        sp.bytes         = bytes;
        sp.send_time_us  = now_us;
        sp.bytes_in_flight = bytes_in_flight;
        sent_packets_.push_back(sp);

        last_sent_packet_ = packet_number;

        if (app_limited)
            appLimited(bytes_in_flight);
    }

    /**
     * 收到一个 ACK，确认了某个包
     * 注：实践中 ACK 通常会确认多个包，你可以为每个被确认的包调用一次本接口，
     * 然后在一批 ACK 处理结束后，调用 onAckedBatchEnd。
     *
     * @param packet_number   被确认包号
     * @param now_us          ACK 到达时间
     * @param bytes_in_flight 这时的 in-flight 字节数
     */
    void onPacketAcked(uint64_t packet_number,
                       uint64_t now_us,
                       uint64_t bytes_in_flight)
    {
        // 第一次 acked 时，初始化 ACK 批次状态
        if (!ack_state_.in_ack) {
            beginAck(now_us, bytes_in_flight);
        }

        // 在已发送队列中找到该包
        auto it = findSentPacket(packet_number);
        if (it == sent_packets_.end())
            return; // 已经处理过或未知包，忽略

        uint32_t bytes = it->bytes;
        uint64_t send_time_us = it->send_time_us;
        uint64_t send_bytes_in_flight = it->bytes_in_flight;

        // RTT 样本：now - send_time
        uint64_t rtt_us = (now_us > send_time_us) ? (now_us - send_time_us) : 0;

        // 简化版的带宽样本：bytes / (now - send_time)
        uint64_t bw_sample = 0;
        if (rtt_us > 0) {
            // bytes / (rtt_us / 1e6) = bytes * 1e6 / rtt_us
            bw_sample = (uint64_t)bytes * 1000000ull / rtt_us;
        }

        // 更新 RTT 统计（简单版本：维护 min_rtt 和 last_rtt）
        if (rtt_us > 0) {
            last_rtt_us_ = rtt_us;
            if (min_rtt_us_ == 0 || rtt_us < min_rtt_us_) {
                min_rtt_us_ = rtt_us;
                min_rtt_timestamp_us_ = now_us;
            }
        }

        // 更新 ack batch 内的最小 RTT
        if (ack_state_.sample_min_rtt_us == std::numeric_limits<uint64_t>::max()
            || rtt_us < ack_state_.sample_min_rtt_us)
        {
            ack_state_.sample_min_rtt_us = rtt_us;
        }

        // 更新 acked_bytes
        ack_state_.acked_bytes += bytes;

        // 更新 ack 最大包号
        if (!isValidPacketNumber(ack_state_.max_packet_number) ||
            packet_number > ack_state_.max_packet_number)
        {
            ack_state_.max_packet_number = packet_number;
        }

        // 样本不是 app-limited：允许更新 max_bandwidth
        // （这里我们简化处理：如果这个样本带宽比当前 max_bandwidth 高，就更新）
        if (!ack_state_.last_sample_app_limited && bw_sample > 0) {
            if (!app_limited_since_last_probe_rtt_ ||
                bw_sample > max_bandwidth_bytes_per_sec_)
            {
                max_bandwidth_bytes_per_sec_ = bw_sample;
            }
        }

        // 当前简单实现中，不做真正的“采样队列”和 “app-limited 复杂标志位”处理
        // 仅保留必要的信息。
    }

    /**
     * 标记一个包丢失
     * 
     * @param packet_number 被判定丢失的包号
     */
    void onPacketLost(uint64_t packet_number)
    {
        // 标记本批 ACK 中有丢包
        ack_state_.has_losses = true;
        // 统计丢失的字节数
        auto it = findSentPacket(packet_number);
        if (it != sent_packets_.end()) {
            ack_state_.lost_bytes += it->bytes;
            sent_packets_.erase(it);
        }
    }

    /**
     * 一批 ACK 处理结束时调用（类似 lsquic_bbr_end_ack）
     * 
     * @param now_us          当前时间（最后一个 ACK 的时间）
     * @param bytes_in_flight 这批 ACK 结束后的 in-flight 字节数
     */
    void onAckedBatchEnd(uint64_t now_us, uint64_t bytes_in_flight)
    {
        if (!ack_state_.in_ack)
            return;

        endAck(now_us, bytes_in_flight);
    }

    /**
     * 上层在检测到连接长时间 idle 后重新发送前，可以调用此接口
     * （类似 lsquic_bbr_was_quiet，当前实现只是记录一个标志）
     */
    void onConnectionWasQuiet(uint64_t /*now_us*/, uint64_t /*bytes_in_flight*/)
    {
        exiting_quiescence_ = true;
    }

    // ========== 查询接口：给发送端使用 ==========

    /** 获取当前建议的 cwnd（字节） */
    uint64_t getCongestionWindow() const
    {
        if (mode_ == PROBE_RTT) {
            return getProbeRttCwnd();
        }

        if (inRecovery()) {
            return std::min(cwnd_, recovery_window_);
        }

        return cwnd_;
    }

    /** 获取当前建议的 pacing rate（字节/秒） */
    uint64_t getPacingRateBytesPerSec() const
    {
        // 若没有可用的 pacing_rate，就基于初始 cwnd 和 min_rtt 估算一个
        if (pacing_rate_bytes_per_sec_ == 0) {
            uint64_t mrtt = getMinRtt();
            if (mrtt == 0) {
                // 给个默认 RTT 25ms，避免除零
                mrtt = 25000;
            }
            // bytes / (mrtt / 1e6) = bytes * 1e6 / mrtt
            uint64_t bw = init_cwnd_ * 1000000ull / mrtt;
            return static_cast<uint64_t>(bw * high_cwnd_gain_);
        }
        return pacing_rate_bytes_per_sec_;
    }

    /** 返回当前所处模式，便于调试 */
    Mode mode() const { return mode_; }

private:
    // ========== 内部辅助结构 ==========

    struct SentPacket {
        uint64_t packet_number;
        uint32_t bytes;
        uint64_t send_time_us;
        uint64_t bytes_in_flight;
    };

    // ACK 批次状态（模仿 lsquic_bbr_ack_state 的精简版本）
    struct AckState {
        bool in_ack = false;

        uint64_t ack_time_us = 0;
        uint64_t in_flight   = 0;     // ack 开始时的 in-flight

        uint64_t acked_bytes = 0;
        uint64_t lost_bytes  = 0;

        uint64_t max_packet_number = invalidPacketNumber();

        bool has_losses = false;

        // 一批 ACK 中采集到的最小 RTT（us）
        uint64_t sample_min_rtt_us =
            std::numeric_limits<uint64_t>::max();

        bool last_sample_app_limited = false;
    };

    // Recovery 状态，参考 BBRv1 的逻辑
    enum class RecoveryState {
        NOT_IN_RECOVERY,
        CONSERVATION,
        GROWTH,
    };

    // ========== 内部状态变量 ==========

    // 模式 & 轮次
    Mode mode_;
    uint64_t round_count_;
    uint64_t current_round_trip_end_;

    // cwnd 及边界
    uint64_t init_cwnd_;
    uint64_t cwnd_;
    uint64_t max_cwnd_;
    uint64_t min_cwnd_;

    // gain 参数
    float high_gain_;
    float high_cwnd_gain_;
    float drain_gain_;
    float pacing_gain_;
    float cwnd_gain_;

    // RTT & 带宽估计
    uint64_t max_bandwidth_bytes_per_sec_;
    uint64_t min_rtt_us_;
    uint64_t min_rtt_timestamp_us_;
    uint64_t min_rtt_since_last_probe_us_;
    uint64_t last_rtt_us_ = 0;

    // PROBE_BW 周期控制
    uint64_t last_cycle_start_us_;
    size_t   cycle_current_offset_;

    // full bandwidth 判定相关
    bool     is_at_full_bandwidth_;
    uint32_t round_wo_bw_gain_;
    uint64_t bw_at_last_round_bytes_per_sec_;

    // PROBE_RTT 控制
    uint64_t exit_probe_rtt_at_us_;
    bool     probe_rtt_round_passed_;
    bool     exiting_quiescence_;

    // app-limited 相关
    bool     last_sample_app_limited_;
    bool     has_non_app_limited_;
    bool     app_limited_since_last_probe_rtt_;

    // Recovery 相关
    RecoveryState recovery_state_;
    uint64_t      end_recovery_at_;
    uint64_t      recovery_window_;

    // pacing 速率缓存
    uint64_t pacing_rate_bytes_per_sec_;

    // 最近一个发送包号
    uint64_t last_sent_packet_;

    // ACK 批次状态
    AckState ack_state_;

    // 已发送但尚未确认或丢失的包队列（简单模拟发送历史）
    std::deque<SentPacket> sent_packets_;

    // ========== 内部工具函数 ==========

    static uint64_t invalidPacketNumber()
    {
        return std::numeric_limits<uint64_t>::max();
    }

    static bool isValidPacketNumber(uint64_t pn)
    {
        return pn != invalidPacketNumber();
    }

    uint64_t getMinRtt() const
    {
        if (min_rtt_us_ != 0)
            return min_rtt_us_;
        // 如果还没有 RTT 样本，假设 25ms
        return 25000;
    }

    auto findSentPacket(uint64_t packet_number)
    {
        return std::find_if(sent_packets_.begin(), sent_packets_.end(),
                            [packet_number](const SentPacket &sp){
                                return sp.packet_number == packet_number;
                            });
    }

    // 应用层限速标记，类似 lsquic 的 bbr_app_limited()
    void appLimited(uint64_t bytes_in_flight)
    {
        uint64_t cwnd = getCongestionWindow();
        if (bytes_in_flight >= cwnd)
            return;

        // 简化一下逻辑：如果 in-flight 还远小于 cwnd，就认为是 app-limited。
        app_limited_since_last_probe_rtt_ = true;
        last_sample_app_limited_ = true;
    }

    // ======== ACK 批次控制：beginAck / endAck ========

    void beginAck(uint64_t ack_time_us, uint64_t bytes_in_flight)
    {
        ack_state_ = AckState{};
        ack_state_.in_ack = true;
        ack_state_.ack_time_us = ack_time_us;
        ack_state_.in_flight   = bytes_in_flight;
        ack_state_.max_packet_number = invalidPacketNumber();
    }

    void endAck(uint64_t now_us, uint64_t bytes_in_flight)
    {
        ack_state_.in_ack = false;

        uint64_t bytes_acked = ack_state_.acked_bytes;
        bool is_round_start = false;
        bool min_rtt_expired = false;

        if (bytes_acked > 0) {
            // 判断是否进入新的 RTT 轮次
            if (!isValidPacketNumber(current_round_trip_end_) ||
                ack_state_.max_packet_number > current_round_trip_end_)
            {
                round_count_++;
                current_round_trip_end_ = last_sent_packet_;
                is_round_start = true;
            }

            // 更新 min_rtt 及判断是否过期
            min_rtt_expired = updateBandwidthAndMinRtt(now_us);

            // 更新 recovery 状态机
            updateRecoveryState(is_round_start);
        }

        // PROBE_BW 模式下更新 gain 周期
        if (mode_ == PROBE_BW) {
            updateGainCyclePhase(bytes_in_flight, now_us);
        }

        // 若进入新 RTT 且尚未 full bandwidth，检查 full BW
        if (is_round_start && !is_at_full_bandwidth_) {
            checkIfFullBandwidthReached();
        }

        // 可能从 STARTUP / DRAIN 切换出去
        maybeExitStartupOrDrain(bytes_in_flight, now_us);

        // 处理 PROBE_RTT 进入/退出
        maybeEnterOrExitProbeRtt(now_us, is_round_start,
                                 min_rtt_expired, bytes_in_flight);

        // 计算 cwnd / pacing 以及 recovery_window
        uint64_t bytes_lost = ack_state_.lost_bytes;

        calculatePacingRate();
        calculateCongestionWindow(bytes_acked);
        calculateRecoveryWindow(bytes_acked, bytes_lost, bytes_in_flight);

        // 清理已 ack 的 sent_packets：简单做法是按包号过滤
        cleanupAckedPackets();
    }

    void cleanupAckedPackets()
    {
        // sent_packets_ 中目前留下的是：尚未被 onPacketAcked / onPacketLost 清除的。
        // 实践中你可以在 onPacketAcked/onPacketLost 里立即删除；
        // 这里我们在 endAck 中可以再清一遍防御性清理（根据实际需要决定）。
    }

    // ======== min_rtt / 带宽更新 ========

    bool updateBandwidthAndMinRtt(uint64_t ack_time_us)
    {
        // 我们已经在 onPacketAcked 中用样本更新过 max_bandwidth 与 min_rtt；
        // 这里主要做 min_rtt 过期判断及扩展逻辑。

        bool min_rtt_expired = false;
        uint64_t now = ack_time_us;

        if (min_rtt_us_ != 0 &&
            now > min_rtt_timestamp_us_ + kMinRttExpiryUs)
        {
            // min_rtt 看起来过期了
            min_rtt_expired = true;
        }

        if (ack_state_.sample_min_rtt_us !=
                std::numeric_limits<uint64_t>::max())
        {
            // 更新“自上次 probe_rtt 以来的最小 RTT”
            if (min_rtt_since_last_probe_us_ ==
                std::numeric_limits<uint64_t>::max())
            {
                min_rtt_since_last_probe_us_ = ack_state_.sample_min_rtt_us;
            }
            else {
                min_rtt_since_last_probe_us_ =
                    std::min(min_rtt_since_last_probe_us_,
                             ack_state_.sample_min_rtt_us);
            }

            // 如果当前没有 min_rtt，或者这次样本更小，则更新
            if (min_rtt_us_ == 0 ||
                ack_state_.sample_min_rtt_us < min_rtt_us_)
            {
                min_rtt_us_ = ack_state_.sample_min_rtt_us;
                min_rtt_timestamp_us_ = now;
                min_rtt_since_last_probe_us_ =
                    std::numeric_limits<uint64_t>::max();
                app_limited_since_last_probe_rtt_ = false;
                min_rtt_expired = false; // 更新后不再视为过期
            }
        }

        return min_rtt_expired;
    }

    // ======== Recovery 状态机 ========

    bool inRecovery() const
    {
        return recovery_state_ != RecoveryState::NOT_IN_RECOVERY;
    }

    void updateRecoveryState(bool is_round_start)
    {
        if (ack_state_.has_losses) {
            end_recovery_at_ = last_sent_packet_;
        }

        switch (recovery_state_) {
        case RecoveryState::NOT_IN_RECOVERY:
            if (ack_state_.has_losses) {
                // 首次检测到丢包：进入 CONSERVATION
                recovery_state_ = RecoveryState::CONSERVATION;
                // recovery_window 在 calculateRecoveryWindow 里初始化
                recovery_window_ = 0;
                // 当前 RTT 的结束点重新设置
                current_round_trip_end_ = last_sent_packet_;
            }
            break;
        case RecoveryState::CONSERVATION:
            if (is_round_start) {
                recovery_state_ = RecoveryState::GROWTH;
            }
            // fallthrough
        case RecoveryState::GROWTH:
            if (!ack_state_.has_losses &&
                isValidPacketNumber(ack_state_.max_packet_number) &&
                ack_state_.max_packet_number > end_recovery_at_)
            {
                recovery_state_ = RecoveryState::NOT_IN_RECOVERY;
            }
            break;
        }
    }

    // ======== PROBE_BW gain 周期 ========

    void updateGainCyclePhase(uint64_t bytes_in_flight, uint64_t now_us)
    {
        uint64_t prior_in_flight = ack_state_.in_flight;
        uint64_t min_rtt = getMinRtt();

        // 默认：一个 phase 至少要跑过一个 RTT
        bool should_advance =
            (now_us - last_cycle_start_us_) > min_rtt;

        // 若 pacing_gain > 1.0（探测阶段），需要确保真正把 in-flight 提升到 pacing_gain * BDP
        if (pacing_gain_ > 1.0f &&
            !ack_state_.has_losses &&
            prior_in_flight < getTargetCwnd(pacing_gain_))
        {
            should_advance = false;
        }

        // 若 pacing_gain < 1.0（drain 阶段），一旦 in-flight 降到 BDP 以下，即可提前结束
        if (pacing_gain_ < 1.0f &&
            bytes_in_flight <= getTargetCwnd(1.0f))
        {
            should_advance = true;
        }

        if (!should_advance)
            return;

        // 切到下一个 gain phase
        cycle_current_offset_ =
            (cycle_current_offset_ + 1) % kPacingGainCycleLength;
        last_cycle_start_us_ = now_us;

        pacing_gain_ = kPacingGainCycle[cycle_current_offset_];
    }

    // ======== full bandwidth 判定 ========

    void checkIfFullBandwidthReached()
    {
        if (max_bandwidth_bytes_per_sec_ == 0)
            return;

        if (last_sample_app_limited_) {
            // 最近的样本是 app-limited，不能据此判断 full bw
            return;
        }

        uint64_t target_bw =
            static_cast<uint64_t>(bw_at_last_round_bytes_per_sec_ *
                                  kStartupGrowthTarget);

        if (max_bandwidth_bytes_per_sec_ >= target_bw) {
            bw_at_last_round_bytes_per_sec_ = max_bandwidth_bytes_per_sec_;
            round_wo_bw_gain_ = 0;
            return;
        }

        round_wo_bw_gain_++;
        if (round_wo_bw_gain_ >= kRoundTripsWithoutGrowthBeforeExitingStartup) {
            is_at_full_bandwidth_ = true;
        }
    }

    // ======== 模式切换：STARTUP / DRAIN / PROBE_BW ========

    void enterProbeBw(uint64_t now_us)
    {
        mode_      = PROBE_BW;
        cwnd_gain_ = kCwndGain;

        // 简化：从 0 开始 gain 周期
        cycle_current_offset_ = 0;
        last_cycle_start_us_  = now_us;
        pacing_gain_          = kPacingGainCycle[cycle_current_offset_];
    }

    void enterStartup()
    {
        mode_ = STARTUP;
        pacing_gain_ = high_gain_;
        cwnd_gain_   = high_cwnd_gain_;
    }

    void maybeExitStartupOrDrain(uint64_t bytes_in_flight, uint64_t now_us)
    {
        if (mode_ == STARTUP && is_at_full_bandwidth_) {
            // 退出 STARTUP，进入 DRAIN
            mode_ = DRAIN;
            pacing_gain_ = drain_gain_;
            cwnd_gain_   = high_cwnd_gain_;
        }

        if (mode_ == DRAIN) {
            uint64_t target_cwnd = getTargetCwnd(1.0f);
            if (bytes_in_flight <= target_cwnd) {
                // drain 完成，进入 PROBE_BW
                enterProbeBw(now_us);
            }
        }
    }

    // ======== PROBE_RTT 控制 ========

    uint64_t getProbeRttCwnd() const
    {
        // 简化：直接采用 min_cwnd
        return min_cwnd_;
    }

    void maybeEnterOrExitProbeRtt(uint64_t now_us,
                                  bool is_round_start,
                                  bool min_rtt_expired,
                                  uint64_t bytes_in_flight)
    {
        // 若 min_rtt 过期且不在 PROBE_RTT，则进入 PROBE_RTT
        if (min_rtt_expired &&
            !exiting_quiescence_ &&
            mode_ != PROBE_RTT)
        {
            // 若此时在 STARTUP，顺路做一次退出处理（这里简化：不统计）
            mode_ = PROBE_RTT;
            pacing_gain_ = 1.f;
            exit_probe_rtt_at_us_ = 0;
        }

        if (mode_ != PROBE_RTT) {
            exiting_quiescence_ = false;
            return;
        }

        // PROBE_RTT 模式：尽量把 in-flight 降到 getProbeRttCwnd()
        if (exit_probe_rtt_at_us_ == 0) {
            if (bytes_in_flight < getProbeRttCwnd() + kDefaultTCPMSS) {
                exit_probe_rtt_at_us_ = now_us + kProbeRttTimeUs;
                probe_rtt_round_passed_ = false;
            }
        } else {
            if (is_round_start)
                probe_rtt_round_passed_ = true;
            if (now_us >= exit_probe_rtt_at_us_ && probe_rtt_round_passed_) {
                min_rtt_timestamp_us_ = now_us;
                if (!is_at_full_bandwidth_) {
                    enterStartup();
                } else {
                    enterProbeBw(now_us);
                }
            }
        }

        exiting_quiescence_ = false;
    }

    // ======== pacing rate / cwnd / recovery_window ========

    uint64_t getTargetCwnd(float gain) const
    {
        uint64_t bw = max_bandwidth_bytes_per_sec_;
        if (bw == 0) {
            // 若还没有带宽样本，用 init_cwnd / min_rtt 估算 BDP
            uint64_t mrtt = getMinRtt();
            bw = init_cwnd_ * 1000000ull / mrtt;
        }
        uint64_t bdp = getMinRtt() * bw / 1000000ull; // bytes

        uint64_t cwnd = static_cast<uint64_t>(gain * bdp);
        if (cwnd == 0)
            cwnd = static_cast<uint64_t>(gain * init_cwnd_);
        cwnd = std::max(cwnd, min_cwnd_);
        return cwnd;
    }

    void calculatePacingRate()
    {
        uint64_t bw = max_bandwidth_bytes_per_sec_;
        if (bw == 0)
            return;

        uint64_t target_rate =
            static_cast<uint64_t>(bw * pacing_gain_);

        if (is_at_full_bandwidth_) {
            pacing_rate_bytes_per_sec_ = target_rate;
            return;
        }

        // 若还没有 pacing_rate，基于 init_cwnd 和 min_rtt 来一发
        if (pacing_rate_bytes_per_sec_ == 0 && min_rtt_us_ != 0) {
            pacing_rate_bytes_per_sec_ =
                init_cwnd_ * 1000000ull / min_rtt_us_;
            return;
        }

        // STARTUP 阶段：不降低 pacing_rate
        if (pacing_rate_bytes_per_sec_ < target_rate) {
            pacing_rate_bytes_per_sec_ = target_rate;
        }
    }

    void calculateCongestionWindow(uint64_t bytes_acked)
    {
        if (mode_ == PROBE_RTT) {
            // PROBE_RTT 下，不增长 cwnd
            return;
        }

        uint64_t target_window = getTargetCwnd(cwnd_gain_);

        if (is_at_full_bandwidth_) {
            // full bandwidth 后，让 cwnd 向 target_window 逼近
            cwnd_ = std::min(target_window, cwnd_ + bytes_acked);
        } else {
            // STARTUP 等阶段：只增不减
            if (cwnd_ < target_window) {
                cwnd_ += bytes_acked;
            }
        }

        // 边界限制
        if (cwnd_ < min_cwnd_)
            cwnd_ = min_cwnd_;
        if (cwnd_ > max_cwnd_)
            cwnd_ = max_cwnd_;
    }

    void calculateRecoveryWindow(uint64_t bytes_acked,
                                 uint64_t bytes_lost,
                                 uint64_t bytes_in_flight)
    {
        if (recovery_state_ == RecoveryState::NOT_IN_RECOVERY)
            return;

        if (recovery_window_ == 0) {
            // 初始化 recovery_window 为当前 in-flight + acked
            recovery_window_ = bytes_in_flight + bytes_acked;
            if (recovery_window_ < min_cwnd_)
                recovery_window_ = min_cwnd_;
            return;
        }

        // 从 recovery_window 中减去丢失的字节
        if (recovery_window_ >= bytes_lost) {
            recovery_window_ -= bytes_lost;
        } else {
            // 防止 underflow
            recovery_window_ = kDefaultTCPMSS;
        }

        // GROWTH 阶段：再加回 acked 字节，实现类似慢启动增长
        if (recovery_state_ == RecoveryState::GROWTH) {
            recovery_window_ += bytes_acked;
        }

        // 至少允许当前 in-flight + acked
        if (recovery_window_ < bytes_in_flight + bytes_acked) {
            recovery_window_ = bytes_in_flight + bytes_acked;
        }
        // 不小于 min_cwnd
        if (recovery_window_ < min_cwnd_)
            recovery_window_ = min_cwnd_;
    }
};
