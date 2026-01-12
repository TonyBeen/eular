/*************************************************************************
    > File Name: util.h
    > Author: eular
    > Brief:
    > Created Time: Thu 08 Jan 2026 05:05:19 PM CST
 ************************************************************************/

#ifndef __UTP_SOCKET_UTIL_H__
#define __UTP_SOCKET_UTIL_H__

#include <stdint.h>

#include "commom.h"
#include "socket/address.h"

namespace eular {
namespace utp {
class Socket
{
public:
    class Ioctl {
    public:
        static int32_t  SetNonBlock(socket_t sockfd, bool nonblock = true);
        static int32_t  SetReuseAddr(socket_t sockfd, bool reuse = true);
        static int32_t  SetReusePort(socket_t sockfd, bool reuse = true);
        static int32_t  SetDontFragment(socket_t sockfd, bool df = true);
        static int32_t  SetBindInterface(socket_t sockfd, const char *ifname);
        static int32_t  SetRecvError(socket_t sockfd, int32_t family, bool recverr = true);
        static int32_t  SetSendBufferSize(socket_t sockfd, int32_t size);
        static int32_t  SetRecvBufferSize(socket_t sockfd, int32_t size);
        static int32_t  SetPktInfoV4(socket_t sockfd, bool on = true);
        static int32_t  SetPktInfoV6(socket_t sockfd, bool on = true);
        static int32_t  SetIPv6Only(socket_t sockfd);
        static int32_t  SetIPTos(socket_t sockfd);
        static int32_t  SetNoSigPipe(socket_t sockfd);
        static int32_t  GetMtuByIfname(socket_t sockfd, const char *ifname);
    };

    class Util
    {
    public:
        static Address  GetIPPktInfo(const msghdr &msg, uint16_t port);
    };


    static socket_t     Open(int32_t family);
    static int32_t      Bind(socket_t sockfd, const Address &addr);
    static void         Close(socket_t sockfd);
};

} // namespace utp
} // namespace eular

#endif // __UTP_SOCKET_UTIL_H__
