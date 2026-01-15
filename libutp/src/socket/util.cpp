/*************************************************************************
    > File Name: util.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 08 Jan 2026 05:05:22 PM CST
 ************************************************************************/

#include "socket/util.h"
#include "util/error.h"

namespace eular {
namespace utp {
int32_t Socket::Ioctl::SetNonBlock(socket_t sockfd, bool nonblock)
{
#ifdef OS_WINDOWS
    u_long flag = 1;
    return ioctlsocket(sockfd, FIONBIO, &flag);
#else
    int32_t flag = fcntl(sockfd, F_GETFL, 0);
    return fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);
#endif
}

int32_t Socket::Ioctl::SetReuseAddr(socket_t sockfd, bool reuse)
{
#ifdef OS_WINDOWS
    BOOL flag = reuse ? 1 : 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&flag, sizeof(flag));
#else
    int32_t flag = reuse ? 1 : 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
#endif
}

int32_t Socket::Ioctl::SetReusePort(socket_t sockfd, bool reuse)
{
#ifdef OS_WINDOWS
    BOOL flag = reuse ? 1 : 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&flag, sizeof(flag));
#else
    int32_t flag = reuse ? 1 : 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
#endif
}

int32_t Socket::Ioctl::SetDontFragment(socket_t sockfd, bool df)
{
#ifdef OS_WINDOWS
    BOOL flag = df ? 1 : 0;
    return setsockopt(sockfd, IPPROTO_IP, IP_DONTFRAGMENT, (const char *)&flag, sizeof(flag));
#else
    int32_t flag = df ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    return setsockopt(sockfd, IPPROTO_IP, IP_MTU_DISCOVER, &flag, sizeof(flag));
#endif
}

int32_t Socket::Ioctl::SetBindInterface(socket_t sockfd, const char *ifname)
{
    if (ifname == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "ifname is nullptr");
        return -1;
    }

#if defined(OS_LINUX)
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
        auto status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "bind interface '{}' failed: [{}, {}]", ifname, status, GetSystemErrnoMsg(status));
        return -1;
    }

    return 0;
#else
    return 0;
#endif

}

int32_t Socket::Ioctl::SetRecvError(socket_t sockfd, int32_t family, bool recverr)
{
    int32_t status = 0;
#if defined(OS_LINUX)
    int32_t flag = recverr ? 1 : 0;
    if (family == AF_INET) {
        if (setsockopt(sockfd, IPPROTO_IP, IP_RECVERR, &flag, sizeof(flag)) < 0) {
            int32_t code = GetSystemLastError();
            SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IP, IP_RECVERR, {}) failed: [{}, {}].",
                sockfd, recverr, code, GetSystemErrnoMsg(code));
            status = -1;
        }
    } else if (family == AF_INET6) {
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_RECVERR, &flag, sizeof(flag)) < 0) {
            int32_t code = GetSystemLastError();
            SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IPV6, IPV6_RECVERR, {}) failed: [{}, {}].",
                sockfd, recverr, code, GetSystemErrnoMsg(code));
            status = -1;
        }
    } else {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "invalid family: {}", family);
        status = -1;
    }
#else
    UNUSED(sockfd);
    UNUSED(family);
    UNUSED(recverr);
#endif
    return status;
}

int32_t Socket::Ioctl::SetSendBufferSize(socket_t sockfd, int32_t size)
{
    int32_t status = ::setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof(size));
    if (status != 0) {
        int32_t code = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, SOL_SOCKET, SO_SNDBUF, {}) failed: [{}, {}].",
                sockfd, size, code, GetSystemErrnoMsg(code));
        return -1;
    }

    return 0;
}

int32_t Socket::Ioctl::SetRecvBufferSize(socket_t sockfd, int32_t size)
{
    int32_t status = ::setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size));
    if (status != 0) {
        int32_t code = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, SOL_SOCKET, SO_RCVBUF, {}) failed: [{}, {}].",
                sockfd, size, code, GetSystemErrnoMsg(code));
        return -1;
    }

    return 0;
}

int32_t Socket::Ioctl::SetPktInfoV4(socket_t sockfd, bool on)
{
#if defined(OS_LINUX) || defined(OS_WINDOWS)
    int32_t enable = on ? 1 : 0;
    int32_t status = ::setsockopt(sockfd, IPPROTO_IP, IP_PKTINFO, (const char *)&enable, sizeof(enable));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IP, IP_PKTINFO, {}) failed: [{}, {}].",
                sockfd, on, status, GetSystemErrnoMsg(status));
        return -1;
    }
#elif defined(OS_APPLE)
    int32_t enable = on ? 1 : 0;
    int32_t status = ::setsockopt(sockfd, IPPROTO_IP, IP_RECVDSTADDR, &enable, sizeof(enable));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IP, IP_RECVDSTADDR, {}) failed: [{}, {}].",
                sockfd, on, status, GetSystemErrnoMsg(status));
        return -1;
    }
#else
    UNUSED(sockfd);
    UNUSED(on);
#endif
    return 0;
}

