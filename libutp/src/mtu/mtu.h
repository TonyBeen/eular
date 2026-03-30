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
    void onPathValidated(utp_time_t nowMs);
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
    bool onDataPacketAck(uint16_t packetSize, utp_time_t nowMs);
    bool onDataPacketLoss(uint16_t packetSize, utp_time_t nowMs);

    static uint16_t PacketSizeFromMtu(uint16_t mtu, Address::Family family);
    static uint16_t MtuFromPacketSize(uint16_t packetSize, Address::Family family);

private:
    enum ProbePhase : uint8_t {
        kProbePhaseLadder = 0,
        kProbePhaseBinary,
        kProbePhaseStable,
    };

private:
    void clearInFlightProbe();
    void resetSearchWindow();
    uint16_t nextLadderTarget() const;
    uint16_t nextBinaryTarget() const;
    uint16_t safetyMtu() const;

private:
    bool            m_enabled{true};
    Address::Family m_family{Address::IPv4};

    uint16_t        m_mtuMin{ETHERNET_MTU_MIN};
    uint16_t        m_mtuMax{UTP_ETHERNET_MTU};
    uint16_t        m_mtuBase{ETHERNET_MTU_MID};
    uint16_t        m_probeStep{16}; // 二分收敛阈值，不再用于线性累加

    uint32_t        m_probeIntervalMs{300000};
    uint16_t        m_probeTimeoutMs{2000};

    uint8_t         m_blackholeLossThreshold{3};      // 黑洞判定：连续大包丢失阈值(次数)
    uint16_t        m_blackholeLossWindowMs{3000};    // 黑洞判定：连续丢失统计窗口(ms)
    uint16_t        m_blackholeCooldownMs{5000};      // 黑洞回退后冷静期(ms)

    uint16_t        m_currentMtu{ETHERNET_MTU_MID};
    uint16_t        m_ceilingMtu{UTP_ETHERNET_MTU};
    uint16_t        m_searchLowMtu{ETHERNET_MTU_MID};  // 二分下界（已确认可达）
    uint16_t        m_searchHighMtu{UTP_ETHERNET_MTU}; // 二分上界（疑似不可达+1）
    ProbePhase      m_probePhase{kProbePhaseLadder};   // 探测阶段：梯队/二分/稳定
    utp_time_t      m_nextProbeTimeMs{0};

    utp_time_t      m_lastLargeAckMs{0};               // 最近一次“大包” ACK 时间
    utp_time_t      m_lastLargeLossMs{0};              // 最近一次“大包”丢包时间
    utp_time_t      m_blackholeCooldownUntilMs{0};     // 黑洞回退后冷静期截止时间
    uint8_t         m_largeLossStreak{0};              // 大包连续丢失计数

    bool            m_hasInFlightProbe{false};
    utp_packno_t    m_inFlightProbePackNo{0};
    uint16_t        m_inFlightProbeMtu{0};
    utp_time_t      m_inFlightProbeDeadlineMs{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_MTU_MTU_H__
