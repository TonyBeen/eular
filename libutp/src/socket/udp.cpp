/*************************************************************************
    > File Name: udp.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:00:00 PM CST
 ************************************************************************/

#include "socket/udp.h"

#include <stdexcept>
#include <mutex>

#include <utils/string8.h>

#include "util/error.h"
#include "socket/util.h"
#include "utp/config.h"
#include "logger/logger.h"

namespace eular {
namespace utp {
UdpSocket::UdpSocket()
#if defined(USE_SENDMMSG)
    : m_mmsg(MAX_MMSG_SIZE, UTP_ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE)
#endif
{
#if defined(USE_SENDMMSG)
    if (!m_mmsg.valid()) {
        throw std::runtime_error("UdpSocket: mmsg init failed");
    }
#endif
    m_recvBuffer.reserve(UTP_ETHERNET_MTU);
}

UdpSocket::~UdpSocket()
{
    if (m_sock) {
        Socket::Close(m_sock);
        m_sock = -1;
    }
}

int32_t UdpSocket::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    Address address(ip, port);
    if (!address.isValid()) {
        SetLastErrorV(UTP_ERR_INVALID_ARGUMENT, "{} bind({}) to an invalid address {}:{}", tag(), m_sock, ip.c_str(), port);
        return -1;
    }

    m_sock = Socket::Open(address.family());
    if (m_sock < 0) {
        return -1;
    }

    do {
        if (Socket::Ioctl::SetNonBlock(m_sock) < 0) {
            break;
        }

        if (Socket::Ioctl::SetReuseAddr(m_sock)) {
            break;
        }

        if (Socket::Ioctl::SetReusePort(m_sock)) {
            break;
        }

        // NOTE 设置不分片, 以免路径MTU发现机制失效
        if (Socket::Ioctl::SetDontFragment(m_sock) < 0) {
            break;
        }

        // NOTE 设置接收错误消息选项, 以便在接收数据报时获取ICMP错误消息
        if (Socket::Ioctl::SetRecvError(m_sock, address.family()) < 0) {
            break;
        }

        // NOTE 绑定在 :: 上时支持IPv4和IPv6双栈
        if (address.family() == AF_INET6 && address != Address::AnyIPv6()) {
            Socket::Ioctl::SetIPv6Only(m_sock);
        }

        if (!ifname.empty() && Socket::Ioctl::SetBindInterface(m_sock, ifname.c_str()) < 0) {
            break;
        }

        if (Socket::Ioctl::SetRecvBufferSize(m_sock, Config::Instance()->recv_buf_size) < 0) {
            break;
        }

        if (Socket::Ioctl::SetSendBufferSize(m_sock, Config::Instance()->send_buf_size) < 0) {
            break;
        }

        // NOTE 调高 UDP 包的优先级
        if (Socket::Ioctl::SetIPTos(m_sock) < 0) {
            break;
        }

        if (Socket::Ioctl::SetNoSigPipe(m_sock) < 0) {
            break;
        }

        if (Socket::Bind(m_sock, address) < 0) {
            break;
        }

        m_bindAddr = address;
        m_localAddr.fromSocket(m_sock);
        return 0;
    } while (0);

    Socket::Close(m_sock);
    m_sock = -1;
    return -1;
}

int32_t UdpSocket::recvErrorMsg(ErrorMsg &errMsg)
{
#if defined(OS_LINUX)
    sockaddr_storage remoteAddr;
    socket_t addrLen = sizeof(remoteAddr);

    struct cmsghdr *cmsg;
    char cmsgbuf[1024];
    struct msghdr msg;
    struct iovec iov;
    iov.iov_base = m_recvBuffer.data();
    iov.iov_len = m_recvBuffer.capacity();

    msg.msg_name = &remoteAddr;
    msg.msg_namelen = addrLen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    msg.msg_flags = 0;

    ssize_t nreads = recvmsg(m_sock, &msg, MSG_ERRQUEUE | MSG_NOSIGNAL | MSG_DONTWAIT);
    if (nreads < 0) {
        int32_t code = GetSystemLastError();
        if (code == EAGAIN || code == EWOULDBLOCK) {
            return 0;
        } else {
            SetLastErrorV(UTP_ERR_SOCKET_READ, "receive ICMP message failed: [{}, {}]", code, GetSystemErrnoMsg(code));
            return -1;
        }
    }

    errMsg.data = m_recvBuffer.data();
    errMsg.len = static_cast<size_t>(nreads);
    errMsg.peer_addr.fromSockAddr((struct sockaddr *)&remoteAddr, addrLen);
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_type != IP_RECVERR) {
            continue;
        }
        if (cmsg->cmsg_level != SOL_IP) {
            continue;
        }
        struct sock_extended_err *err = (struct sock_extended_err *)CMSG_DATA(cmsg);
        if (err == NULL) {
            continue;
        }
        if (err->ee_origin != SO_EE_ORIGIN_ICMP || err->ee_origin != SO_EE_ORIGIN_ICMP6) {
            continue;
        }

        errMsg.ee_type.push_back(err->ee_type);
        errMsg.ee_code.push_back(err->ee_code);
        errMsg.ee_info.push_back(err->ee_info);
    }

    return 1;
#else
    UNUSED(errMsg);
    return 0;
#endif
}

