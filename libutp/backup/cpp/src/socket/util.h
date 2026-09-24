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
#include "util/status.h"

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
        static Status   SetBindInterface(socket_t sockfd, const char *ifname);
        static Status   SetRecvError(socket_t sockfd, int32_t family, bool recverr = true);
        static Status   SetSendBufferSize(socket_t sockfd, int32_t size);
        static Status   SetRecvBufferSize(socket_t sockfd, int32_t size);
        static Status   SetPktInfoV4(socket_t sockfd, bool on = true);
        static Status   SetPktInfoV6(socket_t sockfd, bool on = true);
        static Status   SetIPv6Only(socket_t sockfd);
        static Status   SetIPTos(socket_t sockfd);
        static Status   SetNoSigPipe(socket_t sockfd);
        static int32_t  GetMtuByIfname(socket_t sockfd, const char *ifname, Status &status);
    };

    class Util
    {
    public:
        static Address  GetIPPktInfo(const msghdr_t &msg, uint16_t port);
    };


    static socket_t     Open(int32_t family, Status &status);
    static Status       Bind(socket_t sockfd, const Address &addr);
    static void         Close(socket_t sockfd);
};

} // namespace utp
} // namespace eular

#endif // __UTP_SOCKET_UTIL_H__
