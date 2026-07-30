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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
static bool utp_udp_socket_is_would_block_error(int system_error) {
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
#endif

void utp_udp_socket_init(utp_udp_socket_t *udp_socket) {
    if (udp_socket != NULL) {
        udp_socket->native_handle  = UTP_UDP_SOCKET_INVALID;
        udp_socket->winsock_active = false;
    }
}

bool utp_udp_socket_is_open(const utp_udp_socket_t *udp_socket) {
    return udp_socket != NULL && udp_socket->native_handle != UTP_UDP_SOCKET_INVALID;
}

void utp_udp_socket_close(utp_udp_socket_t *udp_socket) {
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

utp_internal_error_t utp_udp_socket_open(utp_udp_socket_t *udp_socket, uint8_t address_family) {
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
            setsockopt(native_handle, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_address,
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
            int system_error = errno;

            (void)close(native_handle);
            return utp_internal_error_from_errno(system_error);
        }
        if (setsockopt(native_handle, SOL_SOCKET, SO_REUSEADDR, &reuse_address, (socklen_t)sizeof(reuse_address)) < 0) {
            int system_error = errno;

            (void)close(native_handle);
            return utp_internal_error_from_errno(system_error);
        }
        udp_socket->native_handle = (uintptr_t)native_handle;
    }
#endif
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_udp_socket_bind(utp_udp_socket_t *udp_socket, const utp_address_t *requested,
                                         utp_address_t *local) {
    struct sockaddr_storage storage;
    size_t                  storage_length;
    utp_internal_error_t    error;

    if (!utp_udp_socket_is_open(udp_socket) || requested == NULL || local == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_address_to_sockaddr(requested, &storage, &storage_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
#if defined(_WIN32)
    {
        int    actual_length = (int)sizeof(storage);
        SOCKET native_handle = (SOCKET)udp_socket->native_handle;

        if (requested->family == UTP_ADDRESS_FAMILY_IPV6 && !utp_address_is_unspecified_ipv6(requested)) {
            BOOL ipv6_only = TRUE;

            if (setsockopt(native_handle, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&ipv6_only,
                           (int)sizeof(ipv6_only)) != 0) {
                return UTP_INTERNAL_ERROR_IO;
            }
        }
        if (bind(native_handle, (const struct sockaddr *)&storage, (int)storage_length) != 0 ||
            getsockname(native_handle, (struct sockaddr *)&storage, &actual_length) != 0) {
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
        if (bind(native_handle, (const struct sockaddr *)&storage, (socklen_t)storage_length) != 0 ||
            getsockname(native_handle, (struct sockaddr *)&storage, &actual_length) != 0) {
            return utp_internal_error_from_errno(errno);
        }
        storage_length = (size_t)actual_length;
    }
#endif
    return utp_address_from_sockaddr(local, (const struct sockaddr *)&storage, storage_length);
}

utp_internal_error_t utp_udp_socket_send_to(utp_udp_socket_t *udp_socket, const void *data, size_t data_length,
                                            const utp_address_t *peer, size_t *sent_length) {
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

        if (data_length > (size_t)INT_MAX) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        written = sendto(native_handle, (const char *)data, (int)data_length, 0, (const struct sockaddr *)&storage,
                         (int)storage_length);
        if (written == SOCKET_ERROR) {
            int system_error = WSAGetLastError();

            return system_error == WSAEWOULDBLOCK ? UTP_INTERNAL_ERROR_WOULD_BLOCK : UTP_INTERNAL_ERROR_IO;
        }
        if ((size_t)written != data_length) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *sent_length = (size_t)written;
    }
#else
    {
        int     native_handle = (int)udp_socket->native_handle;
        ssize_t written =
            sendto(native_handle, data, data_length, 0, (const struct sockaddr *)&storage, (socklen_t)storage_length);

        if (written < 0) {
            int system_error = errno;

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

utp_internal_error_t utp_udp_socket_recv_from(utp_udp_socket_t *udp_socket, void *data, size_t capacity,
                                              size_t *received_length, utp_address_t *peer) {
    struct sockaddr_storage storage;
    utp_address_t           parsed_peer;

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
        received =
            recvfrom(native_handle, (char *)data, (int)capacity, 0, (struct sockaddr *)&storage, &storage_length);
        if (received == SOCKET_ERROR) {
            int system_error = WSAGetLastError();

            if (system_error == WSAEWOULDBLOCK) {
                return UTP_INTERNAL_ERROR_WOULD_BLOCK;
            }
            return system_error == WSAEMSGSIZE ? UTP_INTERNAL_ERROR_OVERFLOW : UTP_INTERNAL_ERROR_IO;
        }
        if (utp_address_from_sockaddr(&parsed_peer, (const struct sockaddr *)&storage, (size_t)storage_length) !=
            UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *received_length = (size_t)received;
    }
#else
    {
        int           native_handle = (int)udp_socket->native_handle;
        struct iovec  iov;
        struct msghdr message;
        ssize_t       received;

        memset(&storage, 0, sizeof(storage));
        memset(&message, 0, sizeof(message));
        iov.iov_base        = data;
        iov.iov_len         = capacity;
        message.msg_name    = &storage;
        message.msg_namelen = (socklen_t)sizeof(storage);
        message.msg_iov     = &iov;
        message.msg_iovlen  = 1u;
        received            = recvmsg(native_handle, &message, 0);
        if (received < 0) {
            int system_error = errno;

            return utp_udp_socket_is_would_block_error(system_error) ? UTP_INTERNAL_ERROR_WOULD_BLOCK
                                                                     : utp_internal_error_from_errno(system_error);
        }
        if ((message.msg_flags & MSG_TRUNC) != 0) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        if (utp_address_from_sockaddr(&parsed_peer, (const struct sockaddr *)&storage, (size_t)message.msg_namelen) !=
            UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_IO;
        }
        *received_length = (size_t)received;
    }
#endif
    *peer = parsed_peer;
    return UTP_INTERNAL_ERROR_OK;
}