int32_t UdpSocket::recv(std::vector<ReceivedMsg> &msgVec)
{
#if defined(USE_SENDMMSG)
    int32_t n = ::recvmmsg(m_sock, m_mmsg.mmsghdrAt(0), m_mmsg.size(), 0, nullptr);
    if (n < 0) {
        int32_t code = GetSystemLastError();
        if (code == EAGAIN || code == EWOULDBLOCK) {
            return 0;
        } else {
            SetLastErrorV(UTP_ERR_SOCKET_READ, "{} recvmmsg({}) failed: [{}, {}]", tag(), m_sock, code, GetSystemErrnoMsg(code));
            return -1;
        }
    }

    uint16_t port = m_bindAddr.port(); // 当绑定 0 端口时, 使用实际接收端口
    if (m_localAddr.isValid()) {
        port = m_localAddr.port();
    }

    msgVec.resize(n);
    for (int32_t i = 0; i < n; ++i) {
        ReceivedMsg &rmsg = msgVec[i];
        rmsg.len = m_mmsg.mmsghdrAt(i)->msg_len;
        rmsg.data = m_mmsg.dataAt(i);
        rmsg.metaInfo.fd = m_sock;
        rmsg.metaInfo.localAddress = Socket::Util::GetIPPktInfo(m_mmsg.mmsghdrAt(i)->msg_hdr, m_bindAddr.family());
        if (!rmsg.metaInfo.localAddress.isValid()) {
            rmsg.metaInfo.localAddress.setPort(port);
        } else {
            rmsg.metaInfo.localAddress = Address::Loopback(m_bindAddr.family(), port);
        }
        rmsg.metaInfo.peerAddress.fromSockAddr(m_mmsg.sockaddrAt(i), m_mmsg.mmsghdrAt(i)->msg_hdr.msg_namelen);
    }

    return n;
#else

#if defined(OS_LINUX) || defined(OS_APPLE)
    sockaddr_storage remoteAddr;
    socket_t addrLen = sizeof(remoteAddr);
    char cmsgbuf[CMSG_SPACE(sizeof(in6_pktinfo))] = {0};
    struct msghdr msg;
    struct iovec iov;
    iov.iov_base = m_recvBuffer.data();
    iov.iov_len = m_recvBuffer.capacity();
    msg.msg_name = &remoteAddr;
    msg.msg_namelen = addrLen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    msg.msg_flags = 0;
    ssize_t nreads = ::recvmsg(m_sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (nreads < 0) {
        int32_t code = GetSystemLastError();
        if (code == EAGAIN || code == EWOULDBLOCK) {
            return 0;
        } else {
            SetLastErrorV(UTP_ERR_SOCKET_READ, "{} recvmmsg({}) failed: [{}, {}]", tag(), m_sock, code, GetSystemErrnoMsg(code));
            return -1;
        }
    }
#elif defined(OS_WINDOWS)
    static std::once_flag flagWSARecvMsg;
    static LPFN_WSARECVMSG lpfnWSARecvMsg = nullptr;
    std::call_once(flagWSARecvMsg, []() {
        GUID guidWSARecvMsg = WSAID_WSARECVMSG;
        DWORD dwBytesReturned = 0;
        int32_t result = ::WSAIoctl(m_sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
                                    &guidWSARecvMsg, sizeof(guidWSARecvMsg),
                                    &lpfnWSARecvMsg, sizeof(lpfnWSARecvMsg),
                                    &dwBytesReturned, nullptr, nullptr);
        if (result != 0) {
            int32_t code = GetSystemLastError();
            eular::String8 msg = eular::String8::Format("Failed to obtain the pointer to WSARecvMsg: [%d, %s]", code, GetSystemErrnoMsg(code));
            throw std::runtime_error(msg.c_str());
        }
    });

    sockaddr_storage remoteAddr;
    socket_t addrLen = sizeof(remoteAddr);
    WSAMSG msg;
    WSABUF iov;
    iov.buf = m_recvBuffer.data();
    iov.len = static_cast<ULONG>(m_recvBuffer.capacity());
    msg.name = (LPSOCKADDR)&remoteAddr;
    msg.namelen = addrLen;
    msg.lpBuffers = &iov;
    msg.dwBufferCount = 1;
    msg.Control.len = 0;
    msg.Control.buf = nullptr;
    msg.dwFlags = 0;

    DWORD nreads = 0;
    int32_t status = lpfnWSARecvMsg(m_sock, &msg, &nreads, nullptr, nullptr);
    if (status == SOCKET_ERROR) {
        int32_t code = GetSystemLastError();
        if (code == WSAEWOULDBLOCK) {
            return 0;
        } else {
            SetLastErrorV(UTP_ERR_SOCKET_READ, "{} WSARecvMsg({}) failed: [{}, {}]", tag(), m_sock, code, GetSystemErrnoMsg(code));
            return -1;
        }
    }
#endif // defined(OS_LINUX) || defined(OS_APPLE)

    uint16_t port = m_bindAddr.port(); // 当绑定 0 端口时, 使用实际接收端口
    if (m_localAddr.isValid()) {
        port = m_localAddr.port();
    }

    msgVec.resize(1);
    ReceivedMsg &rmsg = msgVec[0];
    rmsg.len = static_cast<size_t>(nreads);
    rmsg.data = m_recvBuffer.data();
    rmsg.metaInfo.fd = m_sock;
    rmsg.metaInfo.localAddress = Socket::Util::GetIPPktInfo(msg, m_bindAddr.family());
    if (!rmsg.metaInfo.localAddress.isValid()) {
        rmsg.metaInfo.localAddress.setPort(port);
    } else {
        rmsg.metaInfo.localAddress = Address::Loopback(m_bindAddr.family(), port);
    }
    rmsg.metaInfo.peerAddress.fromSockAddr((struct sockaddr *)&remoteAddr, addrLen);

    return 1;
#endif // defined(USE_SENDMMSG)
}

} // namespace utp
} // namespace eular
