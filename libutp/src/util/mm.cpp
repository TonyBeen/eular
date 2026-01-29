/*************************************************************************
    > File Name: mm.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 21 Jan 2026 03:14:28 PM CST
 ************************************************************************/

#include "util/mm.h"
#include "mtu/mtu.h"
#include "proto/proto.h"
#include "mm.h"

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

MemoryManager::MemoryManager()
{
}

MemoryManager::~MemoryManager()
{
}

} // namespace utp
} // namespace eular
