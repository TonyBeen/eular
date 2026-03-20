/*************************************************************************
    > File Name: cubic.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:45 PM CST
 ************************************************************************/

#include "congestion/cubic.h"

#include <algorithm>

namespace eular {
namespace utp {

Cubic::Cubic() = default;

void Cubic::onInit(RttStats *stats)
{
    m_rttStats = stats;
    m_cwnd = kInitCwnd;
    m_ssthresh = kMaxCwnd;
    m_ackedBytes = 0;
}

uint64_t Cubic::getPacingRate(int32_t inRecovery)
{
    (void)inRecovery;

    uint64_t srttUs = 25000;
    if (m_rttStats != nullptr && m_rttStats->srtt() > 0) {
        // RttStats::srtt() stores value with alpha scale (x8).
        srttUs = std::max<uint64_t>(1000, m_rttStats->srtt() >> 3);
    }

    return (m_cwnd * 1000000ULL) / srttUs;
}

uint64_t Cubic::getCwnd()
{
    return m_cwnd;
}

void Cubic::onBeginAck(uint64_t ackTimeUs, uint64_t inFlight)
{
    (void)ackTimeUs;
    (void)inFlight;
    m_ackedBytes = 0;
}

void Cubic::onAck(PacketInfo *packetInfo, uint64_t nowUs, int32_t appLimited)
{
    (void)nowUs;
    (void)appLimited;
    if (packetInfo == nullptr) {
        return;
    }

    m_ackedBytes += packetInfo->packetSize;

    if (m_cwnd < m_ssthresh) {
        // Slow start.
        m_cwnd = std::min<uint64_t>(m_cwnd + packetInfo->packetSize, kMaxCwnd);
        return;
    }

    // Reno-compatible increase: cwnd += MSS * acked / cwnd.
    const uint64_t inc = std::max<uint64_t>(1, (kDefaultMss * m_ackedBytes) / std::max<uint64_t>(m_cwnd, 1));
    m_cwnd = std::min<uint64_t>(m_cwnd + inc, kMaxCwnd);
    m_ackedBytes = 0;
}

void Cubic::onLost(PacketInfo *packetInfo)
{
    (void)packetInfo;
    m_cwnd = std::max<uint64_t>((m_cwnd * 7) / 10, kMinCwnd);
    m_ssthresh = m_cwnd;
    m_ackedBytes = 0;
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
    (void)inFlight;
}

void Cubic::onEndAck(uint64_t inFlight)
{
    (void)inFlight;
}

void Cubic::onTimeout()
{
    m_ssthresh = std::max<uint64_t>(m_cwnd / 2, kMinCwnd);
    m_cwnd = kMinCwnd;
    m_ackedBytes = 0;
}

} // namespace utp
} // namespace eular
