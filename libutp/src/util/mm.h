/*************************************************************************
    > File Name: mm.h
    > Author: eular
    > Brief:
    > Created Time: Wed 21 Jan 2026 03:14:25 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_MM_H__
#define __UTP_UTIL_MM_H__

#include <array>

#include "context/stream_impl.h"
#include "proto/packet_in.h"
#include "util/malo.hpp"
#include "proto/packet_out.h"
#include "queue.h"

#define MM_OUT_BUCKETS 5
#define MM_IN_BUCKETS 3

namespace eular {
namespace utp {
class MemoryManager
{
public:
    struct PoolStats {
        uint32_t    calls;
        uint32_t    inuse_max;
        uint32_t    inuse_max_avg;  // 指数加权移动平均 (EWMA)
        uint32_t    inuse_max_var;  // 绝对偏差
        uint32_t    objs_total;     // Number of objects owned by the pool
        uint32_t    objs_inuse;     // Number of objects in use
    };

    MemoryManager();
    ~MemoryManager();

    PacketOut*  getPacketOut(uint32_t size);
    PacketIn*   getPacketIn(uint32_t size);
    RecvFragment* getRecvFragment();
    void        putPacketOut(PacketOut *pkt);
    void        putPacketIn(PacketIn *pkt);
    void        putRecvFragment(RecvFragment *fragment);
    void        retainPacketIn(PacketIn *pkt);
    void        releasePacketIn(PacketIn *pkt);

private:
    void poolStatsAllocated(PoolStats *stats, uint32_t allocated);
    void poolStatsFree(PoolStats *stats);
    void poolSampleMax(PoolStats *stats);
    bool hasNewSample(PoolStats *stats);
    void maybeShrinkPoolOut(uint32_t idx);
    void maybeShrinkPoolIn(uint32_t idx);

public:
    MaloCacheLine<StreamImpl>   stream_malo;
    MaloCacheLine<PacketIn>     packet_in_malo;
    MaloCacheLine<PacketOut>    packet_out_malo;
    MaloCacheLine<RecvFragment> recv_fragment_malo;

    SLIST_HEAD(, PacketOutBuf)      packet_out_bufs[MM_OUT_BUCKETS];
    PoolStats                       packet_out_stats[MM_OUT_BUCKETS];
    SLIST_HEAD(, PacketInBuf)       packet_in_bufs[MM_IN_BUCKETS];
    PoolStats                       packet_in_stats[MM_IN_BUCKETS];
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_MM_H__
