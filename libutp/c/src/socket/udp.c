#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "socket/udp.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
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

static bool utp_udp_socket_source_is_usable(const utp_address_t* local, const utp_address_t* peer);

#if defined(_WIN32)
#define UTP_UDP_SOCKET_CONTROL_CAPACITY WSA_CMSG_SPACE(sizeof(IN6_PKTINFO))

typedef union utp_udp_socket_control {
    WSACMSGHDR alignment;
    uint8_t    bytes[UTP_UDP_SOCKET_CONTROL_CAPACITY];
} utp_udp_socket_control_t;

static utp_internal_error_t utp_udp_socket_error_from_wsa(int32_t system_error)
{
    if (system_error == WSAEWOULDBLOCK) {
        return UTP_INTERNAL_ERROR_WOULD_BLOCK;
    }
    if (system_error == WSAENOBUFS) {
        return UTP_INTERNAL_ERROR_NOBUFS;
    }
    if (system_error == WSAEMSGSIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    return UTP_INTERNAL_ERROR_IO;
}

static utp_internal_error_t utp_udp_socket_load_extension_functions(utp_udp_socket_t* udp_socket)
{
    SOCKET native_handle;
    DWORD  bytes_returned = 0u;

    assert(udp_socket != NULL);
    assert(utp_udp_socket_is_open(udp_socket));
    if (udp_socket->wsa_recv_msg != NULL && udp_socket->wsa_send_msg != NULL) {
        return UTP_INTERNAL_ERROR_OK;
    }
    native_handle = (SOCKET)udp_socket->native_handle;
    if (udp_socket->wsa_recv_msg == NULL) {
        GUID            guid = WSAID_WSARECVMSG;
        LPFN_WSARECVMSG fn   = NULL;

        if (WSAIoctl(native_handle, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, (DWORD)sizeof(guid), &fn,
                     (DWORD)sizeof(fn), &bytes_returned, NULL, NULL) == SOCKET_ERROR ||
            fn == NULL) {
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
        }
        udp_socket->wsa_recv_msg = fn;
    }
    if (udp_socket->wsa_send_msg == NULL) {
        GUID            guid = WSAID_WSASENDMSG;
        LPFN_WSASENDMSG fn   = NULL;

        if (WSAIoctl(native_handle, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, (DWORD)sizeof(guid), &fn,
                     (DWORD)sizeof(fn), &bytes_returned, NULL, NULL) == SOCKET_ERROR ||
            fn == NULL) {
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
        }
        udp_socket->wsa_send_msg = fn;
    }
    return UTP_INTERNAL_ERROR_OK;
}

static bool utp_udp_socket_interface_index_from_name(const char* ifname, uint32_t* out_index)
{
    NET_LUID    luid;
    NET_IFINDEX interface_index = 0u;

    assert(ifname != NULL);
    assert(out_index != NULL);
    if (ConvertInterfaceNameToLuidA(ifname, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToIndex(&luid, &interface_index) != NO_ERROR) {
        return false;
    }
    *out_index = (uint32_t)interface_index;
    return true;
}

static utp_internal_error_t utp_udp_socket_set_windows_pktinfo(WSAMSG* message, utp_udp_socket_control_t* control,
                                                               const utp_address_t* peer, const utp_address_t* local)
{
    WSACMSGHDR* cmsg;

    assert(message != NULL);
    assert(control != NULL);
    if (!utp_udp_socket_source_is_usable(local, peer)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    memset(control, 0, sizeof(*control));
    message->Control.buf = (CHAR*)control->bytes;
    if (peer->family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(IP_PKTINFO)
        IN_PKTINFO* info;

        message->Control.len = (ULONG)WSA_CMSG_SPACE(sizeof(*info));
        cmsg                 = WSA_CMSG_FIRSTHDR(message);
        if (cmsg == NULL) {
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
        }
        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type  = IP_PKTINFO;
        cmsg->cmsg_len   = WSA_CMSG_LEN(sizeof(*info));
        info             = (IN_PKTINFO*)WSA_CMSG_DATA(cmsg);
        memset(info, 0, sizeof(*info));
        info->ipi_ifindex = (ULONG)local->scope_id;
        memcpy(&info->ipi_addr, local->address, 4u);
        return UTP_INTERNAL_ERROR_OK;
#else
        return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
    }
    if (peer->family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_PKTINFO)
        IN6_PKTINFO* info;

        message->Control.len = (ULONG)WSA_CMSG_SPACE(sizeof(*info));
        cmsg                 = WSA_CMSG_FIRSTHDR(message);
        if (cmsg == NULL) {
            return UTP_INTERNAL_ERROR_UNSUPPORTED;
        }
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type  = IPV6_PKTINFO;
        cmsg->cmsg_len   = WSA_CMSG_LEN(sizeof(*info));
        info             = (IN6_PKTINFO*)WSA_CMSG_DATA(cmsg);
        memset(info, 0, sizeof(*info));
        memcpy(&info->ipi6_addr, local->address, 16u);
        info->ipi6_ifindex = (ULONG)local->scope_id;
        return UTP_INTERNAL_ERROR_OK;
#else
        return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
    }
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
}
#else
#define UTP_UDP_SOCKET_CONTROL_CAPACITY CMSG_SPACE(sizeof(struct in6_pktinfo))

typedef union utp_udp_socket_control {
    struct cmsghdr alignment;                               // 保证 cmsg 头与数据的自然对齐
    uint8_t        bytes[UTP_UDP_SOCKET_CONTROL_CAPACITY];  // 单个 IPv6 pktinfo 控制消息
} utp_udp_socket_control_t;

typedef union utp_udp_socket_batch_control {
    max_align_t alignment;                               // 保证批量 cmsg 缓冲的自然对齐
    uint8_t     bytes[UTP_UDP_SOCKET_CONTROL_CAPACITY];  // 单个 IPv6 pktinfo 控制消息
} utp_udp_socket_batch_control_t;

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

    assert(aligned_length != NULL);
    if (length > SIZE_MAX - (alignment - 1u)) {
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
    assert(udp_socket != NULL);
    assert(utp_udp_socket_is_open(udp_socket));
#if defined(_WIN32)
    BOOL                 enabled = TRUE;
    SOCKET               native_handle;
    utp_internal_error_t error;

    error = utp_udp_socket_load_extension_functions(udp_socket);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    native_handle = (SOCKET)udp_socket->native_handle;
    if (address_family == UTP_ADDRESS_FAMILY_IPV4) {
#if defined(IP_PKTINFO)
        return setsockopt(native_handle, IPPROTO_IP, IP_PKTINFO, (const char*)&enabled, (int)sizeof(enabled)) == 0
                   ? UTP_INTERNAL_ERROR_OK
                   : UTP_INTERNAL_ERROR_IO;
#else
        return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
    }
    if (address_family == UTP_ADDRESS_FAMILY_IPV6) {
#if defined(IPV6_PKTINFO)
        return setsockopt(native_handle, IPPROTO_IPV6, IPV6_PKTINFO, (const char*)&enabled, (int)sizeof(enabled)) == 0
                   ? UTP_INTERNAL_ERROR_OK
                   : UTP_INTERNAL_ERROR_IO;
#else
        return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
    }
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
#else
    const int enabled = 1;
    int       native_handle;

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
    assert(udp_socket != NULL);
    assert(utp_udp_socket_is_open(udp_socket));
#if defined(_WIN32)
    {
        uint32_t interface_index;
        SOCKET   native_handle;

        if (!utp_udp_socket_interface_index_from_name(ifname, &interface_index) || interface_index == 0u) {
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
    assert(udp_socket != NULL);
    assert(utp_udp_socket_is_open(udp_socket));
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
        udp_socket->native_handle = UTP_UDP_SOCKET_INVALID;
#if defined(_WIN32)
        udp_socket->wsa_recv_msg = NULL;
        udp_socket->wsa_send_msg = NULL;
#endif
        udp_socket->local_port = 0u;
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
        SOCKET native_handle;
        BOOL   reuse_address = TRUE;
        u_long nonblocking   = 1u;

        native_handle = socket(native_family, SOCK_DGRAM, IPPROTO_UDP);
        if (native_handle == INVALID_SOCKET || ioctlsocket(native_handle, FIONBIO, &nonblocking) != 0 ||
            setsockopt(native_handle, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_address,
                       (int)sizeof(reuse_address)) != 0) {
            if (native_handle != INVALID_SOCKET) {
                (void)closesocket(native_handle);
            }
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

        if (requested->family == UTP_ADDRESS_FAMILY_IPV6) {
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

        if (requested->family == UTP_ADDRESS_FAMILY_IPV6) {
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
        int    written       = 0;

        if (data_length > (size_t)INT_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        if (utp_udp_socket_source_is_usable(local, peer)) {
            WSABUF                   buf;
            WSAMSG                   message    = {0};
            DWORD                    bytes_sent = 0u;
            utp_udp_socket_control_t control;

            error = utp_udp_socket_load_extension_functions(udp_socket);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            buf.buf               = (CHAR*)data;
            buf.len               = (ULONG)data_length;
            message.name          = (struct sockaddr*)&storage;
            message.namelen       = (INT)storage_length;
            message.lpBuffers     = &buf;
            message.dwBufferCount = 1u;
            error                 = utp_udp_socket_set_windows_pktinfo(&message, &control, peer, local);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (udp_socket->wsa_send_msg(native_handle, &message, 0u, &bytes_sent, NULL, NULL) == SOCKET_ERROR) {
                return utp_udp_socket_error_from_wsa(WSAGetLastError());
            }
            if ((size_t)bytes_sent != data_length) {
                return UTP_INTERNAL_ERROR_IO;
            }
            *sent_length = (size_t)bytes_sent;
            return UTP_INTERNAL_ERROR_OK;
        }
        written = sendto(native_handle, (const char*)data, (int)data_length, 0, (const struct sockaddr*)&storage,
                         (int)storage_length);
        if (written == SOCKET_ERROR) {
            return utp_udp_socket_error_from_wsa(WSAGetLastError());
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
        if (utp_udp_socket_source_is_usable(local, peer)) {
            WSAMSG                   message = {0};
            utp_udp_socket_control_t control;

            error = utp_udp_socket_load_extension_functions(udp_socket);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            message.name          = (struct sockaddr*)&storage;
            message.namelen       = (INT)storage_length;
            message.lpBuffers     = bufs;
            message.dwBufferCount = (DWORD)slice_count;
            error                 = utp_udp_socket_set_windows_pktinfo(&message, &control, peer, local);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (udp_socket->wsa_send_msg(native_handle, &message, 0u, &bytes_sent, NULL, NULL) == SOCKET_ERROR) {
                return utp_udp_socket_error_from_wsa(WSAGetLastError());
            }
            if ((size_t)bytes_sent != total_length) {
                return UTP_INTERNAL_ERROR_IO;
            }
            *sent_length = (size_t)bytes_sent;
            return UTP_INTERNAL_ERROR_OK;
        }
        if (WSASendTo(native_handle, bufs, (DWORD)slice_count, &bytes_sent, 0, (const struct sockaddr*)&storage,
                      (int)storage_length, NULL, NULL) == SOCKET_ERROR) {
            return utp_udp_socket_error_from_wsa(WSAGetLastError());
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

utp_internal_error_t utp_udp_socket_send_messages(utp_udp_socket_t* udp_socket, utp_udp_send_message_t* messages,
                                                  size_t message_count, size_t* out_sent_count)
{
    if (out_sent_count != NULL) {
        *out_sent_count = 0u;
    }
    if (!utp_udp_socket_is_open(udp_socket) || messages == NULL || message_count == 0u ||
        message_count > UTP_UDP_SOCKET_BATCH_SIZE || out_sent_count == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#if defined(UTP_HAVE_SENDMMSG)
    {
        struct mmsghdr                 native_messages[UTP_UDP_SOCKET_BATCH_SIZE];
        struct sockaddr_storage        destinations[UTP_UDP_SOCKET_BATCH_SIZE]                           = {{0}};
        struct iovec                   iovecs[UTP_UDP_SOCKET_BATCH_SIZE][UTP_UDP_SOCKET_MAX_SEND_SLICES] = {{{0}}};
        utp_udp_socket_batch_control_t controls[UTP_UDP_SOCKET_BATCH_SIZE];
        size_t                         lengths[UTP_UDP_SOCKET_BATCH_SIZE];
        int32_t                        sent_count;
        const int                      native_handle = (int)udp_socket->native_handle;

        memset(native_messages, 0, sizeof(native_messages));
        memset(controls, 0, sizeof(controls));
        for (size_t index = 0u; index < message_count; ++index) {
            utp_udp_send_message_t* message        = &messages[index];
            struct msghdr*          native_message = &native_messages[index].msg_hdr;
            size_t                  destination_length;
            size_t                  total_length;
            utp_internal_error_t    error;

            message->sent_length = 0u;
            if (message->peer == NULL) {
                return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
            }
            error = utp_udp_socket_validate_slices(message->slices, message->slice_count, &total_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            error = utp_address_to_sockaddr(message->peer, &destinations[index], &destination_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            lengths[index]              = total_length;
            native_message->msg_name    = &destinations[index];
            native_message->msg_namelen = (socklen_t)destination_length;
            native_message->msg_iov     = iovecs[index];
            // glibc 使用 size_t，musl 使用 int；slice_count 已限制为 8。
#if defined(__GLIBC__)
            native_message->msg_iovlen = message->slice_count;
#else
            native_message->msg_iovlen = (int)message->slice_count;
#endif
            for (size_t slice_index = 0u; slice_index < message->slice_count; ++slice_index) {
                iovecs[index][slice_index].iov_base = (void*)message->slices[slice_index].data;
                iovecs[index][slice_index].iov_len  = message->slices[slice_index].length;
            }
            if (utp_udp_socket_source_is_usable(message->local, message->peer)) {
                struct cmsghdr* cmsg;

                native_message->msg_control    = controls[index].bytes;
                native_message->msg_controllen = sizeof(controls[index].bytes);
                cmsg                           = CMSG_FIRSTHDR(native_message);
                cmsg->cmsg_level = message->peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IPPROTO_IP : IPPROTO_IPV6;
                cmsg->cmsg_type  = message->peer->family == UTP_ADDRESS_FAMILY_IPV4 ? IP_PKTINFO : IPV6_PKTINFO;
                if (message->peer->family == UTP_ADDRESS_FAMILY_IPV4) {
                    struct in_pktinfo* info = (struct in_pktinfo*)CMSG_DATA(cmsg);

                    cmsg->cmsg_len    = CMSG_LEN(sizeof(*info));
                    info->ipi_ifindex = (int)message->local->scope_id;
                    memcpy(&info->ipi_spec_dst, message->local->address, 4u);
                    native_message->msg_controllen = CMSG_SPACE(sizeof(*info));
                } else {
                    struct in6_pktinfo* info = (struct in6_pktinfo*)CMSG_DATA(cmsg);

                    cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
                    memcpy(&info->ipi6_addr, message->local->address, 16u);
                    info->ipi6_ifindex             = message->local->scope_id;
                    native_message->msg_controllen = CMSG_SPACE(sizeof(*info));
                }
            }
        }
        sent_count = sendmmsg(native_handle, native_messages, (unsigned int)message_count, MSG_DONTWAIT);
        if (sent_count < 0) {
            const int32_t system_error = errno;

            if (system_error == ENOBUFS) {
                return UTP_INTERNAL_ERROR_NOBUFS;
            }
            return utp_udp_socket_is_would_block_error(system_error) ? UTP_INTERNAL_ERROR_WOULD_BLOCK
                                                                     : utp_internal_error_from_errno(system_error);
        }
        for (size_t index = 0u; index < (size_t)sent_count; ++index) {
            if ((size_t)native_messages[index].msg_len != lengths[index]) {
                *out_sent_count = index;
                return UTP_INTERNAL_ERROR_IO;
            }
            messages[index].sent_length = (size_t)native_messages[index].msg_len;
        }
        *out_sent_count = (size_t)sent_count;
        return UTP_INTERNAL_ERROR_OK;
    }
#else
    (void)messages;
    return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
}

utp_internal_error_t utp_udp_socket_receive_messages(utp_udp_socket_t* udp_socket, utp_udp_receive_message_t* messages,
                                                     size_t message_count, size_t* out_received_count)
{
    if (out_received_count != NULL) {
        *out_received_count = 0u;
    }
    if (!utp_udp_socket_is_open(udp_socket) || messages == NULL || message_count == 0u ||
        message_count > UTP_UDP_SOCKET_BATCH_SIZE || out_received_count == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
#if defined(UTP_HAVE_RECVMMSG)
    {
        struct mmsghdr                 native_messages[UTP_UDP_SOCKET_BATCH_SIZE];
        struct sockaddr_storage        sources[UTP_UDP_SOCKET_BATCH_SIZE] = {{0}};
        struct iovec                   iovecs[UTP_UDP_SOCKET_BATCH_SIZE]  = {{0}};
        utp_udp_socket_batch_control_t controls[UTP_UDP_SOCKET_BATCH_SIZE];
        const int                      native_handle = (int)udp_socket->native_handle;
        int32_t                        received_count;

        memset(native_messages, 0, sizeof(native_messages));
        memset(controls, 0, sizeof(controls));
        for (size_t index = 0u; index < message_count; ++index) {
            struct msghdr* native_message = &native_messages[index].msg_hdr;

            messages[index].received_length = 0u;
            messages[index].peer            = (utp_address_t){0};
            messages[index].local           = (utp_address_t){0};
            messages[index].error           = UTP_INTERNAL_ERROR_OK;
            if (messages[index].data == NULL || messages[index].capacity == 0u) {
                return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
            }
            iovecs[index].iov_base         = messages[index].data;
            iovecs[index].iov_len          = messages[index].capacity;
            native_message->msg_name       = &sources[index];
            native_message->msg_namelen    = (socklen_t)sizeof(sources[index]);
            native_message->msg_iov        = &iovecs[index];
            native_message->msg_iovlen     = 1u;
            native_message->msg_control    = controls[index].bytes;
            native_message->msg_controllen = sizeof(controls[index].bytes);
        }
        received_count = recvmmsg(native_handle, native_messages, (unsigned int)message_count, MSG_DONTWAIT, NULL);
        if (received_count < 0) {
            const int32_t system_error = errno;

            return utp_udp_socket_is_would_block_error(system_error) ? UTP_INTERNAL_ERROR_WOULD_BLOCK
                                                                     : utp_internal_error_from_errno(system_error);
        }
        *out_received_count = (size_t)received_count;
        for (size_t index = 0u; index < (size_t)received_count; ++index) {
            struct msghdr* native_message = &native_messages[index].msg_hdr;
            utp_address_t  parsed_peer;
            utp_address_t  parsed_local = {0};

            if ((native_message->msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
                messages[index].error = UTP_INTERNAL_ERROR_OVERFLOW;
                continue;
            }
            if (utp_address_from_sockaddr(&parsed_peer, (const struct sockaddr*)&sources[index],
                                          (size_t)native_message->msg_namelen) != UTP_INTERNAL_ERROR_OK) {
                messages[index].error = UTP_INTERNAL_ERROR_IO;
                continue;
            }
            for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(native_message); cmsg != NULL;
                 cmsg                 = utp_udp_socket_next_control(native_message, cmsg)) {
                if (parsed_peer.family == UTP_ADDRESS_FAMILY_IPV4 && cmsg->cmsg_level == IPPROTO_IP &&
                    cmsg->cmsg_type == IP_PKTINFO && cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
                    const struct in_pktinfo* info = (const struct in_pktinfo*)CMSG_DATA(cmsg);

                    parsed_local.family   = UTP_ADDRESS_FAMILY_IPV4;
                    parsed_local.port     = udp_socket->local_port;
                    parsed_local.scope_id = (uint32_t)info->ipi_ifindex;
                    memcpy(parsed_local.address, &info->ipi_addr, 4u);
                    break;
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
            messages[index].received_length = (size_t)native_messages[index].msg_len;
            messages[index].peer            = parsed_peer;
            messages[index].local           = parsed_local;
        }
        return UTP_INTERNAL_ERROR_OK;
    }
#else
    (void)messages;
    return UTP_INTERNAL_ERROR_UNSUPPORTED;
#endif
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
        SOCKET                   native_handle = (SOCKET)udp_socket->native_handle;
        WSABUF                   buf;
        WSAMSG                   message        = {0};
        DWORD                    bytes_received = 0u;
        utp_udp_socket_control_t control        = {0};
        utp_internal_error_t     error;

        if (capacity > (size_t)ULONG_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        error = utp_udp_socket_load_extension_functions(udp_socket);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        memset(&storage, 0, sizeof(storage));
        buf.buf               = (CHAR*)data;
        buf.len               = (ULONG)capacity;
        message.name          = (struct sockaddr*)&storage;
        message.namelen       = (INT)sizeof(storage);
        message.lpBuffers     = &buf;
        message.dwBufferCount = 1u;
        message.Control.buf   = (CHAR*)control.bytes;
        message.Control.len   = (ULONG)sizeof(control.bytes);
        if (udp_socket->wsa_recv_msg(native_handle, &message, &bytes_received, NULL, NULL) == SOCKET_ERROR) {
            return utp_udp_socket_error_from_wsa(WSAGetLastError());
        }
#if defined(MSG_TRUNC)
        if ((message.dwFlags & MSG_TRUNC) != 0u) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
#endif
#if defined(MSG_CTRUNC)
        if ((message.dwFlags & MSG_CTRUNC) != 0u) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
#endif
        if (bytes_received > UINT16_MAX) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        if (utp_address_from_sockaddr(&parsed_peer, (const struct sockaddr*)&storage, (size_t)message.namelen) !=
            UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_IO;
        }
        if (local != NULL) {
            bool found_local = false;

            for (WSACMSGHDR* cmsg = WSA_CMSG_FIRSTHDR(&message); cmsg != NULL; cmsg = WSA_CMSG_NXTHDR(&message, cmsg)) {
#if defined(IP_PKTINFO)
                if (parsed_peer.family == UTP_ADDRESS_FAMILY_IPV4 && cmsg->cmsg_level == IPPROTO_IP &&
                    cmsg->cmsg_type == IP_PKTINFO) {
                    const IN_PKTINFO* info;

                    if (cmsg->cmsg_len < WSA_CMSG_LEN(sizeof(*info))) {
                        return UTP_INTERNAL_ERROR_OVERFLOW;
                    }
                    info                  = (const IN_PKTINFO*)WSA_CMSG_DATA(cmsg);
                    parsed_local.family   = UTP_ADDRESS_FAMILY_IPV4;
                    parsed_local.port     = udp_socket->local_port;
                    parsed_local.scope_id = (uint32_t)info->ipi_ifindex;
                    memcpy(parsed_local.address, &info->ipi_addr, 4u);
                    found_local = true;
                    break;
                }
#endif
#if defined(IPV6_PKTINFO)
                if (parsed_peer.family == UTP_ADDRESS_FAMILY_IPV6 && cmsg->cmsg_level == IPPROTO_IPV6 &&
                    cmsg->cmsg_type == IPV6_PKTINFO) {
                    const IN6_PKTINFO* info;

                    if (cmsg->cmsg_len < WSA_CMSG_LEN(sizeof(*info))) {
                        return UTP_INTERNAL_ERROR_OVERFLOW;
                    }
                    info                  = (const IN6_PKTINFO*)WSA_CMSG_DATA(cmsg);
                    parsed_local.family   = UTP_ADDRESS_FAMILY_IPV6;
                    parsed_local.port     = udp_socket->local_port;
                    parsed_local.scope_id = (uint32_t)info->ipi6_ifindex;
                    memcpy(parsed_local.address, &info->ipi6_addr, 16u);
                    found_local = true;
                    break;
                }
#endif
            }
            if (!found_local || !utp_udp_socket_source_is_usable(&parsed_local, &parsed_peer)) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
        }
        *received_length = (size_t)bytes_received;
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
