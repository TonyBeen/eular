#include "kcp_net_utils.h"
#include "kcp_inc.h"
#include "kcp_error.h"

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

const char *sockaddr_to_string(const sockaddr_t *addr, char *buf, size_t len)
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
