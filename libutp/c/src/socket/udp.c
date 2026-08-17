#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "socket/udp.h"

#include <limits.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <net/if.h>
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542 1
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#if !defined(_WIN32)
#define UTP_UDP_SOCKET_CONTROL_CAPACITY CMSG_SPACE(sizeof(struct in6_pktinfo))

typedef union utp_udp_socket_control {
    struct cmsghdr alignment;                               // 保证 cmsg 头与数据的自然对齐
    uint8_t        bytes[UTP_UDP_SOCKET_CONTROL_CAPACITY];  // 单个 IPv6 pktinfo 控制消息
} utp_udp_socket_control_t;

static bool utp_udp_socket_is_would_block_error(int32_t system_error)
{
    if (system_error == EAGAIN) {
        return true;
    }
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (system_error == EWOULDBLOCK) {
        return true;
    }
#endif
    return false;
}

static bool utp_udp_socket_align_control_length(size_t length, size_t* aligned_length)
{
    const size_t alignment = sizeof(size_t);

    if (aligned_length == NULL || length > SIZE_MAX - (alignment - 1u)) {
        return false;
    }
    *aligned_length = (length + alignment - 1u) & ~(alignment - 1u);
    return true;
}

/** @brief 遍历控制消息，避免 musl 的 CMSG_NXTHDR 在严格符号转换检查下触发告警。 */
static struct cmsghdr* utp_udp_socket_next_control(const struct msghdr* message, const struct cmsghdr* current)
{
    const uint8_t* control;
    const uint8_t* current_bytes;
    const uint8_t* end;
    size_t         aligned_length;

    if (message == NULL || current == NULL || message->msg_control == NULL || current->cmsg_len < CMSG_LEN(0u)) {
        return NULL;
    }
    control       = (const uint8_t*)message->msg_control;
    current_bytes = (const uint8_t*)current;
    end           = control + message->msg_controllen;
    if (current_bytes < control || current_bytes > end) {
        return NULL;
    }
    if (!utp_udp_socket_align_control_length((size_t)current->cmsg_len, &aligned_length) ||
        aligned_length > (size_t)(end - current_bytes) ||
        sizeof(struct cmsghdr) > (size_t)(end - (current_bytes + aligned_length))) {
        return NULL;
    }
    return (struct cmsghdr*)(void*)(current_bytes + aligned_length);
}
#endif

static bool utp_udp_socket_ifname_empty(const char* ifname) { return ifname == NULL || ifname[0] == '\0'; }

static bool utp_udp_socket_source_is_usable(const utp_address_t* local, const utp_address_t* peer)
{
    size_t length;

    if (local == NULL || peer == NULL || local->family != peer->family) {
        return false;
    }
    length = local->family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : local->family == UTP_ADDRESS_FAMILY_IPV6 ? 16u : 0u;
    if (length == 0u) {
        return false;
    }
    for (size_t index = 0u; index < length; ++index) {
        if (local->address[index] != 0u) {
            return true;
        }
    }
    return false;
}

static utp_internal_error_t utp_udp_socket_enable_packet_info(utp_udp_socket_t* udp_socket, uint8_t address_family)
{
#if defined(_WIN32)
    (void)udp_socket;
    (void)address_family;
    return UTP_INTERNAL_ERROR_OK;
#else
    const int enabled = 1;
    int       native_handle;

    if (!utp_udp_socket_is_open(udp_socket)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    native_handle = (int)udp_socket->native_handle;
    if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(__APPLE__) && defined(IP_PKTINFO)
        return setsockopt(native_handle, IPPROTO_IP, IP_PKTINFO, &enabled, (socklen_t)sizeof(enabled)) == 0
                   ? UTP_INTERNAL_ERROR_OK
                   : utp_internal_error_from_errno(errno);
#elif defined(IP_PKTINFO)
        return setsockopt(native_handle, IPPROTO_IP, IP_PKTINFO, &enabled, (socklen_t)sizeof(enabled)) == 0
                   ? UTP_INTERNAL_ERROR_OK
                   : utp_internal_error_from_errno(errno);
#else
        return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
    }
    if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_RECVPKTINFO)
        return setsockopt(native_handle, IPPROTO_IPV6, IPV6_RECVPKTINFO, &enabled, (socklen_t)sizeof(enabled)) == 0
                   ? UTP_INTERNAL_ERROR_OK
                   : utp_internal_error_from_errno(errno);
