/*************************************************************************
    > File Name: mtu.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 08:04:16 PM CST
 ************************************************************************/

#include "mtu/mtu.h"

#include <algorithm>
#include <array>

#include "utp/config.h"

namespace {

constexpr std::array<uint16_t, 4> kProbeLadderMtu = {{1380, 1450, 1492, 1500}};

}

namespace eular {
namespace utp {

namespace {

template <typename T>
T ClampValue(T value, T minValue, T maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

} // namespace

void MtuDiscovery::init(const Config *config, Address::Family family)
{
    m_enabled = (config == nullptr) ? true : config->enable_dplpmtud;

    const uint16_t cfgMin = (config == nullptr) ? ETHERNET_MTU_MIN : config->mtu_min;
    const uint16_t cfgMax = (config == nullptr) ? UTP_ETHERNET_MTU : config->mtu_max;
    const uint16_t cfgBase = (config == nullptr) ? ETHERNET_MTU_MID : config->mtu_base;

    m_mtuMin = ClampValue<uint16_t>(cfgMin, ETHERNET_MTU_MIN, UTP_ETHERNET_MTU);
    m_mtuMax = ClampValue<uint16_t>(cfgMax, m_mtuMin, UTP_ETHERNET_MTU);
    m_currentMtu = ClampValue<uint16_t>(cfgBase, m_mtuMin, m_mtuMax);
    m_ceilingMtu = m_mtuMax;
    m_searchLowMtu = m_currentMtu;
    m_searchHighMtu = m_ceilingMtu;
    m_probePhase = kProbePhaseLadder;

    m_probeStep = (config == nullptr || config->mtu_probe_step == 0) ? 16 : config->mtu_probe_step;
    m_probeTimeoutMs = (config == nullptr || config->mtu_probe_timeout == 0)
                         ? 2000
                         : config->mtu_probe_timeout;
    m_blackholeLossThreshold = (config == nullptr || config->mtu_blackhole_loss_threshold == 0)
                             ? 3
                             : config->mtu_blackhole_loss_threshold;
    m_blackholeLossWindowMs = (config == nullptr || config->mtu_blackhole_loss_window_ms == 0)
                            ? 3000
                            : config->mtu_blackhole_loss_window_ms;
    m_blackholeCooldownMs = (config == nullptr || config->mtu_blackhole_cooldown_ms == 0)
                          ? 5000
                          : config->mtu_blackhole_cooldown_ms;

    const uint32_t cfgIntervalSec = (config == nullptr) ? 300 : config->mtu_probe_interval;
    m_probeIntervalMs = std::max<uint32_t>(1000, cfgIntervalSec * 1000);

    m_nextProbeTimeMs = 0;
    m_lastLargeAckMs = 0;
    m_lastLargeLossMs = 0;
    m_blackholeCooldownUntilMs = 0;
    m_largeLossStreak = 0;
    m_hasInFlightProbe = false;
    m_inFlightProbePackNo = 0;
    m_inFlightProbeMtu = 0;
    m_inFlightProbeDeadlineMs = 0;

    setAddressFamily(family);
}

void MtuDiscovery::setAddressFamily(Address::Family family)
{
    if (family == Address::IPv6) {
        m_family = Address::IPv6;
    } else if (family == Address::IPv4) {
        m_family = Address::IPv4;
    }
}

uint16_t MtuDiscovery::nextProbeMtu() const
{
    if (!m_enabled || m_currentMtu >= m_ceilingMtu) {
        return m_currentMtu;
    }

    uint16_t candidate = m_currentMtu;
    if (m_probePhase == kProbePhaseLadder) {
        candidate = nextLadderTarget();
        if (candidate > m_currentMtu) {
            return candidate;
        }

        // 梯队走完后进入二分精修
        candidate = nextBinaryTarget();
        if (candidate > m_currentMtu) {
            return candidate;
        }
        return m_currentMtu;
    }

    if (m_probePhase == kProbePhaseBinary) {
        candidate = nextBinaryTarget();
        if (candidate > m_currentMtu) {
            return candidate;
        }
    }

    return m_currentMtu;
}

uint16_t MtuDiscovery::currentMaxPacketSize() const
{
    return PacketSizeFromMtu(m_currentMtu, m_family);
}

uint16_t MtuDiscovery::absoluteMaxPacketSize() const
{
    return PacketSizeFromMtu(m_ceilingMtu, m_family);
}

bool MtuDiscovery::shouldProbe(utp_time_t nowMs) const
{
    if (!m_enabled || m_hasInFlightProbe) {
        return false;
    }
    if (nowMs < m_blackholeCooldownUntilMs) {
        return false;
    }
    if (nowMs < m_nextProbeTimeMs) {
        return false;
    }
    return nextProbeMtu() > m_currentMtu;
}

bool MtuDiscovery::onProbeSent(utp_packno_t packNo, uint16_t probeMtu, utp_time_t nowMs)
{
    if (!m_enabled || packNo == 0) {
        return false;
    }

    const uint16_t clampedProbe = ClampValue<uint16_t>(probeMtu,
                                                        static_cast<uint16_t>(m_currentMtu + 1),
                                                        m_ceilingMtu);
    if (clampedProbe <= m_currentMtu) {
        return false;
    }

    m_hasInFlightProbe = true;

    const uint16_t ladderTarget = nextLadderTarget();
    if (m_probePhase == kProbePhaseLadder && (ladderTarget == m_currentMtu || clampedProbe != ladderTarget)) {
        m_probePhase = kProbePhaseBinary;
    }

    m_inFlightProbePackNo = packNo;
    m_inFlightProbeMtu = clampedProbe;
    m_inFlightProbeDeadlineMs = nowMs + m_probeTimeoutMs;
    return true;
}

bool MtuDiscovery::onProbeAck(utp_packno_t packNo, utp_time_t nowMs)
{
    if (!m_hasInFlightProbe || packNo != m_inFlightProbePackNo) {
        return false;
    }

    m_currentMtu = std::max(m_currentMtu, m_inFlightProbeMtu);
    m_searchLowMtu = std::max<uint16_t>(m_searchLowMtu, m_currentMtu);
    m_lastLargeAckMs = nowMs;
    m_largeLossStreak = 0;
    clearInFlightProbe();

    if (m_currentMtu >= m_mtuMax) {
        m_currentMtu = m_mtuMax;
        m_ceilingMtu = m_mtuMax;
        m_searchLowMtu = m_currentMtu;
        m_searchHighMtu = m_currentMtu;
        m_probePhase = kProbePhaseStable;
        m_nextProbeTimeMs = nowMs + m_probeIntervalMs;
    } else if (m_probePhase == kProbePhaseBinary
            && m_searchHighMtu <= static_cast<uint16_t>(m_searchLowMtu + m_probeStep)) {
        m_currentMtu = m_searchLowMtu;
        m_ceilingMtu = m_searchLowMtu;
        m_probePhase = kProbePhaseStable;
        m_nextProbeTimeMs = nowMs + m_probeIntervalMs;
    } else {
        m_nextProbeTimeMs = nowMs + 1;
    }

    return true;
}

bool MtuDiscovery::onProbeLost(utp_packno_t packNo, utp_time_t nowMs)
{
    if (!m_hasInFlightProbe || packNo != m_inFlightProbePackNo) {
        return false;
    }

    if (m_inFlightProbeMtu > 0) {
        const uint16_t newHigh = static_cast<uint16_t>(m_inFlightProbeMtu - 1);
        m_searchHighMtu = std::min<uint16_t>(m_searchHighMtu, newHigh);
        m_ceilingMtu = std::min<uint16_t>(m_ceilingMtu, m_searchHighMtu);
        m_probePhase = kProbePhaseBinary;
    }

    clearInFlightProbe();

    if (m_searchHighMtu <= static_cast<uint16_t>(m_searchLowMtu + m_probeStep)) {
        m_currentMtu = m_searchLowMtu;
        m_ceilingMtu = m_searchLowMtu;
        m_probePhase = kProbePhaseStable;
        m_nextProbeTimeMs = nowMs + m_probeIntervalMs;
    } else {
        m_nextProbeTimeMs = nowMs + 1;
    }

    return true;
}

bool MtuDiscovery::onProbeTimeout(utp_time_t nowMs)
{
    if (!m_hasInFlightProbe || nowMs < m_inFlightProbeDeadlineMs) {
        return false;
    }
    return onProbeLost(m_inFlightProbePackNo, nowMs);
}

bool MtuDiscovery::onDataPacketAck(uint16_t packetSize, utp_time_t nowMs)
{
    if (!m_enabled) {
        return false;
    }

    if (packetSize + m_probeStep < currentMaxPacketSize()) {
        return false;
    }

    m_lastLargeAckMs = nowMs;
    m_largeLossStreak = 0;
    return true;
}

bool MtuDiscovery::onDataPacketLoss(uint16_t packetSize, utp_time_t nowMs)
{
    if (!m_enabled) {
        return false;
    }

    if (packetSize + m_probeStep < currentMaxPacketSize()) {
        return false;
    }

    if (m_lastLargeLossMs == 0 || nowMs > m_lastLargeLossMs + m_blackholeLossWindowMs) {
        m_largeLossStreak = 0;
    }
    m_lastLargeLossMs = nowMs;

    if (m_largeLossStreak < UINT8_MAX) {
        ++m_largeLossStreak;
    }

    if (m_largeLossStreak < m_blackholeLossThreshold) {
        return false;
    }

    // 若近期仍有大包成功 ACK，优先认为是短时拥塞，不触发黑洞回退。
    if (m_lastLargeAckMs != 0 && nowMs <= m_lastLargeAckMs + static_cast<utp_time_t>(m_probeTimeoutMs * 2)) {
        return false;
    }

    m_currentMtu = safetyMtu();
    m_ceilingMtu = m_mtuMax;
    m_probePhase = kProbePhaseLadder;
    resetSearchWindow();
    clearInFlightProbe();
    m_blackholeCooldownUntilMs = nowMs + m_blackholeCooldownMs;
    m_nextProbeTimeMs = m_blackholeCooldownUntilMs;
    m_largeLossStreak = 0;
    return true;
}

uint16_t MtuDiscovery::PacketSizeFromMtu(uint16_t mtu, Address::Family family)
{
    uint16_t ipHeader = (family == Address::IPv6) ? IPV6_HEADER_SIZE : IPV4_HEADER_SIZE;
    uint16_t overhead = static_cast<uint16_t>(ipHeader + UDP_HEADER_SIZE);
    if (mtu <= overhead) {
        return 0;
    }
    return static_cast<uint16_t>(mtu - overhead);
}

uint16_t MtuDiscovery::MtuFromPacketSize(uint16_t packetSize, Address::Family family)
{
    uint16_t ipHeader = (family == Address::IPv6) ? IPV6_HEADER_SIZE : IPV4_HEADER_SIZE;
    return static_cast<uint16_t>(packetSize + ipHeader + UDP_HEADER_SIZE);
}

void MtuDiscovery::clearInFlightProbe()
{
    m_hasInFlightProbe = false;
    m_inFlightProbePackNo = 0;
    m_inFlightProbeMtu = 0;
    m_inFlightProbeDeadlineMs = 0;
}

void MtuDiscovery::resetSearchWindow()
{
    m_searchLowMtu = m_currentMtu;
    m_searchHighMtu = m_ceilingMtu;
}

uint16_t MtuDiscovery::nextLadderTarget() const
{
    for (uint16_t ladder : kProbeLadderMtu) {
        if (ladder <= m_currentMtu) {
            continue;
        }
        if (ladder > m_ceilingMtu) {
            continue;
        }
        if (ladder > m_mtuMax) {
            continue;
        }
        if (ladder < m_mtuMin) {
            continue;
        }
        return ladder;
    }

    return m_currentMtu;
}

uint16_t MtuDiscovery::nextBinaryTarget() const
{
    if (m_searchHighMtu <= m_searchLowMtu + m_probeStep) {
        return m_currentMtu;
    }

    const uint16_t low = static_cast<uint16_t>(m_searchLowMtu + 1);
    const uint16_t high = m_searchHighMtu;
    if (low >= high) {
        return m_currentMtu;
    }

    const uint16_t mid = static_cast<uint16_t>(low + (high - low) / 2);
    if (mid <= m_currentMtu) {
        return m_currentMtu;
    }
    return ClampValue<uint16_t>(mid, low, high);
}

uint16_t MtuDiscovery::safetyMtu() const
{
    return ETHERNET_MTU_MIN;
}

} // namespace utp
} // namespace eular
