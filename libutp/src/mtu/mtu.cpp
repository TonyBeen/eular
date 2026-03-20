/*************************************************************************
    > File Name: mtu.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 08:04:16 PM CST
 ************************************************************************/

#include "mtu/mtu.h"

#include <algorithm>

#include "utp/config.h"

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

    m_probeStep = (config == nullptr || config->mtu_probe_step == 0) ? 16 : config->mtu_probe_step;
    m_probeTimeoutMs = (config == nullptr || config->mtu_probe_timeout == 0)
                         ? 2000
                         : config->mtu_probe_timeout;

    const uint32_t cfgIntervalSec = (config == nullptr) ? 300 : config->mtu_probe_interval;
    m_probeIntervalMs = std::max<uint32_t>(1000, cfgIntervalSec * 1000);

    m_nextProbeTimeMs = 0;
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

    uint32_t candidate = static_cast<uint32_t>(m_currentMtu) + m_probeStep;
    if (candidate > m_ceilingMtu) {
        candidate = m_ceilingMtu;
    }

    if (candidate <= m_currentMtu) {
        return m_currentMtu;
    }

    return static_cast<uint16_t>(candidate);
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

    const uint16_t clampedProbe = ClampValue<uint16_t>(probeMtu, m_currentMtu, m_ceilingMtu);
    if (clampedProbe <= m_currentMtu) {
        return false;
    }

    m_hasInFlightProbe = true;
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
    clearInFlightProbe();

    if (m_currentMtu >= m_mtuMax) {
        m_currentMtu = m_mtuMax;
        m_ceilingMtu = m_mtuMax;
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

    if (m_inFlightProbeMtu > m_probeStep) {
        uint16_t newCeiling = static_cast<uint16_t>(m_inFlightProbeMtu - m_probeStep);
        if (newCeiling < m_currentMtu) {
            newCeiling = m_currentMtu;
        }
        m_ceilingMtu = std::min(m_ceilingMtu, newCeiling);
    }

    clearInFlightProbe();
    m_nextProbeTimeMs = nowMs + m_probeIntervalMs;
    return true;
}

bool MtuDiscovery::onProbeTimeout(utp_time_t nowMs)
{
    if (!m_hasInFlightProbe || nowMs < m_inFlightProbeDeadlineMs) {
        return false;
    }
    return onProbeLost(m_inFlightProbePackNo, nowMs);
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

} // namespace utp
} // namespace eular
