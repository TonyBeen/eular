/*************************************************************************
    > File Name: bbr.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:33 PM CST
 ************************************************************************/

#include "congestion/bbr_v1.h"

#include <inttypes.h>

#include <random>

#include "proto/packet_common.h"
#include "logger/logger.h"
#include "bbr_v1.h"

#define ms(val_)    ((val_) * 1000)
#define sec(val_)   ((val_) * 1000 * 1000)

// Default maximum packet size used in the Linux TCP implementation.
// Used in QUIC for congestion window computations in bytes.
#define kDefaultTCPMSS 1460
#define kMaxSegmentSize kDefaultTCPMSS

// Constants based on TCP defaults.
// The minimum CWND to ensure delayed acks don't reduce bandwidth measurements.
// Does not inflate the pacing rate.
#define kDefaultMinimumCongestionWindow  (4 * kDefaultTCPMSS)

// The gain used for the STARTUP, equal to 2/ln(2).
#define kDefaultHighGain 2.885f

// The newly derived gain for STARTUP, equal to 4 * ln(2)
#define kDerivedHighGain 2.773f

// The newly derived CWND gain for STARTUP, 2.
#define kDerivedHighCWNDGain 2.0f

// The gain used in STARTUP after loss has been detected.
// 1.5 is enough to allow for 25% exogenous loss and still observe a 25% growth
// in measured bandwidth.
#define kStartupAfterLossGain 1.5f

// We match SPDY's use of 32 (since we'd compete with SPDY).
#define kInitialCongestionWindow 32

/* Taken from send_algorithm_interface.h */
#define kDefaultMaxCongestionWindowPackets 2000

// The time after which the current min_rtt value expires.
#define kMinRttExpiry sec(10)

// Coefficient to determine if a new RTT is sufficiently similar to min_rtt that
// we don't need to enter PROBE_RTT.
#define kSimilarMinRttThreshold 1.125f

// If the bandwidth does not increase by the factor of |kStartupGrowthTarget|
// within |kRoundTripsWithoutGrowthBeforeExitingStartup| rounds, the connection
// will exit the STARTUP mode.
#define kStartupGrowthTarget 1.25

#define kRoundTripsWithoutGrowthBeforeExitingStartup 3

#define startup_rate_reduction_multiplier_ 0

// The cycle of gains used during the PROBE_BW stage.
static const float kPacingGain[] = {1.25, 0.75, 1, 1, 1, 1, 1, 1};

// The length of the gain cycle.
static const size_t kGainCycleLength = sizeof(kPacingGain)
                                                    / sizeof(kPacingGain[0]);

// Coefficient of target congestion window to use when basing PROBE_RTT on BDP.
#define kModerateProbeRttMultiplier 0.75

// The maximum packet size of any QUIC packet over IPv6, based on ethernet's max
// size, minus the IP and UDP headers. IPv6 has a 40 byte header, UDP adds an
// additional 8 bytes.  This is a total overhead of 48 bytes.  Ethernet's
// max packet size is 1500 bytes,  1500 - 48 = 1452.
#define kMaxV6PacketSize 1452
// The maximum packet size of any QUIC packet over IPv4.
// 1500(Ethernet) - 20(IPv4 header) - 8(UDP header) = 1472.
#define kMaxV4PacketSize 1472
// The maximum incoming packet size allowed.
#define kMaxIncomingPacketSize kMaxV4PacketSize
// The maximum outgoing packet size allowed.
#define kMaxOutgoingPacketSize kMaxV6PacketSize

// The minimum time the connection can spend in PROBE_RTT mode.
#define kProbeRttTime ms(200)

/* FLAG* are from net/quic/quic_flags_list.h */

// When in STARTUP and recovery, do not add bytes_acked to QUIC BBR's CWND in
// CalculateCongestionWindow()
#define FLAGS_quic_bbr_no_bytes_acked_in_startup_recovery 0

// When true, ensure BBR allows at least one MSS to be sent in response to an
// ACK in packet conservation.
#define FLAG_quic_bbr_one_mss_conservation 0

