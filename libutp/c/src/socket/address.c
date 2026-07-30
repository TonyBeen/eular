#include "socket/address.h"

#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

static const uint8_t k_unspecified_ipv6_address[16] = {0};

utp_internal_error_t utp_address_parse(utp_address_t *address, const char *text, uint16_t port) {
    utp_address_t parsed;

    if (address == NULL || text == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(&parsed, 0, sizeof(parsed));
    if (inet_pton(AF_INET, text, parsed.address) == 1) {
        parsed.family = UTP_ADDRESS_FAMILY_IPV4;
    } else if (inet_pton(AF_INET6, text, parsed.address) == 1) {
        parsed.family = UTP_ADDRESS_FAMILY_IPV6;
    } else {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    parsed.port = port;
    *address    = parsed;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_address_from_sockaddr(utp_address_t *address, const struct sockaddr *socket_address,
                                               size_t socket_address_length) {
    utp_address_t parsed;

    if (address == NULL || socket_address == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(&parsed, 0, sizeof(parsed));
    if (socket_address->sa_family == AF_INET && socket_address_length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)socket_address;

        parsed.family = UTP_ADDRESS_FAMILY_IPV4;
        parsed.port   = ntohs(ipv4->sin_port);
        memcpy(parsed.address, &ipv4->sin_addr, 4u);
    } else if (socket_address->sa_family == AF_INET6 && socket_address_length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)socket_address;

        parsed.family   = UTP_ADDRESS_FAMILY_IPV6;
        parsed.port     = ntohs(ipv6->sin6_port);
        parsed.scope_id = ipv6->sin6_scope_id;
        memcpy(parsed.address, &ipv6->sin6_addr, 16u);
    } else {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *address = parsed;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_address_to_sockaddr(const utp_address_t *address, struct sockaddr_storage *storage,
                                             size_t *storage_length) {
    if (address == NULL || storage == NULL || storage_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(storage, 0, sizeof(*storage));
    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)storage;

        ipv4->sin_family = AF_INET;
        ipv4->sin_port   = htons(address->port);
        memcpy(&ipv4->sin_addr, address->address, 4u);
        *storage_length = sizeof(*ipv4);
    } else if (address->family == UTP_ADDRESS_FAMILY_IPV6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)storage;

        ipv6->sin6_family   = AF_INET6;
        ipv6->sin6_port     = htons(address->port);
        ipv6->sin6_scope_id = address->scope_id;
        memcpy(&ipv6->sin6_addr, address->address, 16u);
        *storage_length = sizeof(*ipv6);
    } else {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return UTP_INTERNAL_ERROR_OK;
}

bool utp_address_equal(const utp_address_t *left, const utp_address_t *right) {
    size_t length;

    if (left == NULL || right == NULL || left->family != right->family || left->port != right->port ||
        left->scope_id != right->scope_id) {
        return false;
    }
    if (left->family == UTP_ADDRESS_FAMILY_IPV4) {
        length = 4u;
    } else if (left->family == UTP_ADDRESS_FAMILY_IPV6) {
        length = 16u;
    } else {
        return false;
    }
    return memcmp(left->address, right->address, length) == 0;
}

bool utp_address_is_unspecified_ipv6(const utp_address_t *address) {
    return address != NULL && address->family == UTP_ADDRESS_FAMILY_IPV6 &&
           memcmp(address->address, k_unspecified_ipv6_address, sizeof(k_unspecified_ipv6_address)) == 0;
}