#else
        return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
    }
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
#endif
}

static utp_internal_error_t utp_udp_socket_validate_slices(const utp_udp_send_slice_t* slices, size_t slice_count,
                                                           size_t* total_length)
{
    if (slices == NULL || slice_count == 0u || slice_count > UTP_UDP_SOCKET_MAX_SEND_SLICES || total_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    size_t total = 0u;

    for (size_t index = 0u; index < slice_count; ++index) {
        if (slices[index].data == NULL || slices[index].length == 0u || slices[index].length > SIZE_MAX - total) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        total += slices[index].length;
    }
    *total_length = total;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_udp_socket_bind_interface(utp_udp_socket_t* udp_socket, uint8_t address_family,
                                                          const char* ifname)
{
    if (utp_udp_socket_ifname_empty(ifname)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (!utp_udp_socket_is_open(udp_socket)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    {
        const uint32_t interface_index = (uint32_t)if_nametoindex(ifname);
        SOCKET         native_handle;

        if (interface_index == 0u) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        native_handle = (SOCKET)udp_socket->native_handle;
        if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(IP_UNICAST_IF)
            DWORD option = htonl((uint32_t)interface_index);

            if (setsockopt(native_handle, IPPROTO_IP, IP_UNICAST_IF, (const char*)&option, (int)sizeof(option)) != 0) {
                return UTP_INTERNAL_ERROR_IO;
            }
            return UTP_INTERNAL_ERROR_OK;
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
        if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_UNICAST_IF)
            DWORD option = (DWORD)interface_index;

            if (setsockopt(native_handle, IPPROTO_IPV6, IPV6_UNICAST_IF, (const char*)&option, (int)sizeof(option)) !=
                0) {
                return UTP_INTERNAL_ERROR_IO;
            }
            return UTP_INTERNAL_ERROR_OK;
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#else
    {
        const uint32_t interface_index = (uint32_t)if_nametoindex(ifname);
        int            native_handle;

        if (interface_index == 0u) {
            return errno == 0 ? UTP_INTERNAL_ERROR_INVALID_ARGUMENT : utp_internal_error_from_errno(errno);
        }
        native_handle = (int)udp_socket->native_handle;
#if defined(__linux__) && defined(SO_BINDTODEVICE)
        (void)address_family;
        if (setsockopt(native_handle, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)(strlen(ifname) + 1u)) < 0) {
            return utp_internal_error_from_errno(errno);
        }
        return UTP_INTERNAL_ERROR_OK;
#else
        if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(IP_BOUND_IF)
            if (setsockopt(native_handle, IPPROTO_IP, IP_BOUND_IF, &interface_index,
                           (socklen_t)sizeof(interface_index)) < 0) {
                return utp_internal_error_from_errno(errno);
            }
            return UTP_INTERNAL_ERROR_OK;
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
        if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_BOUND_IF)
            if (setsockopt(native_handle, IPPROTO_IPV6, IPV6_BOUND_IF, &interface_index,
                           (socklen_t)sizeof(interface_index)) < 0) {
                return utp_internal_error_from_errno(errno);
            }
            return UTP_INTERNAL_ERROR_OK;
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
#endif
    }
#endif
}

static utp_internal_error_t utp_udp_socket_enable_dont_fragment(utp_udp_socket_t* udp_socket, uint8_t address_family)
{
    if (!utp_udp_socket_is_open(udp_socket)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    {
        SOCKET native_handle = (SOCKET)udp_socket->native_handle;
        DWORD  enabled       = 1u;

        if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(IP_DONTFRAGMENT)
            return setsockopt(native_handle, IPPROTO_IP, IP_DONTFRAGMENT, (const char*)&enabled,
                              (int)sizeof(enabled)) == 0
                       ? UTP_INTERNAL_ERROR_OK
                       : UTP_INTERNAL_ERROR_IO;
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
        if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_DONTFRAG)
            return setsockopt(native_handle, IPPROTO_IPV6, IPV6_DONTFRAG, (const char*)&enabled,
                              (int)sizeof(enabled)) == 0
                       ? UTP_INTERNAL_ERROR_OK
                       : UTP_INTERNAL_ERROR_IO;
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
    }
#else
    {
        int native_handle = (int)udp_socket->native_handle;

        if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
            const int mode = IP_PMTUDISC_DO;

            return setsockopt(native_handle, IPPROTO_IP, IP_MTU_DISCOVER, &mode, (socklen_t)sizeof(mode)) == 0
                       ? UTP_INTERNAL_ERROR_OK
                       : utp_internal_error_from_errno(errno);
#elif defined(IP_DONTFRAG)
            const int enabled = 1;

            return setsockopt(native_handle, IPPROTO_IP, IP_DONTFRAG, &enabled, (socklen_t)sizeof(enabled)) == 0
                       ? UTP_INTERNAL_ERROR_OK
                       : utp_internal_error_from_errno(errno);
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
        if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_DONTFRAG)
            const int enabled = 1;

            return setsockopt(native_handle, IPPROTO_IPV6, IPV6_DONTFRAG, &enabled, (socklen_t)sizeof(enabled)) == 0
                       ? UTP_INTERNAL_ERROR_OK
                       : utp_internal_error_from_errno(errno);
#else
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
        }
    }
#endif
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
}

