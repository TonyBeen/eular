/*************************************************************************
    > File Name: cubic.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:43 PM CST
 ************************************************************************/

#ifndef __UTP_CONGESTION_CUBIC_H__
#define __UTP_CONGESTION_CUBIC_H__

#include "congestion/congestion.h"
#include "utp/config.h"

namespace eular {
namespace utp {
class Cubic : public Congestion
{
public:
    explicit Cubic(const Config *cfg = nullptr);
    ~Cubic() override = default;

    void        onInit(RttStats *stats) override;
    uint64_t    getPacingRate(int32_t inRecovery) override;
    uint64_t    getCwnd() override;
    void        onBeginAck(uint64_t ackTimeUs, uint64_t inFlight) override;
    void        onAck(PacketInfo *packetInfo, uint64_t nowUs, int32_t appLimited) override;
    void        onLost(PacketInfo *packetInfo) override;
    void        onPacketSent(PacketInfo *packetInfo, uint64_t inFlight, int32_t appLimited) override;
    void        wasQuiet(uint64_t nowUs, uint64_t inFlight) override;
    void        onEndAck(uint64_t inFlight) override;
    void        onTimeout() override;

private:
    const uint64_t kDefaultMss = 1460;
    const uint64_t kMaxCwnd = 2000 * kDefaultMss;
    const uint64_t kDefaultMinCwnd = 4 * kDefaultMss;
    const uint64_t kDefaultInitCwnd = 32 * kDefaultMss;
    const double kDefaultBeta = 0.7;
    const double kDefaultCubicC = 0.4;

    RttStats    *m_rttStats{nullptr};
    uint64_t    m_cwnd{kDefaultInitCwnd};
    uint64_t    m_ssthresh{kMaxCwnd};
    uint64_t    m_ackedBytes{0};
    uint64_t    m_epochStartUs{0};
    uint64_t    m_lastMaxCwnd{0};
    uint64_t    m_originPointCwnd{0};
    double      m_k{0.0};
    double      m_beta{kDefaultBeta};
    double      m_cubicC{kDefaultCubicC};
    uint64_t    m_initCwnd{kDefaultInitCwnd};
    uint64_t    m_minCwnd{kDefaultMinCwnd};

    uint64_t    smoothedRttUs() const;
    void        resetEpoch();
    void        ensureEpoch(uint64_t nowUs);
    uint64_t    cubicTargetCwnd(uint64_t nowUs) const;
    uint64_t    cubicIncrement(uint64_t ackedBytes, uint64_t nowUs) const;
    uint64_t    renoIncrement(uint64_t ackedBytes) const;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONGESTION_CUBIC_H__
