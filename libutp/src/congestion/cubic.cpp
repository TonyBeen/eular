/*************************************************************************
    > File Name: cubic.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:45 PM CST
 ************************************************************************/

#include "congestion/cubic.h"

#include <algorithm>
#include <cmath>

namespace eular {
namespace utp {

Cubic::Cubic(const Config *cfg)
{
    if (cfg == nullptr) {
        return;
    }

    if (cfg->cubic_beta > 0.0 && cfg->cubic_beta < 1.0) {
        m_beta = cfg->cubic_beta;
    }
    if (cfg->cubic_c > 0.0 && cfg->cubic_c <= 2.0) {
        m_cubicC = cfg->cubic_c;
    }

    const uint64_t minCwnd = std::max<uint64_t>(1, cfg->cubic_min_cwnd_mss) * kDefaultMss;
    const uint64_t initCwnd = std::max<uint64_t>(1, cfg->cubic_init_cwnd_mss) * kDefaultMss;
    m_minCwnd = std::min<uint64_t>(minCwnd, kMaxCwnd);
    m_initCwnd = std::min<uint64_t>(std::max<uint64_t>(initCwnd, m_minCwnd), kMaxCwnd);
    m_cwnd = m_initCwnd;
}

void Cubic::onInit(RttStats *stats)
{
    m_rttStats = stats;
    m_cwnd = m_initCwnd;
    m_ssthresh = kMaxCwnd;
    m_ackedBytes = 0;
    m_epochStartUs = 0;
    m_lastMaxCwnd = 0;
    m_originPointCwnd = 0;
    m_k = 0.0;
}

uint64_t Cubic::getPacingRate(int32_t inRecovery)
{
    const uint64_t srttUs = smoothedRttUs();
    const uint64_t baseRate = (m_cwnd * 1000000ULL) / std::max<uint64_t>(srttUs, 1);
    // Recovery phase uses conservative pacing gain to avoid burst amplification.
    const uint64_t gainPct = (inRecovery != 0) ? 100 : 125;
    return (baseRate * gainPct) / 100;
}

uint64_t Cubic::getCwnd()
{
    return m_cwnd;
}

void Cubic::onBeginAck(uint64_t ackTimeUs, uint64_t inFlight)
{
    (void)ackTimeUs;
    (void)inFlight;
}

void Cubic::onAck(PacketInfo *packetInfo, uint64_t nowUs, int32_t appLimited)
{
    (void)appLimited;
    if (packetInfo == nullptr) {
        return;
    }

    const uint64_t acked = std::max<uint64_t>(packetInfo->packetSize, 1);
    m_ackedBytes += acked;

    if (m_cwnd < m_ssthresh) {
        // Slow start.
        m_cwnd = std::min<uint64_t>(m_cwnd + acked, kMaxCwnd);
        return;
    }

    ensureEpoch(nowUs);

    const uint64_t cubicInc = cubicIncrement(acked, nowUs);
    const uint64_t renoInc = renoIncrement(acked);
    const uint64_t inc = std::max<uint64_t>(cubicInc, renoInc);
    m_cwnd = std::min<uint64_t>(m_cwnd + inc, kMaxCwnd);
}

void Cubic::onLost(PacketInfo *packetInfo)
{
    (void)packetInfo;

    if (m_cwnd < m_lastMaxCwnd) {
        m_lastMaxCwnd = static_cast<uint64_t>((m_cwnd * (2.0 - m_beta)) / 2.0);
    } else {
        m_lastMaxCwnd = m_cwnd;
    }

    m_cwnd = std::max<uint64_t>(static_cast<uint64_t>(m_cwnd * m_beta), m_minCwnd);
    m_ssthresh = m_cwnd;
    resetEpoch();
}

void Cubic::onPacketSent(PacketInfo *packetInfo, uint64_t inFlight, int32_t appLimited)
{
    (void)packetInfo;
    (void)inFlight;
    (void)appLimited;
}

void Cubic::wasQuiet(uint64_t nowUs, uint64_t inFlight)
{
    (void)nowUs;
    if (inFlight == 0) {
        resetEpoch();
    }
}

void Cubic::onEndAck(uint64_t inFlight)
{
    (void)inFlight;
}

void Cubic::onTimeout()
{
    m_lastMaxCwnd = m_cwnd;
    m_ssthresh = std::max<uint64_t>(static_cast<uint64_t>(m_cwnd * m_beta), m_minCwnd);
    m_cwnd = m_minCwnd;
    resetEpoch();
}

uint64_t Cubic::smoothedRttUs() const
{
    uint64_t srttUs = 25000;
    if (m_rttStats != nullptr && m_rttStats->srtt() > 0) {
        // RttStats::srtt() stores value with alpha scale (x8).
        srttUs = std::max<uint64_t>(1000, m_rttStats->srtt() >> 3);
    }
    return srttUs;
}

void Cubic::resetEpoch()
{
    m_epochStartUs = 0;
    m_originPointCwnd = 0;
    m_k = 0.0;
    m_ackedBytes = 0;
}

void Cubic::ensureEpoch(uint64_t nowUs)
{
    if (m_epochStartUs != 0) {
        return;
    }

    m_epochStartUs = nowUs;
    if (m_lastMaxCwnd > m_cwnd) {
        m_originPointCwnd = m_lastMaxCwnd;
        const double wMaxPackets = static_cast<double>(m_lastMaxCwnd) / static_cast<double>(kDefaultMss);
        const double cwndPackets = static_cast<double>(m_cwnd) / static_cast<double>(kDefaultMss);
        const double deltaPackets = std::max(0.0, wMaxPackets - cwndPackets);
        m_k = std::cbrt(deltaPackets / m_cubicC);
    } else {
        m_originPointCwnd = m_cwnd;
        m_k = 0.0;
    }
}

uint64_t Cubic::cubicTargetCwnd(uint64_t nowUs) const
{
    if (m_epochStartUs == 0) {
        return m_cwnd;
    }

    const double t = (static_cast<double>(nowUs - m_epochStartUs) / 1000000.0)
                   + (static_cast<double>(smoothedRttUs()) / 1000000.0);
    const double dt = t - m_k;
    const double originPackets = static_cast<double>(m_originPointCwnd) / static_cast<double>(kDefaultMss);
    const double targetPackets = std::max(0.0, m_cubicC * dt * dt * dt + originPackets);
    const double targetBytes = targetPackets * static_cast<double>(kDefaultMss);
    return std::max<uint64_t>(m_minCwnd, static_cast<uint64_t>(targetBytes));
}

uint64_t Cubic::cubicIncrement(uint64_t ackedBytes, uint64_t nowUs) const
{
    if (ackedBytes == 0) {
        return 0;
    }

    const uint64_t target = cubicTargetCwnd(nowUs);
    const uint64_t cwnd = std::max<uint64_t>(m_cwnd, 1);
    if (target <= cwnd) {
        // Near/after plateau: keep very small growth to avoid stalling forever.
        return std::max<uint64_t>(1, (ackedBytes * kDefaultMss) / (cwnd * 100));
    }

    const uint64_t distance = target - cwnd;
    return std::max<uint64_t>(1, (ackedBytes * distance) / cwnd);
}

uint64_t Cubic::renoIncrement(uint64_t ackedBytes) const
{
    if (ackedBytes == 0) {
        return 0;
    }
    return std::max<uint64_t>(1, (ackedBytes * kDefaultMss) / std::max<uint64_t>(m_cwnd, 1));
}

} // namespace utp
} // namespace eular
