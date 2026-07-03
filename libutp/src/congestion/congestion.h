/*************************************************************************
    > File Name: congestion.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:22:53 PM CST
 ************************************************************************/

#ifndef __CONGESTION_CONGESTION_H__
#define __CONGESTION_CONGESTION_H__

#include <stdint.h>

#include <memory>

#include <utp/platform.h>
#include "congestion/rtt.h"

namespace eular {
namespace utp {
struct PacketInfo {
    uint64_t    packetNo;
    uint64_t    sendTimeUs;
    uint32_t    packetSize;
    void*       packetState{nullptr}; // bw_sampler.h BWPacketState
};

class Congestion {
public:
    using SP  = std::shared_ptr<Congestion>;
    using WP  = std::weak_ptr<Congestion>;
    using Ptr = std::unique_ptr<Congestion>;

    Congestion() = default;
    virtual ~Congestion() = default;

    virtual void        onInit(RttStats *stats) = 0;
    virtual uint64_t    getPacingRate(int32_t inRecovery) = 0;
    virtual uint64_t    getCwnd() = 0;
    virtual void        onBeginAck(uint64_t ackTimeUs, uint64_t inFlight) = 0;
    virtual void        onAck(PacketInfo *packetInfo, uint64_t nowUs, int32_t appLimited) = 0;
    virtual void        onLost(PacketInfo *packetInfo) = 0;
    virtual void        onPacketSent(PacketInfo *packetInfo, uint64_t inFlight, int32_t appLimited) = 0;
    virtual void        wasQuiet(uint64_t nowUs, uint64_t inFlight) = 0;
    virtual void        onEndAck(uint64_t inFlight) = 0;
    virtual void        onLoss() {}
    virtual void        onTimeout() {}
};

} // namespace utp
} // namespace utp

#endif // __CONGESTION_CONGESTION_H__