void utp_udp_socket_init(utp_udp_socket_t* udp_socket)
{
    if (udp_socket != NULL) {
        udp_socket->native_handle  = UTP_UDP_SOCKET_INVALID;
        udp_socket->local_port     = 0u;
        udp_socket->winsock_active = false;
    }
}

bool utp_udp_socket_is_open(const utp_udp_socket_t* udp_socket)
{
    return udp_socket != NULL && udp_socket->native_handle != UTP_UDP_SOCKET_INVALID;
}

void utp_udp_socket_close(utp_udp_socket_t* udp_socket)
{
    if (!utp_udp_socket_is_open(udp_socket)) {
        return;
    }
#if defined(_WIN32)
    (void)closesocket((SOCKET)udp_socket->native_handle);
    if (udp_socket->winsock_active) {
        (void)WSACleanup();
    }
#else
    (void)close((int)udp_socket->native_handle);
#endif
    utp_udp_socket_init(udp_socket);
}

utp_internal_error_t utp_udp_socket_open(utp_udp_socket_t* udp_socket, uint8_t address_family)
{
    int native_family;

    if (udp_socket == NULL || utp_udp_socket_is_open(udp_socket)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
        native_family = AF_INET;
    } else if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
        native_family = AF_INET6;
    } else {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    {
        WSADATA data;
        SOCKET  native_handle;
        BOOL    reuse_address = TRUE;
        u_long  nonblocking   = 1u;

        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return UTP_INTERNAL_ERROR_IO;
        }
        udp_socket->winsock_active = true;
        native_handle              = socket(native_family, SOCK_DGRAM, IPPROTO_UDP);
        if (native_handle == INVALID_SOCKET || ioctlsocket(native_handle, FIONBIO, &nonblocking) != 0 ||
            setsockopt(native_handle, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_address,
                       (int)sizeof(reuse_address)) != 0) {
            if (native_handle != INVALID_SOCKET) {
                (void)closesocket(native_handle);
            }
            (void)WSACleanup();
            udp_socket->winsock_active = false;
            return UTP_INTERNAL_ERROR_IO;
        }
        udp_socket->native_handle = (uintptr_t)native_handle;
    }
#else
    {
        int native_handle = socket(native_family, SOCK_DGRAM, 0);
        int flags;
        int reuse_address = 1;

        if (native_handle < 0) {
            return utp_internal_error_from_errno(errno);
        }
        flags = fcntl(native_handle, F_GETFL, 0);
        if (flags < 0 || fcntl(native_handle, F_SETFL, flags | O_NONBLOCK) < 0) {
            int32_t system_error = errno;

            (void)close(native_handle);
            return utp_internal_error_from_errno(system_error);
        }
        if (setsockopt(native_handle, SOL_SOCKET, SO_REUSEADDR, &reuse_address, (socklen_t)sizeof(reuse_address)) < 0) {
            int32_t system_error = errno;

            (void)close(native_handle);
            return utp_internal_error_from_errno(system_error);
        }
        udp_socket->native_handle = (uintptr_t)native_handle;
    }