/* From net/quic/quic_flags_list.h */
#define kCwndGain 2.0


namespace eular {
namespace utp {

static const char *mode2str[] = {
    [BbrV1::Mode::StartUp]  = "StartUp",
    [BbrV1::Mode::Drain]    = "Drain",
    [BbrV1::Mode::ProbeBW]  = "ProbeBW",
    [BbrV1::Mode::ProbeRTT] = "ProbeRTT",
};

void BbrV1::onInit(RttStats *stats)
{
    if (stats) {
        m_rttStats = stats;
    }

    m_mode = Mode::StartUp;
    m_roundCount = 0;

    m_maxBandwidth.init(10);
    m_maxAckHeight.init(10);
    m_aggregationEpochBytes = 0;
    m_aggregationEpochStartTime = 0;
    m_minRtt = 0;
    m_minRttTimestamp = 0;
    m_initCwnd = kInitialCongestionWindow * kDefaultTCPMSS;
    m_cwnd = kInitialCongestionWindow * kDefaultTCPMSS;
    m_maxCwnd = kDefaultMaxCongestionWindowPackets * kDefaultTCPMSS;
    m_minCwnd = kDefaultMinimumCongestionWindow;
    m_highGain = kDefaultHighGain;
    m_highCwndGain = kDefaultHighGain;
    m_drainGain = 1.0f / kDefaultHighGain;
    m_pacingRate = BW_ZERO();
    m_pacingGain = 1.0f;
    m_nStartupRtts = kRoundTripsWithoutGrowthBeforeExitingStartup;
    m_flags &= ~BBR_FLAG_EXIT_STARTUP_ON_LOSS;
    m_cycleCurrentOffset = 0;
    m_lastCycleStart = 0;
    m_flags &= ~BBR_FLAG_IS_AT_FULL_BANDWIDTH;
    m_roundWoBwGain = 0;
    m_bwAtLastRound = BW_ZERO();
    m_flags &= ~BBR_FLAG_EXITING_QUIESCENCE;
    m_exitProbeRttAt = 0;
    m_flags &= ~BBR_FLAG_PROBE_RTT_ROUND_PASSED;
    m_flags &= ~BBR_FLAG_LAST_SAMPLE_APP_LIMITED;
    m_flags &= ~BBR_FLAG_HAS_NON_APP_LIMITED;
    m_flags &= ~BBR_FLAG_FLEXIBLE_APP_LIMITED;

    setStartupValues();
    UTP_LOGD("BbrV1::onInit");
}

uint64_t BbrV1::getPacingRate(int32_t inRecovery)
{
    uint64_t minRtt;
    BandWidth bw;

    if (!BW_IS_ZERO(&m_pacingRate)) {
        bw = m_pacingRate;
    } else {
        minRtt = getMinRtt();
        bw = BW_FROM_BYTES_AND_DELTA(m_initCwnd, minRtt);
        bw = BW_TIMES(&bw, m_highCwndGain);
    }

    return BW_TO_BYTES_PER_SEC(&bw);
}

uint64_t BbrV1::getCwnd()
{
    uint64_t cwnd;
    if (m_mode == Mode::ProbeRTT) {
        cwnd = getProbeRttCwnd();
    } else if (inRecovery() && !((m_flags & BBR_FLAG_RATE_BASED_STARTUP) && m_mode == Mode::StartUp)) {
        cwnd = std::min(m_cwnd, m_recoveryWindow);
    } else {
        cwnd = m_cwnd;
    }

    return cwnd;
}

void BbrV1::onBeginAck(uint64_t ackTimeUs, uint64_t inflight)
{
    assert(!(m_flags & BBR_FLAG_IN_ACK));
    m_flags |= BBR_FLAG_IN_ACK;
    m_ackState = AckState();
    m_ackState.ackTime = ackTimeUs;
    m_ackState.maxPackNo = UINT64_MAX;
    m_ackState.inflightBytes = inflight;
    m_ackState.totalBytesAckedBefore = m_bwSampler.totalAcked();
}

void BbrV1::onAck(PacketInfo *packetInfo, uint64_t nowUs, int32_t appLimited)
{
    assert(m_flags & BBR_FLAG_IN_ACK);
    BWSample sample = m_bwSampler.onPacketAcked(packetInfo, nowUs);
    if (sample.valid()) {
        m_ackState.sampleList.push_back(sample);
    }

    if (!IsValidPackNo(m_ackState.maxPackNo) || packetInfo->packetNo > m_ackState.maxPackNo) {
        m_ackState.maxPackNo = packetInfo->packetNo;
    }
    m_ackState.ackedBytes += packetInfo->packetSize;
}

void BbrV1::onLost(PacketInfo *packetInfo)
{
    m_bwSampler.onPacketLost(packetInfo);
    m_ackState.hasLosses = true;
    m_ackState.lostBytes += packetInfo->packetSize;
}

void BbrV1::onPacketSent(PacketInfo *packetInfo, uint64_t inflight, int32_t isAppLimited)
{
    m_bwSampler.onPacketSent(packetInfo, inflight);
    m_lastSentPackNo = packetInfo->packetNo;
    if (isAppLimited) {
        appLimited(inflight);
    }
}

void BbrV1::wasQuiet(uint64_t nowUs, uint64_t inflight)
{
    UNUSED(nowUs);
    UNUSED(inflight);
    UTP_LOGD("BbrV1::wasQuiet called");
}

void BbrV1::onEndAck(uint64_t inflight)
{
    bool isRoundStart;
    bool minRttExpired;
    uint64_t bytesAcked;
    uint64_t excessAcked;
    uint64_t bytesLost;

    assert(m_flags & BBR_FLAG_IN_ACK);
    m_flags &= ~BBR_FLAG_IN_ACK;

    UTP_LOGD("end_ack; mode: %s; in_flight: %"PRIu64, mode2str[m_mode], inflight);

    bytesAcked = m_bwSampler.totalAcked() - m_ackState.totalBytesAckedBefore;
    if (m_ackState.ackedBytes) {
        isRoundStart = m_ackState.maxPackNo > m_currentRoundTripEnd || !IsValidPackNo(m_currentRoundTripEnd);
        if (isRoundStart) {
            ++m_roundCount;
            m_currentRoundTripEnd = m_lastSentPackNo;
            UTP_LOGD("up round count to %"PRIu64"; new rt end: %"PRIu64, m_roundCount, m_currentRoundTripEnd);
        }

        minRttExpired = updateBandwidthAndMinRtt();
        updateRecoveryState(isRoundStart);
        excessAcked = updateAckAggregationBytes(bytesAcked);
    } else {
        isRoundStart = false;
        minRttExpired = false;
        excessAcked = 0;
    }

    if (m_mode == Mode::ProbeBW) {
        updateGainCyclePhase(inflight);
    }

    if (isRoundStart && !(m_flags & BBR_FLAG_IS_AT_FULL_BANDWIDTH)) {
        checkIsFullBwReached();
    }

    maybeExitStartupOrDrain(m_ackState.ackTime, inflight);
    maybeEnterOrExitProbeRtt(m_ackState.ackTime, isRoundStart, minRttExpired, inflight);

    bytesLost = m_ackState.lostBytes;

    calculatePacingRate();
    calculateCwnd(bytesAcked, excessAcked);
    calculateRecoveryWindow(bytesAcked, bytesLost, inflight);
}

void BbrV1::setStartupValues()
{
    m_pacingGain = m_highGain;
    m_cwndGain = m_highCwndGain;
}

uint64_t BbrV1::getMinRtt()
{
    if (m_minRtt > 0 ) {
        return m_minRtt;
    }

    uint64_t minRtt = m_rttStats->minRTT();
    if (minRtt == 0) {
        minRtt = 25000; // 25ms
    }
    return minRtt;
}

uint64_t BbrV1::getProbeRttCwnd()
{
    if (m_flags & BBR_FLAG_PROBE_RTT_BASED_ON_BDP) {
        return getTargetCwnd(kModerateProbeRttMultiplier);
    } else {
        return m_minCwnd;
    }
}

uint64_t BbrV1::getTargetCwnd(float_t gain)
{
    BandWidth bw;
    uint64_t bdp, cwnd;

    bw = BW(m_maxBandwidth.get());
    bdp = getMinRtt() * BW_TO_BYTES_PER_SEC(&bw) / 1000000;
    cwnd = static_cast<uint64_t>(gain * bdp);

    if (cwnd == 0) {
        cwnd = gain * m_initCwnd;
    }

    return std::max(cwnd, m_minCwnd);
}

bool BbrV1::inRecovery()
{
    return m_recoveryState != RecoveryState::NotInRecovery;
}

void BbrV1::appLimited(uint64_t inflight)
{
    uint64_t cwnd = getCwnd();
    if (inflight >= cwnd) {
        return;
    }

    if ((m_flags & BBR_FLAG_FLEXIBLE_APP_LIMITED) && isPipeSufficientlyFull(inflight)) {
        return;
    }

    m_flags |= BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT;
    m_bwSampler.appLimited();
    UTP_LOGD("becoming application-limited.  Last sent packet: %"PRIu64"; CWND: %"PRIu64, m_lastSentPackNo, cwnd);
}

bool BbrV1::isPipeSufficientlyFull(uint64_t inflight)
{
    if (m_mode == Mode::StartUp) {
        // STARTUP exits if it doesn't observe a 25% bandwidth increase, so
        // the CWND must be more than 25% above the target.
        return inflight >= getTargetCwnd(1.5f);
    } else if (m_pacingGain > 1) {
        // Super-unity PROBE_BW doesn't exit until 1.25 * BDP is achieved.
        return inflight >= getTargetCwnd(m_pacingGain);
    } else {
        // If bytes_in_flight are above the target congestion window, it should
        // be possible to observe the same or more bandwidth if it's available.
        return inflight >= getTargetCwnd(1.1f);
    }
}

bool BbrV1::updateBandwidthAndMinRtt()
{
    uint64_t sampleMinRtt = UINT64_MAX;
    bool minRttExpired = false;

    for (auto it = m_ackState.sampleList.begin(); it != m_ackState.sampleList.end(); ++it) {
        if (it->isAppLimited) {
            m_flags |= BBR_FLAG_LAST_SAMPLE_APP_LIMITED;
        } else {
            m_flags &= ~BBR_FLAG_LAST_SAMPLE_APP_LIMITED;
            m_flags |=  BBR_FLAG_HAS_NON_APP_LIMITED;
        }

        if (sampleMinRtt == UINT64_MAX || it->rtt < sampleMinRtt) {
            sampleMinRtt = it->rtt;
        }

        if (!it->isAppLimited || BW_VALUE(&it->bandwidth) > m_maxBandwidth.get()) {
            m_maxBandwidth.updateMax(m_roundCount, BW_VALUE(&it->bandwidth));
        }
    }

    if (sampleMinRtt == UINT64_MAX) {
        return false;
    }

    m_minRttSinceLastProbe = std::min(m_minRttSinceLastProbe, sampleMinRtt);
    minRttExpired = m_minRtt != 0 && (m_ackState.ackTime > m_minRttTimestamp + kMinRttExpiry);
    if (minRttExpired || sampleMinRtt < m_minRtt || m_minRtt == 0) {
        if (minRttExpired && shouldExtendMinRttExpiry()) {
            UTP_LOGD("min rtt expiration extended, stay at: %"PRIu64, m_minRtt);
            minRttExpired = false;
        } else {
            UTP_LOGD("min rtt updated: %"PRIu64" -> %"PRIu64, m_minRtt, sampleMinRtt);
            m_minRtt = sampleMinRtt;
        }

        m_minRttTimestamp = m_ackState.ackTime;
        m_minRttSinceLastProbe = UINT64_MAX;
        m_flags &= ~BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT;
    }

    return minRttExpired;
}

void BbrV1::updateRecoveryState(bool isRoundStart)
{
    // 当一轮没有损失时退出丢包恢复模式
    if (m_ackState.hasLosses) {
        m_endRecoveryAt = m_lastSentPackNo;
    }

    switch (m_recoveryState) {
    case NotInRecovery:
        // 丢包后进入保护
        if (m_ackState.hasLosses) {
            m_recoveryState = RecoveryState::Conservation;
            // m_recoveryWindow 在 calculateRecoveryWindow() 中设置为正确的值
            m_recoveryWindow = 0;
            // 由于保护阶段将持续一整轮, 因此将当前一轮延长, 就像现在就开始一样
            m_currentRoundTripEnd = m_lastSentPackNo;
        }
        break;
    case Conservation:
        if (isRoundStart) {
            m_recoveryState = RecoveryState::Growth;
        }
    case Growth:
        if (!m_ackState.hasLosses && m_ackState.maxPackNo > m_endRecoveryAt) {
            m_recoveryState = RecoveryState::NotInRecovery;
        }
        break;
    }
}

void BbrV1::calculatePacingRate()
{
    BandWidth bw = BW(m_maxBandwidth.get());
    if (BW_IS_ZERO(&bw)) {
        return;
    }

    UTP_LOGD("BW estimate: %"PRIu64, BW_VALUE(&bw));
    BandWidth targetRate = BW_TIMES(&bw, m_pacingGain);
    if (m_flags & BBR_FLAG_IS_AT_FULL_BANDWIDTH) {
        m_pacingRate = targetRate;
        return;
    }

    // RTT测量结果可用后, 以 初始拥塞窗口 / RTT 的速度进行
    if (BW_IS_ZERO(&m_pacingRate) && 0 != m_rttStats->minRTT()) {
        m_pacingRate  = BW_FROM_BYTES_AND_DELTA(m_initCwnd, m_rttStats->minRTT());
        return;
    }

    // 一旦检测到丢失, 在STARTUP中降低pacing rate
    bool hasEverDetectedLoss = m_endRecoveryAt != 0;
    if (hasEverDetectedLoss && (m_flags & (BBR_FLAG_SLOWER_STARTUP | BBR_FLAG_HAS_NON_APP_LIMITED)) ==
                               (BBR_FLAG_SLOWER_STARTUP | BBR_FLAG_HAS_NON_APP_LIMITED)) {
        m_pacingRate = BW_TIMES(&bw, kStartupAfterLossGain);
        return;
    }

    if (startup_rate_reduction_multiplier_ != 0 && hasEverDetectedLoss && (m_flags & BBR_FLAG_HAS_NON_APP_LIMITED)) {
        m_pacingRate = BW_TIMES(&targetRate, (1 - (m_startupBytesLost * startup_rate_reduction_multiplier_ * 1.0f / m_cwndGain)));
        // 确保pacing rate不会低于启动增长目标乘以带宽估计值。
        if (BW_VALUE(&m_pacingRate) < BW_VALUE(&bw) * kStartupGrowthTarget) {
            m_pacingRate = BW_TIMES(&bw, kStartupGrowthTarget);
        }
        return;
    }

    if (BW_VALUE(&m_pacingRate) < BW_VALUE(&targetRate)) {
        m_pacingRate = targetRate;
    }
}

void BbrV1::calculateCwnd(uint64_t bytesAcked, uint64_t excessAcked)
{
    if (m_mode == Mode::ProbeRTT) {
        return;
    }

    uint64_t targetWindow = getTargetCwnd(m_cwndGain);
    if (m_flags & BBR_FLAG_IS_AT_FULL_BANDWIDTH) {
        targetWindow += m_maxAckHeight.get();
    } else if (m_flags & BBR_FLAG_ENABLE_ACK_AGG_IN_STARTUP) {
        targetWindow += excessAcked;
    }

    bool addBytesAcked = !FLAGS_quic_bbr_no_bytes_acked_in_startup_recovery || !inRecovery();
    if (m_flags & BBR_FLAG_IS_AT_FULL_BANDWIDTH) {
        m_cwnd = std::min(targetWindow, m_cwnd + bytesAcked);
    } else if (addBytesAcked && (m_cwnd < targetWindow || m_bwSampler.totalAcked() < m_initCwnd)) {
        m_cwnd += bytesAcked;
    }

    m_cwnd = CLAMP(m_cwnd, m_minCwnd, m_maxCwnd);
}

void BbrV1::calculateRecoveryWindow(uint64_t bytesAcked, uint64_t bytestLost, uint64_t bytestInflight)
{
    if ((m_flags & BBR_FLAG_RATE_BASED_STARTUP) && m_mode == Mode::StartUp) {
        return;
    }

    if (m_recoveryState == NotInRecovery) {
        return;
    }

    // 初始化恢复窗口
    if (m_recoveryWindow == 0) {
        m_recoveryWindow = bytestInflight + bytesAcked;
        m_recoveryWindow = std::max(m_minCwnd, m_recoveryWindow);
        return;
    }

    // 丢包期间再次丢包需要减少恢复窗口大小, 最小不低于一个MTU可传输大小
    if (m_recoveryWindow >= bytestLost) {
        m_recoveryWindow -= bytestLost;
    } else {
        m_recoveryWindow = kMaxSegmentSize;
    }

    // 增长阶段增加恢复窗口大小
    if (m_recoveryState == Growth) {
        m_recoveryWindow += bytesAcked;
    }

    // 最低限检查
    m_recoveryWindow = std::max(bytestInflight + bytesAcked, m_recoveryWindow);
    if (FLAG_quic_bbr_one_mss_conservation) {
        m_recoveryWindow = std::max(m_recoveryWindow, bytestInflight + kMaxSegmentSize);
    }

    m_recoveryWindow = std::max(m_minCwnd, m_recoveryWindow);
}

uint64_t BbrV1::updateAckAggregationBytes(uint64_t bytesAcked)
{
    uint64_t ackTime = m_ackState.ackTime;

    // 根据估值带宽, 计算预计将传输多少字节
    uint64_t expectedBytesAcked = m_maxBandwidth.get() * (ackTime - m_aggregationEpochStartTime);

    // 一旦ack到达率小于或等于最大带宽, 就重置当前聚合时间。
    if (m_aggregationEpochBytes <= expectedBytesAcked) {
        m_aggregationEpochStartTime = ackTime;
        m_aggregationEpochBytes = bytesAcked;
        return 0;
    }

    m_aggregationEpochBytes += bytesAcked;
    uint64_t diff = m_aggregationEpochBytes - expectedBytesAcked;
    m_maxAckHeight.updateMax(m_roundCount, diff);
    return diff;
}

void BbrV1::updateGainCyclePhase(uint64_t bytestInflight)
{
    uint64_t priorInflight = m_ackState.inflightBytes;
    uint64_t now = m_ackState.ackTime;

    bool shouldAdvanceGainCycling = (now - m_lastCycleStart) >= getMinRtt();

    if (m_pacingGain > 1.0 && !m_ackState.hasLosses && priorInflight < getTargetCwnd(m_pacingGain)) {
        shouldAdvanceGainCycling = false;
    }

    if (m_pacingGain < 1.0 && bytestInflight <= getTargetCwnd(1.0f)) {
        shouldAdvanceGainCycling = true;
    }

    if (shouldAdvanceGainCycling) {
        m_cycleCurrentOffset = (m_cycleCurrentOffset + 1) % kGainCycleLength;
        m_lastCycleStart = now;

        if ((m_flags & BBR_FLAG_DRAIN_TO_TARGET) && m_pacingGain < 1.0f &&
             kPacingGain[m_cycleCurrentOffset] == 1.0f && bytestInflight > getTargetCwnd(1.0f)) {
            return;
        }

        m_pacingGain = kPacingGain[m_cycleCurrentOffset];
        UTP_LOGD("advanced gain cycle, pacing gain set to %.2f", m_pacingGain);
    }
}

void BbrV1::checkIsFullBwReached()
{
    BandWidth target; // 当前轮次希望看到的 "增长目标带宽"
    BandWidth bw; // 当前估计的最大带宽
    if (m_flags & BBR_FLAG_LAST_SAMPLE_APP_LIMITED) {
        UTP_LOGD("last sample app limited: full BW not reached");
        return;
    }

    // 此次带宽希望较上次增加1.25倍
    target = BW_TIMES(&m_bwAtLastRound, kStartupGrowthTarget);
    // 获取当前观测到最大的带宽
    bw = BW(m_maxBandwidth.get());
    if (BW_VALUE(&bw) >= BW_VALUE(&target)) { // 带宽存在提升
        m_bwAtLastRound = bw;
        m_roundWoBwGain = 0;
        if (m_flags & BBR_FLAG_EXPIRE_ACK_AGG_IN_STARTUP) {
            m_maxAckHeight.reset(MinMaxSample{m_roundCount, 0});
        }

        UTP_LOGD("bandwidth increased to %"PRIu64"bytes/sec: full BW not reached", BW_TO_BYTES_PER_SEC(&bw));
        return;
    }

    // 带宽未增长
    ++m_roundWoBwGain;
    UTP_LOGD("no bandwidth growth this round (%"PRIu32"/%"PRIu32")", m_roundWoBwGain, m_nStartupRtts);
    if (m_roundWoBwGain >= m_nStartupRtts || ((m_flags & BBR_FLAG_EXIT_STARTUP_ON_LOSS) && inRecovery())) {
        assert(m_flags & BBR_FLAG_HAS_NON_APP_LIMITED);
        m_flags |= BBR_FLAG_IS_AT_FULL_BANDWIDTH;
        UTP_LOGD("no bandwidth growth for %"PRIu32" rounds: full BW reached", m_nStartupRtts);
    } else {
        UTP_LOGD("rounds w/o gain: %u, full BW not reached", m_roundWoBwGain);
    }
}

void BbrV1::maybeExitStartupOrDrain(uint64_t now, uint64_t bytestInflight)
{
    uint64_t targetCwnd;
    if (m_mode == Mode::StartUp && (m_flags & BBR_FLAG_IS_AT_FULL_BANDWIDTH)) {
        onExitStartup(now);
        setMode(Mode::Drain);
        m_pacingGain = m_drainGain;
        m_cwndGain = m_highCwndGain;
    }

    if (m_mode == Mode::Drain) {
        targetCwnd = getTargetCwnd(1.0f);
        UTP_LOGD("bytes in flight: %"PRIu64"; target cwnd: %"PRIu64, bytestInflight, targetCwnd);
        if (bytestInflight <= targetCwnd) {
            enterProbeBWMode(now);
        }
    }
}

void BbrV1::maybeEnterOrExitProbeRtt(uint64_t now, bool isRoundStart, bool minRttExpired, uint64_t bytestInflight)
{
    if (minRttExpired && !(m_flags & BBR_FLAG_EXITING_QUIESCENCE) && m_mode != Mode::ProbeRTT) {
        if (isSlowStart()) {
            onExitStartup(now);
        }
        setMode(Mode::ProbeRTT);
        m_pacingGain = 1.0f;
        // 在bytestInflight达到目标小值之前, 不要决定退出ProbeRTT的时间
        m_exitProbeRttAt = 0;
    }

    if (m_mode == Mode::ProbeRTT) {
        m_bwSampler.appLimited();
        UTP_LOGD("exit probe at: %"PRIu64"; now: %"PRIu64"; round start: %d; round passed: %d; rtt: %"PRIu64" usec",
            m_exitProbeRttAt, now, isRoundStart, !!(m_flags & BBR_FLAG_PROBE_RTT_ROUND_PASSED), m_rttStats->minRTT());
        
        if (m_exitProbeRttAt == 0) {
            // 途中未确认字节数已小于目标值, 计划退出ProbeRTT
            if (bytestInflight < getProbeRttCwnd() + kMaxOutgoingPacketSize) {
                m_flags &= ~BBR_FLAG_PROBE_RTT_ROUND_PASSED;
                m_exitProbeRttAt = now + kProbeRttTime;
                UTP_LOGD("exit time set to %"PRIu64, m_exitProbeRttAt);
            }
        } else {
            if (isRoundStart) {
                m_flags |= BBR_FLAG_PROBE_RTT_ROUND_PASSED;
            }
            if ((now >= m_exitProbeRttAt) && (m_flags & BBR_FLAG_PROBE_RTT_ROUND_PASSED)) {
                m_minRttTimestamp = now;
                if (!(m_flags & BBR_FLAG_IS_AT_FULL_BANDWIDTH)) {
                    enterStartupMode(now);
                } else {
                    enterProbeBWMode(now);
                }
            }
        }
    }

    m_flags &= ~BBR_FLAG_EXITING_QUIESCENCE;
}

bool BbrV1::shouldExtendMinRttExpiry()
{
    bool increasedSinceLastProbe;
    if ((m_flags & (BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT | BBR_FLAG_PROBE_RTT_DISABLED_IF_APP_LIMITED)) == 
        (BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT | BBR_FLAG_PROBE_RTT_DISABLED_IF_APP_LIMITED)) {
        // 自上次 Probe RTT 以后我们一直是 app-limited, 且策略是 app-limited 时就禁用 Probe RTT,
        // 延长当前 min_rtt 的有效期
        return true;
    }

    increasedSinceLastProbe = m_minRttSinceLastProbe > m_minRtt * kSimilarMinRttThreshold;
    if ((m_flags & (BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT | BBR_FLAG_PROBE_RTT_SKIPPED_IF_SIMILAR_RTT)) == 
        (BBR_FLAG_APP_LIMITED_SINCE_LAST_PROBE_RTT | BBR_FLAG_PROBE_RTT_SKIPPED_IF_SIMILAR_RTT) &&
        !increasedSinceLastProbe) {
        // 如果最近是 app-limited, 并且在这期间测到的 RTT 依然与当前 min_rtt 相差不大
        // 那就认为当前 min_rtt 还算靠谱, 延长它的有效期, 不进入新的 Probe RTT
        return true;
    }

    return false;
}

void BbrV1::onExitStartup(uint64_t now)
{
    UNUSED(now);
    assert(m_mode == Mode::StartUp);
    // Apparently this method is just to update stats, something that we don't do yet.
}

void BbrV1::setMode(Mode newMode)
{
    if (m_mode == newMode) {
        UTP_LOGD("mode remains %s", mode2str[newMode]);
        return;
    }

    UTP_LOGD("mode change %s -> %s", mode2str[m_mode], mode2str[newMode]);
    m_mode = newMode;
}

void BbrV1::enterProbeBWMode(uint64_t now)
{
    setMode(Mode::ProbeBW);
    m_cwndGain = kCwndGain;

    // 使用当前时间作为随机种子
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, std::numeric_limits<uint8_t>::max());
    uint8_t randomValue = dist(gen);

    m_cycleCurrentOffset = randomValue % (kGainCycleLength - 1);
    // 跳过1是因为0.75是减小, 当前轮次无法做到后续在增大
    if (m_cycleCurrentOffset >= 1) {
        ++m_cycleCurrentOffset;
    }
    m_lastCycleStart = now;
    m_pacingGain = kPacingGain[m_cycleCurrentOffset];
}

void BbrV1::enterStartupMode(uint64_t now)
{
    UNUSED(now);
    setMode(Mode::StartUp);
    setStartupValues();
}

bool BbrV1::isSlowStart()
{
    return m_mode == Mode::StartUp;
}
} // namespace utp
} // namespace eular
