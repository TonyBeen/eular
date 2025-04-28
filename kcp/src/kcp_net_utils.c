#include "kcp_net_utils.h"

#include <string.h>

#include "kcp_inc.h"
#include "kcp_error.h"
#include "kcp_protocol.h"

int32_t set_socket_nonblock(socket_t fd)
{
#ifdef OS_WINDOWS
    u_long flag = 1;
    return ioctlsocket(fd, FIONBIO, &flag);
#else
    int32_t flag = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flag | O_NONBLOCK);
#endif
}

int32_t set_socket_reuseaddr(socket_t fd)
{
#ifdef OS_WINDOWS
    BOOL flag = TRUE;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&flag, sizeof(flag));
#else
    int32_t flag = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
#endif
}

int32_t set_socket_reuseport(socket_t fd)
{
#ifdef OS_WINDOWS
    BOOL flag = TRUE;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&flag, sizeof(flag));
#else
    int32_t flag = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
#endif
}

int32_t set_socket_dont_fragment(socket_t fd)
{
#ifdef OS_WINDOWS
    BOOL flag = TRUE;
    return setsockopt(fd, IPPROTO_IP, IP_DONTFRAGMENT, (const char *)&flag, sizeof(flag));
#else
    int32_t flag = IP_PMTUDISC_DO;
    return setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &flag, sizeof(flag));
#endif
}

int32_t set_socket_bind_nic(socket_t fd, const char *nic)
{
    if (nic == NULL) {
        return INVALID_PARAM;
    }

    int32_t status = NO_ERROR;
#if defined(OS_LINUX) || defined(OS_MAC)
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, nic, strlen(nic)) < 0) {
        status = IOCTL_ERROR;
    }
#else
    DWORD bytesReturned = 0;
    if (WSAIoctl(fd, SIO_BINDTODEVICE, (LPVOID)nic, (DWORD)strlen(nic), NULL, 0, &bytesReturned, NULL, NULL) == SOCKET_ERROR) {
        status = IOCTL_ERROR;
    }
#endif

    return status;
}

int32_t set_socket_recverr(socket_t fd, const sockaddr_t *addr)
{
    if (addr == NULL) {
        return INVALID_PARAM;
    }

    int32_t flag = 1;
    if (addr->sa.sa_family == AF_INET) {
        if (setsockopt(fd, IPPROTO_IP, IP_RECVERR, &flag, sizeof(flag)) < 0) {
            return IOCTL_ERROR;
        }
    } else if (addr->sa.sa_family == AF_INET6) {
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVERR, &flag, sizeof(flag)) < 0) {
            return IOCTL_ERROR;
        }
    }

    return NO_ERROR;
}

int32_t set_socket_sendbuf(socket_t fd, int32_t size)
{
    int32_t status = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    if (status != 0) {
        status = IOCTL_ERROR;
    }

    return status;
}

int32_t set_socket_recvbuf(socket_t fd, int32_t size)
{
    int32_t status = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if (status != 0) {
        status = IOCTL_ERROR;
    }

    return status;
}

bool sockaddr_equal(const sockaddr_t *a, const sockaddr_t *b)
{
    if (a->sa.sa_family != b->sa.sa_family) {
        return false;
    }

    if (a->sa.sa_family == AF_INET) {
        return (a->sin.sin_port == b->sin.sin_port) &&
               (a->sin.sin_addr.s_addr == b->sin.sin_addr.s_addr);
    } else if (a->sa.sa_family == AF_INET6) {
        return (a->sin6.sin6_port == b->sin6.sin6_port) &&
               (memcmp(a->sin6.sin6_addr.s6_addr, b->sin6.sin6_addr.s6_addr, 16) == 0);
    }

    return false;
}

