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

    return 0;
}