int32_t Socket::Ioctl::SetPktInfoV6(socket_t sockfd, bool on)
{
#if defined(OS_LINUX) || defined(OS_APPLE)
    int32_t enable = on ? 1 : 0;
    int32_t status = ::setsockopt(sockfd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &enable, sizeof(enable));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IPV6, IPV6_RECVPKTINFO, {}) failed: [{}, {}].",
                sockfd, on, status, GetSystemErrnoMsg(status));
        return -1;
    }
#elif defined(OS_WINDOWS)
    DWORD enable = on ? 1 : 0;
    int32_t status = ::setsockopt(sockfd, IPPROTO_IPV6, IPV6_PKTINFO, (const char*)&enable, sizeof(enable));
    if (status == SOCKET_ERROR) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IPV6, IPV6_PKTINFO, {}) failed: [{}, {}].",
                sockfd, on, status, GetSystemErrnoMsg(status));
        return -1;
    }
    return 0;
#endif
}

int32_t Socket::Ioctl::SetIPv6Only(socket_t sockfd)
{
#if defined(OS_LINUX)
    int32_t enable = 1;
    int32_t status = ::setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IPV6, IPV6_V6ONLY, 1) failed: [{}, {}].",
                sockfd, status, GetSystemErrnoMsg(status));
        return -1;
    }

#else
    UNUSED(sockfd);
#endif

    return 0;
}

int32_t Socket::Ioctl::SetIPTos(socket_t sockfd)
{
#if defined(OS_LINUX)
    int32_t tos = 0xA0; // DSCP CS5(0xA0)
    int32_t status = ::setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IP, IP_TOS, 0x20) failed: [{}, {}].",
                sockfd, status, GetSystemErrnoMsg(status));
        return -1;
    }
#elif defined(OS_APPLE)
    int32_t tos = NET_SERVICE_TYPE_VO;
    int32_t status = ::setsockopt(sockfd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &tos, sizeof(tos));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, IPPROTO_IP, IP_TOS, NET_SERVICE_TYPE_VO) failed: [{}, {}].",
                sockfd, status, GetSystemErrnoMsg(status));
        return -1;
    }
#else
    UNUSED(sockfd);
#endif
    return 0;
}

int32_t Socket::Ioctl::SetNoSigPipe(socket_t sockfd)
{
#if defined(OS_APPLE) && defined(SO_NOSIGPIPE)
    int32_t flag = 1;
    int32_t status = ::setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
    if (status != 0) {
        status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "setsockopt({}, SOL_SOCKET, SO_NOSIGPIPE, 1) failed: [{}, {}].",
                sockfd, status, GetSystemErrnoMsg(status));
        return -1;
    }
    return 0;
#else
    UNUSED(sockfd);
    return 0;
#endif
}

int32_t Socket::Ioctl::GetMtuByIfname(socket_t sockfd, const char *ifname)
{
    if (ifname == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "ifname is nullptr");
        return -1;
    }

#if defined(OS_LINUX)
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(sockfd, SIOCGIFMTU, &ifr) < 0) {
        auto status = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "ioctl SIOCGIFMTU for interface '{}' failed: [{}, {}]", ifname, status, GetSystemErrnoMsg(status));
        return -1;
    }

    return ifr.ifr_mtu;
#elif defined(OS_APPLE)
    struct ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0) {
        int err = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "getifaddrs('{}') failed: [{}, {}]", ifname, err, GetSystemErrnoMsg(err));
        return -1;
    }

    int32_t mtu = -1;
    for (struct ifaddrs *it = ifs; it != nullptr; it = it->ifa_next) {
        if (it->ifa_name == nullptr) continue;
        if (strcmp(it->ifa_name, ifname) != 0) continue;

        if (it->ifa_data != nullptr) {
            struct if_data *ifd = reinterpret_cast<struct if_data*>(it->ifa_data);
            mtu = static_cast<int>(ifd->ifi_mtu);
            break;
        }
    }

    freeifaddrs(ifs);

    if (mtu < 0) {
        SetLastErrorV(UTP_ERR_SOCKET_IOCTL, "unable to obtain MTU for interface '{}'", ifname);
    }

    return mtu;
#else
    DWORD dwRetVal = 0;
    ULONG outBufLen = sizeof(IP_ADAPTER_ADDRESSES);
    IP_ADAPTER_ADDRESSES* pAddresses = NULL;
    int32_t mtu = -1;

    // 初始缓冲区大小
    outBufLen = 15 * 1024;
    pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (pAddresses == NULL) {
        return -1;
    }

    // 循环尝试，直到缓冲区足够
    do {
        dwRetVal = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, pAddresses, &outBufLen);
        if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
            // 缓冲区不够，重新分配更大的
            free(pAddresses);
            pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
            if (pAddresses == NULL) {
                return -1;
            }
        } else if (dwRetVal != NO_ERROR) {
            free(pAddresses);
            return -1;
        }
    } while (dwRetVal == ERROR_BUFFER_OVERFLOW);

    // 遍历适配器
    PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
    while (pCurr) {
        // 匹配 AdapterName (ANSI)
        if (strcmp(pCurr->AdapterName, ifname) == 0) {
            mtu = (int)pCurr->Mtu;
            break;
        }

        // 匹配 FriendlyName (Unicode) - 需要转换 ifname 到 WCHAR
        if (pCurr->FriendlyName) {
            // 将 ifname 从 utf-8 转换为 utf-16
            int32_t wlen = MultiByteToWideChar(CP_UTF8, 0, ifname, -1, NULL, 0);
            std::wstring wIfname;
            wIfname.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, ifname, -1, wIfname.data(), wIfname.size());
            if (wIfname == pCurr->FriendlyName) {
                mtu = (int)pCurr->Mtu;
                break;
            }
        }

        pCurr = pCurr->Next;
    }

    free(pAddresses);
    return mtu;
