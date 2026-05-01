/*************************************************************************
    > File Name: mm.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 21 Jan 2026 03:14:28 PM CST
 ************************************************************************/

#include "util/mm.h"
#include "mtu/mtu.h"
#include "proto/proto.h"
#include "logger/logger.h"
#include "mm.h"

#include <cstdlib>
#include <cstring>

#define POOL_SAMPLE_PERIOD 1024

namespace eular {
namespace utp {

struct PacketInBuf {
    SLIST_ENTRY(PacketInBuf)  next_pib;
};

struct PacketOutBuf {
    SLIST_ENTRY(PacketOutBuf) next_pob;
};

enum {
    PACKET_OUT_PAYLOAD_0 = ETHERNET_MTU_MIN - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - UTP_HEADER_SIZE,
    PACKET_OUT_PAYLOAD_1 = ETHERNET_MTU_MID - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - UTP_HEADER_SIZE,
    PACKET_OUT_PAYLOAD_2 = UTP_ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - UTP_HEADER_SIZE,
    PACKET_OUT_PAYLOAD_3 = 4096,
    PACKET_OUT_PAYLOAD_4 = 0xffff,
};

static constexpr uint16_t g_packetOutSizeVec[] = {
    PACKET_OUT_PAYLOAD_0,
    PACKET_OUT_PAYLOAD_1,
    PACKET_OUT_PAYLOAD_2,
    PACKET_OUT_PAYLOAD_3,
    PACKET_OUT_PAYLOAD_4,
};

enum {
    PACKET_IN_SIZE_0 = ETHERNET_MTU_MIN,
    PACKET_IN_SIZE_1 = UTP_ETHERNET_MTU,
    PACKET_IN_SIZE_2 = 0xffff,
};

static constexpr uint16_t g_packetInSizeVec[] = {
    PACKET_IN_SIZE_0,
    PACKET_IN_SIZE_1,
    PACKET_IN_SIZE_2,
};

static int32_t PacketOutIndex(uint32_t size)
{
    uint32_t idx = (size > PACKET_OUT_PAYLOAD_0)
                + (size > PACKET_OUT_PAYLOAD_1)
                + (size > PACKET_OUT_PAYLOAD_2)
                + (size > PACKET_OUT_PAYLOAD_3);
    return idx;
}

static int32_t PacketInIndex(uint32_t size)
{
    uint32_t idx = (size > PACKET_IN_SIZE_0)
                + (size > PACKET_IN_SIZE_1);
    return idx;
}

MemoryManager::MemoryManager()
{
    for (uint32_t i = 0; i < MM_OUT_BUCKETS; ++i) {
        SLIST_INIT(&packet_out_bufs[i]);
        std::memset(&packet_out_stats[i], 0, sizeof(packet_out_stats[i]));
    }

    for (uint32_t i = 0; i < MM_IN_BUCKETS; ++i) {
        SLIST_INIT(&packet_in_bufs[i]);
        std::memset(&packet_in_stats[i], 0, sizeof(packet_in_stats[i]));
    }
}

MemoryManager::~MemoryManager()
{
    for (uint32_t i = 0; i < MM_OUT_BUCKETS; ++i) {
        PacketOutBuf *pob = nullptr;
        while ((pob = SLIST_FIRST(&packet_out_bufs[i])) != nullptr) {
            SLIST_REMOVE_HEAD(&packet_out_bufs[i], next_pob);
            std::free(pob);
        }
    }

    for (uint32_t i = 0; i < MM_IN_BUCKETS; ++i) {
        PacketInBuf *pib = nullptr;
        while ((pib = SLIST_FIRST(&packet_in_bufs[i])) != nullptr) {
            SLIST_REMOVE_HEAD(&packet_in_bufs[i], next_pib);
            std::free(pib);
        }
    }
}

PacketOut *MemoryManager::getPacketOut(uint32_t size)
{
    PacketOut *packetOut = packet_out_malo.get();
    if (!packetOut) {
        return nullptr;
    }

    uint32_t idx = PacketOutIndex(size);
    PacketOutBuf *pob = SLIST_FIRST(&packet_out_bufs[idx]);
    if (pob) {
        SLIST_REMOVE_HEAD(&packet_out_bufs[idx], next_pob);
        poolStatsAllocated(&packet_out_stats[idx], 0);
    } else {
        pob = (PacketOutBuf *)malloc(g_packetOutSizeVec[idx]);
        if (!pob) {
            packet_out_malo.put(packetOut);
            return nullptr;
        }
        poolStatsAllocated(&packet_out_stats[idx], 1);
    }

    if (hasNewSample(&packet_out_stats[idx])) {
        maybeShrinkPoolOut(idx);
    }

    memset(packetOut, 0, sizeof(PacketOut));
    packetOut->alloc_size = g_packetOutSizeVec[idx];
    packetOut->raw_data = (uint8_t *)pob;
    return packetOut;
}

void MemoryManager::putPacketOut(PacketOut *pkt)
{
    if (pkt == nullptr || pkt->raw_data == nullptr || pkt->alloc_size == 0) {
        return;
    }

    pkt->clearSendAttempts();
    if (pkt->encrypt_data != nullptr && pkt->encrypt_data != pkt->raw_data) {
        std::free(pkt->encrypt_data);
        pkt->encrypt_data = nullptr;
        pkt->encrypt_data_size = 0;
    }

    PacketOutBuf *pob = (PacketOutBuf *)pkt->raw_data;
    uint32_t idx = PacketOutIndex(pkt->alloc_size);

    // Make putPacketOut idempotent in release builds. If upper layer
    // accidentally releases the same PacketOut twice, the second call will
    // short-circuit on null raw_data/alloc_size instead of duplicating pool nodes.
    pkt->raw_data = nullptr;
    pkt->alloc_size = 0;

    SLIST_INSERT_HEAD(&packet_out_bufs[idx], pob, next_pob);
    poolStatsFree(&packet_out_stats[idx]);
    if (hasNewSample(&packet_out_stats[idx])) {
        maybeShrinkPoolOut(idx);
    }
    packet_out_malo.put(pkt);
}

PacketIn *MemoryManager::getPacketIn(uint32_t size)
{
    PacketIn *packetIn = packet_in_malo.get();
    if (!packetIn) {
        return nullptr;
    }

    const uint32_t idx = PacketInIndex(size);
    PacketInBuf *pib = SLIST_FIRST(&packet_in_bufs[idx]);
    if (pib) {
        SLIST_REMOVE_HEAD(&packet_in_bufs[idx], next_pib);
        poolStatsAllocated(&packet_in_stats[idx], 0);
    } else {
        pib = static_cast<PacketInBuf *>(std::malloc(g_packetInSizeVec[idx]));
        if (!pib) {
            packet_in_malo.put(packetIn);
            return nullptr;
        }
        poolStatsAllocated(&packet_in_stats[idx], 1);
    }

    if (hasNewSample(&packet_in_stats[idx])) {
        maybeShrinkPoolIn(idx);
    }

    std::memset(packetIn, 0, sizeof(PacketIn));
    packetIn->refcnt = 1;
    packetIn->alloc_size = g_packetInSizeVec[idx];
    packetIn->raw_data = reinterpret_cast<const uint8_t *>(pib);
    return packetIn;
}

RecvFragment *MemoryManager::getRecvFragment()
{
    RecvFragment *fragment = recv_fragment_malo.get();
    if (!fragment) {
        return nullptr;
    }

    std::memset(fragment, 0, sizeof(RecvFragment));
    return fragment;
}

void MemoryManager::putPacketIn(PacketIn *pkt)
{
    if (pkt == nullptr || pkt->raw_data == nullptr || pkt->alloc_size == 0) {
        return;
    }

    PacketInBuf *pib = (PacketInBuf *) pkt->raw_data;
    const uint32_t idx = PacketInIndex(pkt->alloc_size);

    // Make putPacketIn idempotent in release builds. If upper layer
    // accidentally releases the same PacketIn twice, the second call will
    // short-circuit on null raw_data/alloc_size and avoid double-free later.
    pkt->raw_data = nullptr;
    pkt->alloc_size = 0;
    pkt->refcnt = 0;
    pkt->raw_size = 0;
    pkt->payload = nullptr;
    pkt->payload_size = 0;
    pkt->frame_types = 0;

    SLIST_INSERT_HEAD(&packet_in_bufs[idx], pib, next_pib);
    poolStatsFree(&packet_in_stats[idx]);
    if (hasNewSample(&packet_in_stats[idx])) {
        maybeShrinkPoolIn(idx);
    }
    packet_in_malo.put(pkt);
}

void MemoryManager::putRecvFragment(RecvFragment *fragment)
{
    if (fragment == nullptr) {
        return;
    }

    recv_fragment_malo.put(fragment);
}

void MemoryManager::retainPacketIn(PacketIn *pkt)
{
    if (pkt == nullptr) {
        return;
    }

    ++pkt->refcnt;
}

void MemoryManager::releasePacketIn(PacketIn *pkt)
{
    if (pkt == nullptr || pkt->refcnt == 0) {
        return;
    }

    --pkt->refcnt;
    if (pkt->refcnt == 0) {
        putPacketIn(pkt);
    }
}

void MemoryManager::poolStatsAllocated(PoolStats *stats, uint32_t allocated)
{
    ++stats->calls;
    stats->objs_inuse += 1;
    stats->objs_total += allocated;
    if (stats->objs_inuse > stats->inuse_max) {
        stats->inuse_max = stats->objs_inuse;
    }

    if (0 == stats->calls % POOL_SAMPLE_PERIOD) {
        poolSampleMax(stats);
    }
}

void MemoryManager::poolStatsFree(PoolStats *stats)
{
    --stats->objs_inuse;
    ++stats->calls;
    if (0 == stats->calls % POOL_SAMPLE_PERIOD) {
        poolSampleMax(stats);
    }
}

void MemoryManager::poolSampleMax(PoolStats *stats)
{
#define ALPHA_SHIFT 3
#define BETA_SHIFT  2
    uint32_t diff;

    if (stats->inuse_max_avg) {
        stats->inuse_max_var -= stats->inuse_max_var >> BETA_SHIFT;
        if (stats->inuse_max_avg > stats->inuse_max)
            diff = stats->inuse_max_avg - stats->inuse_max;
        else
            diff = stats->inuse_max - stats->inuse_max_avg;
        stats->inuse_max_var += diff >> BETA_SHIFT;
        stats->inuse_max_avg -= stats->inuse_max_avg >> ALPHA_SHIFT;
        stats->inuse_max_avg += stats->inuse_max >> ALPHA_SHIFT;
    } else {
        /* First measurement */
        stats->inuse_max_avg  = stats->inuse_max;
        stats->inuse_max_var  = stats->inuse_max / 2;
    }

    stats->calls = 0;
    stats->inuse_max = stats->objs_inuse;
#if defined(UTP_LOG_POOL_STATS)
    UTP_LOGI("pool stats: calls=%u, inuse=%u, total=%u, inuse_max=%u, inuse_max_avg=%u, inuse_max_var=%u",
             stats->calls, stats->objs_inuse, stats->objs_total,
             stats->inuse_max, stats->inuse_max_avg, stats->inuse_max_var);
#endif // defined(UTP_LOG_POOL_STATS)
}

bool MemoryManager::hasNewSample(PoolStats *stats)
{
    return 0 == stats->calls;
}

/// @brief 平均最大值低于分配的所有对象的四分之一, 则释放一半已分配的对象
void MemoryManager::maybeShrinkPoolOut(uint32_t idx)
{
    PoolStats *stats = &packet_out_stats[idx];
    PacketOutBuf *pob;
    uint32_t shrink = 0;
    if (stats->inuse_max_avg * 4 < stats->objs_total) {
        shrink = stats->objs_total / 2;
        while (stats->objs_total > shrink && (pob = SLIST_FIRST(&packet_out_bufs[idx]))) {
            SLIST_REMOVE_HEAD(&packet_out_bufs[idx], next_pob);
            free(pob);
            --stats->objs_total;
        }
#if defined(UTP_LOG_POOL_STATS)
        UTP_LOGI("pool #%u; max avg %u; shrank from %u to %u objs",
                idx, stats->inuse_max_avg, shrink * 2, stats->objs_total);
#endif
    } else {
#if defined(UTP_LOG_POOL_STATS)
        UTP_LOGI("pool #%u; max avg %u; objs: %u; won't shrink",
                idx, stats->inuse_max_avg, stats->objs_total);
#endif
    }
}

void MemoryManager::maybeShrinkPoolIn(uint32_t idx)
{
    PoolStats *stats = &packet_in_stats[idx];
    PacketInBuf *pib;
    uint32_t shrink = 0;
    if (stats->inuse_max_avg * 4 < stats->objs_total) {
        shrink = stats->objs_total / 2;
        while (stats->objs_total > shrink && (pib = SLIST_FIRST(&packet_in_bufs[idx]))) {
            SLIST_REMOVE_HEAD(&packet_in_bufs[idx], next_pib);
            std::free(pib);
            --stats->objs_total;
        }
#if defined(UTP_LOG_POOL_STATS)
        UTP_LOGI("packet in pool #%u; max avg %u; shrank from %u to %u objs",
                idx, stats->inuse_max_avg, shrink * 2, stats->objs_total);
#endif
    } else {
#if defined(UTP_LOG_POOL_STATS)
        UTP_LOGI("packet in pool #%u; max avg %u; objs: %u; won't shrink",
                idx, stats->inuse_max_avg, stats->objs_total);
#endif
    }
}
} // namespace utp
} // namespace eular