#endif
    utp_internal_error_t error = utp_udp_socket_enable_dont_fragment(udp_socket, address_family);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_udp_socket_close(udp_socket);
        return error;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_udp_socket_bind(utp_udp_socket_t* udp_socket, const utp_address_t* requested,
                                         const char* ifname, utp_address_t* local)
{
    if (!utp_udp_socket_is_open(udp_socket) || requested == NULL || local == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    struct sockaddr_storage storage;
    size_t                  storage_length;
    utp_internal_error_t    error = utp_address_to_sockaddr(requested, &storage, &storage_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_udp_socket_bind_interface(udp_socket, requested->family, ifname);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
#if defined(_WIN32)
    {
        int    actual_length = (int)sizeof(storage);
        SOCKET native_handle = (SOCKET)udp_socket->native_handle;

        if (requested->family == UTP_ADDRESS_FAMILY_IPV6 && !utp_address_is_unspecified_ipv6(requested)) {
            BOOL ipv6_only = TRUE;

            if (setsockopt(native_handle, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&ipv6_only, (int)sizeof(ipv6_only)) !=
                0) {
                return UTP_INTERNAL_ERROR_IO;
            }
        }
        if (bind(native_handle, (const struct sockaddr*)&storage, (int)storage_length) != 0 ||
            getsockname(native_handle, (struct sockaddr*)&storage, &actual_length) != 0) {
            return UTP_INTERNAL_ERROR_IO;
        }
        storage_length = (size_t)actual_length;
    }
#else
    {
        socklen_t actual_length = (socklen_t)sizeof(storage);
        int       native_handle = (int)udp_socket->native_handle;

        if (requested->family == UTP_ADDRESS_FAMILY_IPV6 && !utp_address_is_unspecified_ipv6(requested)) {
            int ipv6_only = 1;

            if (setsockopt(native_handle, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, (socklen_t)sizeof(ipv6_only)) < 0) {
                return utp_internal_error_from_errno(errno);
            }
        }
        if (bind(native_handle, (const struct sockaddr*)&storage, (socklen_t)storage_length) != 0 ||
            getsockname(native_handle, (struct sockaddr*)&storage, &actual_length) != 0) {
            return utp_internal_error_from_errno(errno);
        }
        storage_length = (size_t)actual_length;
    }
#endif
    error = utp_address_from_sockaddr(local, (const struct sockaddr*)&storage, storage_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        udp_socket->local_port = local->port;
        error                  = utp_udp_socket_enable_packet_info(udp_socket, requested->family);
    }
    return error;
}

utp_internal_error_t utp_udp_socket_send_from_to(utp_udp_socket_t* udp_socket, const void* data, size_t data_length,
                                                 const utp_address_t* peer, const utp_address_t* local,
                                                 size_t* sent_length)
{
    struct sockaddr_storage storage;
    size_t                  storage_length;
    utp_internal_error_t    error;

    if (sent_length != NULL) {
        *sent_length = 0u;
    }
    if (!utp_udp_socket_is_open(udp_socket) || (data == NULL && data_length != 0u) || peer == NULL ||
        sent_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_address_to_sockaddr(peer, &storage, &storage_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
#if defined(_WIN32)
    {
        SOCKET native_handle = (SOCKET)udp_socket->native_handle;
        int    written;

        (void)local;

        if (data_length > (size_t)INT_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        written = sendto(native_handle, (const char*)data, (int)data_length, 0, (const struct sockaddr*)&storage,
                         (int)storage_length);
        if (written == SOCKET_ERROR) {
            int32_t system_error = WSAGetLastError();

            if (system_error == WSAEWOULDBLOCK) {
                return UTP_INTERNAL_ERROR_WOULD_BLOCK;
            }
            if (system_error == WSAENOBUFS) {
                return UTP_INTERNAL_ERROR_NOBUFS;
            }
            return system_error == WSAEMSGSIZE ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_IO;
        }
        if ((size_t)written != data_length) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *sent_length = (size_t)written;
    }
#else
    {
        struct iovec             iov;
        struct msghdr            message       = {0};
        utp_udp_socket_control_t control       = {0};
        int                      native_handle = (int)udp_socket->native_handle;
        ssize_t                  written;

        iov.iov_base        = (void*)data;
        iov.iov_len         = data_length;
        message.msg_name    = &storage;
        message.msg_namelen = (socklen_t)storage_length;
        message.msg_iov     = &iov;
        message.msg_iovlen  = 1u;
        if (utp_udp_socket_source_is_usable(local, peer)) {
            struct cmsghdr* cmsg;

            message.msg_control    = control.bytes;
            message.msg_controllen = sizeof(control.bytes);
            cmsg                   = CMSG_FIRSTHDR(&message);
            cmsg->cmsg_level       = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IPPROTO_IP : IPPROTO_IPV6;
#if defined(__APPLE__)
            cmsg->cmsg_type = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IP_PKTINFO : IPV6_PKTINFO;
            if (peer->family == UTP_ADDRESS_FAMILY_IPV4) {
                struct in_pktinfo* info = (struct in_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len    = CMSG_LEN(sizeof(*info));
                info->ipi_ifindex = local->scope_id;
                memcpy(&info->ipi_spec_dst, local->address, 4u);
            } else {
                struct in6_pktinfo* info = (struct in6_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
                memcpy(&info->ipi6_addr, local->address, 16u);
                info->ipi6_ifindex = local->scope_id;
            }
#else
            cmsg->cmsg_type = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IP_PKTINFO : IPV6_PKTINFO;
            if (peer->family == UTP_ADDRESS_FAMILY_IPV4) {
                struct in_pktinfo* info = (struct in_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len    = CMSG_LEN(sizeof(*info));
                info->ipi_ifindex = (int)local->scope_id;
                memcpy(&info->ipi_spec_dst, local->address, 4u);
            } else {
                struct in6_pktinfo* info = (struct in6_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
                memcpy(&info->ipi6_addr, local->address, 16u);
                info->ipi6_ifindex = local->scope_id;
            }
#endif
            message.msg_controllen = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? CMSG_SPACE(sizeof(struct in_pktinfo))
                                                                             : CMSG_SPACE(sizeof(struct in6_pktinfo));
        }
        written = sendmsg(native_handle, &message, 0);

        if (written < 0) {
            int32_t system_error = errno;

            if (system_error == ENOBUFS) {
                return UTP_INTERNAL_ERROR_NOBUFS;
            }
            return utp_udp_socket_is_would_block_error(system_error) ? UTP_INTERNAL_ERROR_WOULD_BLOCK
                                                                     : utp_internal_error_from_errno(system_error);
        }
        if ((size_t)written != data_length) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *sent_length = (size_t)written;
    }
#endif
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_udp_socket_send_to(utp_udp_socket_t* udp_socket, const void* data, size_t data_length,
                                            const utp_address_t* peer, size_t* sent_length)
{
    return utp_udp_socket_send_from_to(udp_socket, data, data_length, peer, NULL, sent_length);
}

utp_internal_error_t utp_udp_socket_send_from_to_slices(utp_udp_socket_t*           udp_socket,
                                                        const utp_udp_send_slice_t* slices, size_t slice_count,
                                                        const utp_address_t* peer, const utp_address_t* local,
                                                        size_t* sent_length)
{
    struct sockaddr_storage storage;
    size_t                  storage_length;
    size_t                  total_length;
    utp_internal_error_t    error;

    if (sent_length != NULL) {
        *sent_length = 0u;
    }
    if (!utp_udp_socket_is_open(udp_socket) || peer == NULL || sent_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_udp_socket_validate_slices(slices, slice_count, &total_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (slice_count == 1u) {
        return utp_udp_socket_send_from_to(udp_socket, slices[0].data, slices[0].length, peer, local, sent_length);
    }
    error = utp_address_to_sockaddr(peer, &storage, &storage_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
#if defined(_WIN32)
    {
        WSABUF bufs[UTP_UDP_SOCKET_MAX_SEND_SLICES];
        SOCKET native_handle = (SOCKET)udp_socket->native_handle;
        DWORD  bytes_sent    = 0u;
        size_t index;

        (void)local;

        if (total_length > (size_t)ULONG_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        for (index = 0u; index < slice_count; ++index) {
            if (slices[index].length > (size_t)ULONG_MAX) {
                return UTP_INTERNAL_ERROR_LIMIT;
            }
            bufs[index].buf = (CHAR*)slices[index].data;
            bufs[index].len = (ULONG)slices[index].length;
        }
        if (WSASendTo(native_handle, bufs, (DWORD)slice_count, &bytes_sent, 0, (const struct sockaddr*)&storage,
                      (int)storage_length, NULL, NULL) == SOCKET_ERROR) {
            int32_t system_error = WSAGetLastError();

            if (system_error == WSAEWOULDBLOCK) {
                return UTP_INTERNAL_ERROR_WOULD_BLOCK;
            }
            if (system_error == WSAENOBUFS) {
                return UTP_INTERNAL_ERROR_NOBUFS;
            }
            return system_error == WSAEMSGSIZE ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_IO;
        }
        if ((size_t)bytes_sent != total_length) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *sent_length = (size_t)bytes_sent;
    }
#else
    {
        struct iovec             iov[UTP_UDP_SOCKET_MAX_SEND_SLICES];
        struct msghdr            message       = {0};
        utp_udp_socket_control_t control       = {0};
        int                      native_handle = (int)udp_socket->native_handle;
        ssize_t                  written;
        size_t                   index;

        if (total_length > (size_t)SSIZE_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        for (index = 0u; index < slice_count; ++index) {
            iov[index].iov_base = (void*)slices[index].data;
            iov[index].iov_len  = slices[index].length;
        }
        message.msg_name    = &storage;
        message.msg_namelen = (socklen_t)storage_length;
        message.msg_iov     = iov;
        // glibc 使用 size_t，musl 与部分 BSD 使用 int；slice_count 已限制为 8。
#if defined(__GLIBC__)
        message.msg_iovlen = slice_count;
#else
        message.msg_iovlen = (int)slice_count;
#endif
        if (utp_udp_socket_source_is_usable(local, peer)) {
            struct cmsghdr* cmsg;

            message.msg_control    = control.bytes;
            message.msg_controllen = sizeof(control.bytes);
            cmsg                   = CMSG_FIRSTHDR(&message);
            cmsg->cmsg_level       = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IPPROTO_IP : IPPROTO_IPV6;
#if defined(__APPLE__)
            cmsg->cmsg_type = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IP_PKTINFO : IPV6_PKTINFO;
            if (peer->family == UTP_ADDRESS_FAMILY_IPV4) {
                struct in_pktinfo* info = (struct in_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len    = CMSG_LEN(sizeof(*info));
                info->ipi_ifindex = local->scope_id;
                memcpy(&info->ipi_spec_dst, local->address, 4u);
            } else {
                struct in6_pktinfo* info = (struct in6_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
                memcpy(&info->ipi6_addr, local->address, 16u);
                info->ipi6_ifindex = local->scope_id;
            }
#else
            cmsg->cmsg_type = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IP_PKTINFO : IPV6_PKTINFO;
            if (peer->family == UTP_ADDRESS_FAMILY_IPV4) {
                struct in_pktinfo* info = (struct in_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len    = CMSG_LEN(sizeof(*info));
                info->ipi_ifindex = (int)local->scope_id;
                memcpy(&info->ipi_spec_dst, local->address, 4u);
            } else {
                struct in6_pktinfo* info = (struct in6_pktinfo*)CMSG_DATA(cmsg);

                cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
                memcpy(&info->ipi6_addr, local->address, 16u);
                info->ipi6_ifindex = local->scope_id;
            }
#endif
            message.msg_controllen = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? CMSG_SPACE(sizeof(struct in_pktinfo))
                                                                             : CMSG_SPACE(sizeof(struct in6_pktinfo));
        }
        written = sendmsg(native_handle, &message, 0);
        if (written < 0) {
            int32_t system_error = errno;

            if (system_error == ENOBUFS) {
                return UTP_INTERNAL_ERROR_NOBUFS;
            }
            return utp_udp_socket_is_would_block_error(system_error) ? UTP_INTERNAL_ERROR_WOULD_BLOCK
                                                                     : utp_internal_error_from_errno(system_error);
        }
        if ((size_t)written != total_length) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *sent_length = (size_t)written;
    }
#endif
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_udp_socket_send_to_slices(utp_udp_socket_t* udp_socket, const utp_udp_send_slice_t* slices,
                                                   size_t slice_count, const utp_address_t* peer, size_t* sent_length)
{
    return utp_udp_socket_send_from_to_slices(udp_socket, slices, slice_count, peer, NULL, sent_length);
}

utp_internal_error_t utp_udp_socket_recv_from_ex(utp_udp_socket_t* udp_socket, void* data, size_t capacity,
                                                 size_t* received_length, utp_address_t* peer, utp_address_t* local)
{
    struct sockaddr_storage storage;
    utp_address_t           parsed_peer;
    utp_address_t           parsed_local = {0};

    if (received_length != NULL) {
        *received_length = 0u;
    }
    if (!utp_udp_socket_is_open(udp_socket) || data == NULL || capacity == 0u || received_length == NULL ||
        peer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    {
        SOCKET native_handle  = (SOCKET)udp_socket->native_handle;
        int    storage_length = (int)sizeof(storage);
        int    received;

        if (capacity > (size_t)INT_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        received = recvfrom(native_handle, (char*)data, (int)capacity, 0, (struct sockaddr*)&storage, &storage_length);
        if (received == SOCKET_ERROR) {
            int32_t system_error = WSAGetLastError();

            if (system_error == WSAEWOULDBLOCK) {
                return UTP_INTERNAL_ERROR_WOULD_BLOCK;
            }
            return system_error == WSAEMSGSIZE ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_IO;
        }
        if (utp_address_from_sockaddr(&parsed_peer, (const struct sockaddr*)&storage, (size_t)storage_length) !=
            UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *received_length = (size_t)received;
    }
#else
    {
        int                      native_handle = (int)udp_socket->native_handle;
        struct iovec             iov;
        struct msghdr            message = {0};
        utp_udp_socket_control_t control = {0};
        ssize_t                  received;

        memset(&storage, 0, sizeof(storage));
        iov.iov_base           = data;
        iov.iov_len            = capacity;
        message.msg_name       = &storage;
        message.msg_namelen    = (socklen_t)sizeof(storage);
        message.msg_iov        = &iov;
        message.msg_iovlen     = 1u;
        message.msg_control    = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        received               = recvmsg(native_handle, &message, 0);
        if (received < 0) {
            int32_t system_error = errno;

            return utp_udp_socket_is_would_block_error(system_error) ? UTP_INTERNAL_ERROR_WOULD_BLOCK
                                                                     : utp_internal_error_from_errno(system_error);
        }
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        if (utp_address_from_sockaddr(&parsed_peer, (const struct sockaddr*)&storage, (size_t)message.msg_namelen) !=
            UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_IO;
        }
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != NULL;
             cmsg                 = utp_udp_socket_next_control(&message, cmsg)) {
            if (parsed_peer.family == UTP_ADDRESS_FAMILY_IPV4 && cmsg->cmsg_level == IPPROTO_IP) {
#if defined(__APPLE__) && defined(IP_PKTINFO)
                if (cmsg->cmsg_type == IP_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
                    const struct in_pktinfo* info = (const struct in_pktinfo*)CMSG_DATA(cmsg);

                    parsed_local.family   = UTP_ADDRESS_FAMILY_IPV4;
                    parsed_local.port     = udp_socket->local_port;
                    parsed_local.scope_id = info->ipi_ifindex;
                    memcpy(parsed_local.address, &info->ipi_addr, 4u);
                    break;
                }
#elif defined(IP_PKTINFO)
                if (cmsg->cmsg_type == IP_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
                    const struct in_pktinfo* info = (const struct in_pktinfo*)CMSG_DATA(cmsg);

                    parsed_local.family   = UTP_ADDRESS_FAMILY_IPV4;
                    parsed_local.port     = udp_socket->local_port;
                    parsed_local.scope_id = (uint32_t)info->ipi_ifindex;
                    memcpy(parsed_local.address, &info->ipi_addr, 4u);
                    break;
                }
#endif
            }
            if (parsed_peer.family == UTP_ADDRESS_FAMILY_IPV6 && cmsg->cmsg_level == IPPROTO_IPV6 &&
                cmsg->cmsg_type == IPV6_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
                const struct in6_pktinfo* info = (const struct in6_pktinfo*)CMSG_DATA(cmsg);

                parsed_local.family   = UTP_ADDRESS_FAMILY_IPV6;
                parsed_local.port     = udp_socket->local_port;
                parsed_local.scope_id = info->ipi6_ifindex;
                memcpy(parsed_local.address, &info->ipi6_addr, 16u);
                break;
            }
        }
        *received_length = (size_t)received;
    }
#endif
    *peer = parsed_peer;
    if (local != NULL) {
        *local = parsed_local;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_udp_socket_recv_from(utp_udp_socket_t* udp_socket, void* data, size_t capacity,
                                              size_t* received_length, utp_address_t* peer)
{
    return utp_udp_socket_recv_from_ex(udp_socket, data, capacity, received_length, peer, NULL);
}