#endif
}

Address Socket::Util::GetIPPktInfo(const msghdr_t &msg, uint16_t port)
{
#if defined(OS_LINUX)
    if (msg.msg_controllen == 0 || msg.msg_control == nullptr) {
        return Address();
    }

    Address addr;
    for (cmsghdr *cmsg = CMSG_FIRSTHDR((msghdr *)&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR((msghdr *)&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            in_pktinfo *pktinfo = (in_pktinfo *)CMSG_DATA(cmsg);
            sockaddr_in temp;
            temp.sin_family = AF_INET;
            temp.sin_addr = pktinfo->ipi_addr;
            temp.sin_port = htons(port);
            addr.fromSockAddrIn(temp);
            break;
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
            in6_pktinfo *pktinfo6 = (in6_pktinfo *)CMSG_DATA(cmsg);
            sockaddr_in6 temp;
            temp.sin6_family = AF_INET6;
            temp.sin6_addr = pktinfo6->ipi6_addr;
            temp.sin6_port = htons(port);
            addr.fromSockAddrIn6(temp);
            break;
        }
    }

    return addr;
#elif defined(OS_APPLE)
    if (msg.msg_controllen == 0 || msg.msg_control == nullptr) {
        return Address();
    }
    Address addr;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR((msghdr *)&msg); cmsg; cmsg = CMSG_NXTHDR((msghdr *)&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVDSTADDR) {
            struct in_addr *dst = (struct in_addr *)CMSG_DATA(cmsg);
            sockaddr_in temp;
            temp.sin_family = AF_INET;
            temp.sin_addr = *dst;
            temp.sin_port = htons(port);
            addr.fromSockAddrIn(temp);
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo* pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
            sockaddr_in6 temp;
            temp.sin6_family = AF_INET6;
            temp.sin6_addr = pktinfo->ipi6_addr;
            temp.sin6_port = htons(port);
            addr.fromSockAddrIn6(temp);
        }
    }

    return addr;
#else
    if (msg.Control.buf == nullptr || msg.Control.len == 0) {
        return Address();
    }

    for (WSACMSGHDR *cmsg = WSA_CMSG_FIRSTHDR((msghdr_t *)&msg); cmsg != nullptr; cmsg = WSA_CMSG_NXTHDR((msghdr_t *)&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            IN_PKTINFO *pktinfo = (IN_PKTINFO *)WSA_CMSG_DATA(cmsg);
            sockaddr_in temp;
            temp.sin_family = AF_INET;
            temp.sin_addr = pktinfo->ipi_addr;
            temp.sin_port = htons(port);
            Address addr;
            addr.fromSockAddrIn(temp);
            return addr;
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
            IN6_PKTINFO *pktinfo6 = (IN6_PKTINFO *)WSA_CMSG_DATA(cmsg);
            sockaddr_in6 temp;
            temp.sin6_family = AF_INET6;
            temp.sin6_addr = pktinfo6->ipi6_addr;
            temp.sin6_port = htons(port);
            Address addr;
            addr.fromSockAddrIn6(temp);
            return addr;
        }
    }
#endif
}

socket_t Socket::Open(int32_t family)
{
    if (family != AF_INET && family != AF_INET6) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "invalid family: {}", family);
        return -1;
    }

    socket_t sockfd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        int32_t code = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_CREATE, "socket({}, SOCK_DGRAM, IPPROTO_UDP) failed: [{}, {}].", family, code, GetSystemErrnoMsg(code));
        return -1;
    }

    return sockfd;
}

int32_t Socket::Bind(socket_t sockfd, const Address &addr)
{
    sockaddr_storage storage;
    socklen_t len = addr.toSockAddr(storage);
    int32_t status = ::bind(sockfd, (sockaddr *)&storage, len);
    if (status != 0) {
        int32_t code = GetSystemLastError();
        SetLastErrorV(UTP_ERR_SOCKET_BIND, "bind {} to {} failed: [{}, {}].",
                sockfd, addr.toString(), code, GetSystemErrnoMsg(code));
        return -1;
    }

    return 0;
}

void Socket::Close(socket_t sockfd)
{
#if defined(OS_LINUX) || defined(OS_APPLE)
    close(sockfd);
#else
    closesocket(sockfd);
#endif
}


} // namespace utp

} // namespace eular
