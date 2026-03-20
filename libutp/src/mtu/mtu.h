/*************************************************************************
    > File Name: mtu.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 08:04:13 PM CST
 ************************************************************************/

#ifndef __UTP_MTU_MTU_H__
#define __UTP_MTU_MTU_H__

#include <stdint.h>

#include "socket/address.h"
#include "utp/types.h"

// 即只上探到 1500 字节的 MTU
#define UTP_ETHERNET_MTU    1500
#define IPV4_HEADER_SIZE    20
#define IPV6_HEADER_SIZE    40
#define UDP_HEADER_SIZE     8

#define ETHERNET_MTU_MIN    1280 // IPv6 minimum MTU
#define ETHERNET_MTU_MID    1400 // middle MTU

namespace eular {
namespace utp {

class Config;

// DPLPMTUD-like probe controller for one connection path.
class MtuDiscovery {
public:
    MtuDiscovery() = default;

    void init(const Config *config, Address::Family family);
    void setAddressFamily(Address::Family family);

    bool enabled() const { return m_enabled; }
    bool hasInFlightProbe() const { return m_hasInFlightProbe; }

    uint16_t pathMtu() const { return m_currentMtu; }
    uint16_t nextProbeMtu() const;

    uint16_t currentMaxPacketSize() const;
    uint16_t absoluteMaxPacketSize() const;

    bool shouldProbe(utp_time_t nowMs) const;
    bool onProbeSent(utp_packno_t packNo, uint16_t probeMtu, utp_time_t nowMs);
    bool onProbeAck(utp_packno_t packNo, utp_time_t nowMs);
    bool onProbeLost(utp_packno_t packNo, utp_time_t nowMs);
    bool onProbeTimeout(utp_time_t nowMs);

    static uint16_t PacketSizeFromMtu(uint16_t mtu, Address::Family family);
    static uint16_t MtuFromPacketSize(uint16_t packetSize, Address::Family family);

private:
    void clearInFlightProbe();

private:
    bool            m_enabled{true};
    Address::Family m_family{Address::IPv4};

    uint16_t        m_mtuMin{ETHERNET_MTU_MIN};
    uint16_t        m_mtuMax{UTP_ETHERNET_MTU};
    uint16_t        m_probeStep{16};

    uint32_t        m_probeIntervalMs{300000};
    uint16_t        m_probeTimeoutMs{2000};

    uint16_t        m_currentMtu{ETHERNET_MTU_MID};
    uint16_t        m_ceilingMtu{UTP_ETHERNET_MTU};
    utp_time_t      m_nextProbeTimeMs{0};

    bool            m_hasInFlightProbe{false};
    utp_packno_t    m_inFlightProbePackNo{0};
    uint16_t        m_inFlightProbeMtu{0};
    utp_time_t      m_inFlightProbeDeadlineMs{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_MTU_MTU_H__
