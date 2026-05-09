/*************************************************************************
    > File Name: udp.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:00:00 PM CST
 ************************************************************************/

#include "socket/udp.h"

#include <cstring>
#include <stdexcept>
#include <mutex>

#include <utils/string8.h>

#include "util/error.h"
#include "socket/util.h"
#include "utp/config.h"
#include "logger/logger.h"
#include "util/fiu_local.h"

namespace eular {
namespace utp {
UdpSocket::UdpSocket(Config &config) :
#if defined(USE_SENDMMSG)
    m_mmsg(MAX_MMSG_SIZE, UTP_ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE),
#endif
    m_config(config)
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
        m_sock = INVALID_SOCKET;
    }
}

void UdpSocket::updateTag(const std::string &tag)
{
    m_tag = tag + "[udp socket(" + std::to_string(m_sock) + ")]";
}

Status UdpSocket::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    Address address(ip, port);
    if (!address.isValid()) {
        return Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("{} bind({}) to an invalid address {}:{}", tag(), m_sock, ip.c_str(), port));
    }

    {
        Status openStatus;
        m_sock = Socket::Open(address.family(), openStatus);
        if (!openStatus.ok()) {
            return openStatus;
        }
    }

    Status st = Status::ErrorLiteral(UTP_ERR_SOCKET_IOCTL, "socket setup failed");
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
        st = Socket::Ioctl::SetRecvError(m_sock, address.family());
        if (!st.ok()) break;

        // NOTE 绑定在 :: 上时支持IPv4和IPv6双栈
        if (address.family() == AF_INET6 && address != Address::AnyIPv6()) {
            st = Socket::Ioctl::SetIPv6Only(m_sock);
            if (!st.ok()) break;
        }

        if (!ifname.empty()) {
            st = Socket::Ioctl::SetBindInterface(m_sock, ifname.c_str());
            if (!st.ok()) break;
        }

        st = Socket::Ioctl::SetRecvBufferSize(m_sock, m_config.recv_buf_size);
        if (!st.ok()) break;

        st = Socket::Ioctl::SetSendBufferSize(m_sock, m_config.send_buf_size);
        if (!st.ok()) break;

        // NOTE 调高 UDP 包的优先级
        st = Socket::Ioctl::SetIPTos(m_sock);
        if (!st.ok()) break;

        st = Socket::Ioctl::SetNoSigPipe(m_sock);
        if (!st.ok()) break;

        st = Socket::Bind(m_sock, address);
        if (!st.ok()) break;

        m_bindAddr = address;
        m_localAddr.fromSocket(m_sock);
        return Status::OK();
    } while (0);

    Socket::Close(m_sock);
    m_sock = INVALID_SOCKET;
    return st;
}

int32_t UdpSocket::recvErrorMsg(ErrorMsg &errMsg, Status &status)
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
            status = Status::Error(UTP_ERR_SOCKET_READ, fmt::format("receive ICMP message failed: [{}, {}]", code, GetSystemErrnoMsg(code)));
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
        if (err->ee_origin != SO_EE_ORIGIN_ICMP && err->ee_origin != SO_EE_ORIGIN_ICMP6) {
            continue;
        }

        errMsg.ee_type.push_back(err->ee_type);
        errMsg.ee_code.push_back(err->ee_code);
        errMsg.ee_info.push_back(err->ee_info);
    }

    return 1;
#else
    UNUSED(errMsg);
    UNUSED(status);
    return 0;
#endif
}

