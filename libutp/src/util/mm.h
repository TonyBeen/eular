/*************************************************************************
    > File Name: mm.h
    > Author: eular
    > Brief:
    > Created Time: Wed 21 Jan 2026 03:14:25 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_MM_H__
#define __UTP_UTIL_MM_H__

#include <array>

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
        uint32_t    objs_total;
        uint32_t    objs_inuse;
    };

    MemoryManager();
    ~MemoryManager();

public:
    struct {
        MaloCacheLine<StreamImpl>       stream_malo;
    };
    
    MaloCacheLine<PacketOut>        packet_out_malo;
    SLIST_HEAD(, PacketOutBuf)      packet_out_bufs[MM_OUT_BUCKETS];
    PoolStats                       packet_out_stats[MM_OUT_BUCKETS];
    SLIST_HEAD(, PacketInBuf)       packet_in_bufs[MM_IN_BUCKETS];
    PoolStats                       packet_in_stats[MM_IN_BUCKETS];
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_MM_H__
