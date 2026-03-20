/*************************************************************************
    > File Name: cubic.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:43 PM CST
 ************************************************************************/

#ifndef __UTP_CONGESTION_CUBIC_H__
#define __UTP_CONGESTION_CUBIC_H__

#include "congestion/congestion.h"

namespace eular {
namespace utp {
class Cubic : public Congestion
{
public:
    Cubic();
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
    static constexpr uint64_t kDefaultMss = 1460;
    static constexpr uint64_t kMinCwnd = 4 * kDefaultMss;
    static constexpr uint64_t kInitCwnd = 32 * kDefaultMss;
    static constexpr uint64_t kMaxCwnd = 2000 * kDefaultMss;

    RttStats    *m_rttStats{nullptr};
    uint64_t    m_cwnd{kInitCwnd};
    uint64_t    m_ssthresh{kMaxCwnd};
    uint64_t    m_ackedBytes{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONGESTION_CUBIC_H__