int32_t UdpSocket::recv(std::vector<MsgMetaInfo> &msgVec, Status &status)
{
#if defined(USE_SENDMMSG)
    int32_t n = ::recvmmsg(m_sock, m_mmsg.mmsghdrAt(0), m_mmsg.size(), 0, nullptr);
    if (n < 0) {
        int32_t code = GetSystemLastError();
        if (code == EAGAIN || code == EWOULDBLOCK) {
            return 0;
        } else {
            status = Status::Error(UTP_ERR_SOCKET_READ, fmt::format("{} recvmmsg({}) failed: [{}, {}]", tag(), m_sock, code, GetSystemErrnoMsg(code)));
            return -1;
        }
    }

    const Address *pointer = &m_bindAddr;
    uint16_t port = m_bindAddr.port(); // 当绑定 0 端口时, 使用实际接收端口
    if (m_localAddr.isValid()) {
        port = m_localAddr.port();
        pointer = &m_localAddr;
    }

    msgVec.resize(n);
    for (int32_t i = 0; i < n; ++i) {
        MsgMetaInfo &rmsg = msgVec[i];
        rmsg.len = m_mmsg.mmsghdrAt(i)->msg_len;
        rmsg.data = m_mmsg.dataAt(i);
        rmsg.slice_count = 0;
        rmsg.metaInfo.fd = m_sock;
        rmsg.metaInfo.localAddress = Socket::Util::GetIPPktInfo(m_mmsg.mmsghdrAt(i)->msg_hdr, m_bindAddr.family());
        if (!rmsg.metaInfo.localAddress.isValid()) {
            rmsg.metaInfo.localAddress = *pointer;
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
            status = Status::Error(UTP_ERR_SOCKET_READ, fmt::format("{} recvmmsg({}) failed: [{}, {}]", tag(), m_sock, code, GetSystemErrnoMsg(code)));
            return -1;
        }
    }
#elif defined(OS_WINDOWS)
    static std::once_flag flagWSARecvMsg;
    static LPFN_WSARECVMSG lpfnWSARecvMsg = nullptr;
    std::call_once(flagWSARecvMsg, [this]() {
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
    char cmsgbuf[WSA_CMSG_SPACE(sizeof(in6_pktinfo))] = {0};
    WSAMSG msg;
    WSABUF iov;
    iov.buf = reinterpret_cast<char *>(m_recvBuffer.data());
    iov.len = static_cast<ULONG>(m_recvBuffer.capacity());
    msg.name = (LPSOCKADDR)&remoteAddr;
    msg.namelen = addrLen;
    msg.lpBuffers = &iov;
    msg.dwBufferCount = 1;
    msg.Control.buf = cmsgbuf;
    msg.Control.len = sizeof(cmsgbuf);
    msg.dwFlags = 0;

    DWORD nreads = 0;
    int32_t wsaStatus = lpfnWSARecvMsg(m_sock, &msg, &nreads, nullptr, nullptr);
    if (wsaStatus == SOCKET_ERROR) {
        int32_t code = GetSystemLastError();
        if (code == WSAEWOULDBLOCK) {
            return 0;
        } else {
            status = Status::Error(UTP_ERR_SOCKET_READ, fmt::format("{} WSARecvMsg({}) failed: [{}, {}]", tag(), m_sock, code, GetSystemErrnoMsg(code)));
            return -1;
        }
    }
#endif // defined(OS_LINUX) || defined(OS_APPLE)

    const Address *pointer = &m_bindAddr;
    uint16_t port = m_bindAddr.port(); // 当绑定 0 端口时, 使用实际接收端口
    if (m_localAddr.isValid()) {
        port = m_localAddr.port();
        pointer = &m_localAddr;
    }

    msgVec.resize(1);
    MsgMetaInfo &rmsg = msgVec[0];
    rmsg.len = static_cast<size_t>(nreads);
    rmsg.data = m_recvBuffer.data();
    rmsg.slice_count = 0;
    rmsg.metaInfo.fd = m_sock;
    rmsg.metaInfo.localAddress = Socket::Util::GetIPPktInfo(msg, port);
    if (!rmsg.metaInfo.localAddress.isValid()) {
        rmsg.metaInfo.localAddress = *pointer;
    }
    rmsg.metaInfo.peerAddress.fromSockAddr((struct sockaddr *)&remoteAddr, addrLen);

    return 1;
#endif // defined(USE_SENDMMSG)
}

int32_t UdpSocket::send(const std::vector<MsgMetaInfo> &msgVec, Status &status)
{
    if (!isValid()) {
        status = Status::Error(UTP_ERR_SOCKET_WRITE, fmt::format("{} send failed: socket invalid", tag()));
        return -1;
    }

    int32_t sentCount = 0;
    for (const MsgMetaInfo &msg : msgVec) {
        if (!msg.metaInfo.peerAddress.isValid()) {
            continue;
        }

        const bool hasSlices = msg.slice_count > 0;
        if (!hasSlices && (msg.data == nullptr || msg.len == 0)) {
            continue;
        }

        sockaddr_storage remoteStorage;
        std::memset(&remoteStorage, 0, sizeof(remoteStorage));
        socklen_t remoteLen = msg.metaInfo.peerAddress.toSockAddr(remoteStorage);
        if (remoteLen == 0) {
            continue;
        }

#if defined(OS_WINDOWS)
        int32_t wsaRet = 0;
        if (hasSlices) {
            WSABUF bufs[kMaxMsgSlices] = {};
            DWORD iovCount = 0;
            for (uint8_t i = 0; i < msg.slice_count && i < kMaxMsgSlices; ++i) {
                if (msg.slices[i].data == nullptr || msg.slices[i].len == 0) {
                    continue;
                }
                bufs[iovCount].buf = const_cast<char *>(static_cast<const char *>(msg.slices[i].data));
                bufs[iovCount].len = static_cast<ULONG>(msg.slices[i].len);
                ++iovCount;
            }

            if (iovCount == 0) {
                continue;
            }

            DWORD bytesSent = 0;
            wsaRet = ::WSASendTo(m_sock,
                                 bufs,
                                 iovCount,
                                 &bytesSent,
                                 0,
                                 reinterpret_cast<sockaddr *>(&remoteStorage),
                                 remoteLen,
                                 nullptr,
                                 nullptr);
        } else {
            int32_t nwritten = ::sendto(m_sock,
                                        static_cast<const char *>(msg.data),
                                        static_cast<int32_t>(msg.len),
                                        0,
                                        reinterpret_cast<sockaddr *>(&remoteStorage),
                                        remoteLen);
            wsaRet = (nwritten == SOCKET_ERROR) ? SOCKET_ERROR : 0;
        }

        if (wsaRet == SOCKET_ERROR) {
            int32_t code = GetSystemLastError();
            if (code == WSAEWOULDBLOCK) {
                return sentCount > 0 ? sentCount : 0;
            }

            status = Status::Error(UTP_ERR_SOCKET_WRITE,
                          fmt::format("{} sendto({}) failed: [{}, {}]",
                          tag(),
                          m_sock,
                          code,
                          GetSystemErrnoMsg(code)));
            return sentCount > 0 ? sentCount : -1;
        }
#else
        ssize_t nwritten = 0;
        if (hasSlices) {
            struct iovec iov[kMaxMsgSlices] = {};
            size_t iovCount = 0;
            for (uint8_t i = 0; i < msg.slice_count && i < kMaxMsgSlices; ++i) {
                if (msg.slices[i].data == nullptr || msg.slices[i].len == 0) {
                    continue;
                }
                iov[iovCount].iov_base = const_cast<void *>(msg.slices[i].data);
                iov[iovCount].iov_len = msg.slices[i].len;
                ++iovCount;
            }

            if (iovCount == 0) {
                continue;
            }

            struct msghdr sndmsg;
            std::memset(&sndmsg, 0, sizeof(sndmsg));
            sndmsg.msg_name = &remoteStorage;
            sndmsg.msg_namelen = remoteLen;
            sndmsg.msg_iov = iov;
            sndmsg.msg_iovlen = iovCount;
            nwritten = ::sendmsg(m_sock, &sndmsg, MSG_NOSIGNAL);
        } else {
            nwritten = ::sendto(m_sock,
                                msg.data,
                                msg.len,
                                MSG_NOSIGNAL,
                                reinterpret_cast<sockaddr *>(&remoteStorage),
                                remoteLen);
        }

        if (fiu_fail("net/udp/sendto")) {
            errno = ENOBUFS;
            nwritten = -1;
        }
        if (nwritten < 0) {
            int32_t code = GetSystemLastError();
            if (code == EAGAIN || code == EWOULDBLOCK) {
                return sentCount > 0 ? sentCount : 0;
            }

            status = Status::Error(UTP_ERR_SOCKET_WRITE,
                          fmt::format("{} sendto({}) failed: [{}, {}]",
                          tag(),
                          m_sock,
                          code,
                          GetSystemErrnoMsg(code)));
            return sentCount > 0 ? sentCount : -1;
        }
#endif
        ++sentCount;
    }

    return sentCount;
}

} // namespace utp
} // namespace eular