const char *sockaddr_to_string(const sockaddr_t *addr, char *buf, uint32_t len)
{
    if (addr == NULL || buf == NULL || len == 0) {
        return NULL;
    }

    char ip[SOCKADDR_STRING_LEN] = {0};
    uint16_t port = 0;
    if (addr->sa.sa_family == AF_INET) {
        inet_ntop(AF_INET, &addr->sin.sin_addr, ip, len);
        port = ntohs(addr->sin.sin_port);
        snprintf(buf, len, "%s:%d", ip, port);
    } else if (addr->sa.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &addr->sin6.sin6_addr, ip, len);
        port = ntohs(addr->sin6.sin6_port);
        snprintf(buf, len, "[%s]:%d", ip, port);
    } else {
        buf[0] = '\0';
    }

    return buf;
}

int32_t kcp_send_packet(struct KcpConnection *kcp_conn, const struct iovec *data, uint32_t size)
{
    return kcp_send_packet_raw(kcp_conn->kcp_ctx->sock, &kcp_conn->remote_host, data, size);
}

int32_t kcp_send_packet_raw(int32_t sock, const sockaddr_t *remote_addr, const struct iovec *data, uint32_t size)
{
    if (size > KCP_WND_RCV) {
        return INVALID_PARAM;
    }

    int32_t send_packet = 0;
    int32_t send_size = 0;
#if defined(OS_LINUX) || defined(OS_MAC)

#if !defined(USE_SENDMMSG)
    for (int32_t i = 0; i < size; ++i) {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = (void *)remote_addr;
        msg.msg_namelen = sizeof(sockaddr_t);
        msg.msg_iov = (struct iovec *)&data[i];
        msg.msg_iovlen = 1;
        send_size = sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (send_size <= 0) {
            int32_t code = get_last_errno();
            if (code != EAGAIN) {
                return WRITE_ERROR;
            } else {
                break;
            }
        }

        ++send_packet;
    }
#else
    struct mmsghdr msgvec[KCP_WND_RCV];
    for (int32_t i = 0; i < size; ++i) {
        struct msghdr *msg = &msgvec[i].msg_hdr;
        msg->msg_name = remote_addr;
        msg->msg_namelen = sizeof(sockaddr_t);
        msg->msg_iov = &data[i];
        msg->msg_iovlen = 1;

        msgvec[i].msg_len = 1;
    }
    send_packet = sendmmsg(sock, msgvec, size, MSG_NOSIGNAL);
    if (send_packet <= 0) {
        send_packet = WRITE_ERROR;
    }

#endif

#else
    // TODO WSASendMsg 会合并数据为一个包, 需要使用类似sendmmsg接口
    // WSABUF wsa_buf[PACKET_COUNT_PER_SENT];
    // for (uint32_t i = 0; i < size; i++) {
    //     wsa_buf[i].buf = (char *)data[i].iov_base;
    //     wsa_buf[i].len = data[i].iov_len;
    // }

    // WSAMSG msg;
    // memset(&msg, 0, sizeof(msg));
    // msg.name = (SOCKADDR *)remote_addr;
    // msg.namelen = sizeof(sockaddr_t);
    // msg.lpBuffers = wsa_buf;
    // msg.dwBufferCount = size;

    // DWORD bytes_sent = 0;
    // int32_t status = WSASendMsg(sock, &msg, 0, &bytes_sent, NULL, NULL);
    // if (status == SOCKET_ERROR) {
    //     send_size = WRITE_ERROR;
    // } else {
    //     send_size = bytes_sent;
    // }

#endif
    return send_packet;
}

int32_t get_last_errno()
{
#ifdef OS_WINDOWS
    return WSAGetLastError();
#else
    return errno;
#endif
}

THREAD_LOCAL char buffer[128];

const char *errno_string(int32_t err)
{
#ifdef OS_WINDOWS
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK,
        0, ABS(err), 0, buffer, sizeof(buffer), NULL);

    return buffer;
#else
    return strerror(ABS(err));
#endif
}

int32_t kcp_add_write_event(struct KcpConnection *kcp_conn)
{
    if (list_empty(&kcp_conn->node_list)) {
        kcp_context_t *kcp_ctx = kcp_conn->kcp_ctx;
        return event_add(kcp_ctx->write_event, NULL);
    }

    return NO_ERROR;
}
