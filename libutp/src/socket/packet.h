/*************************************************************************
    > File Name: packet.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 08:10:13 PM CST
 ************************************************************************/

#ifndef __UTP_SOCKET_PACKET_H__
#define __UTP_SOCKET_PACKET_H__

#include "socket/address.h"

namespace eular {
namespace utp {

struct PacketMetaInfo {
    int32_t         fd;
    Address         localAddress;
    Address         peerAddress;
};


} // namespace utp
} // namespace eular

#endif // __UTP_SOCKET_PACKET_H__
