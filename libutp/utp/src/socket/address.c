#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "socket/address.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif
#endif

static const uint8_t k_unspecified_ipv6_address[16] = {0};

#if !defined(_WIN32)
typedef struct utp_address_candidate_entry {
    utp_address_t address;
    uint32_t      ifindex;
    uint32_t      address_flags;
    uint8_t       priority;
    bool          selected;
} utp_address_candidate_entry_t;

static bool utp_address_is_unspecified(const utp_address_t* address)
{
    assert(address != NULL);
    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        return address->address[0] == 0u && address->address[1] == 0u && address->address[2] == 0u &&
               address->address[3] == 0u;
    }
    return address->family == UTP_ADDRESS_FAMILY_IPV6 &&
           memcmp(address->address, k_unspecified_ipv6_address, sizeof(k_unspecified_ipv6_address)) == 0;
}

static bool utp_address_is_link_local(const utp_address_t* address)
{
    assert(address != NULL);
    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        return address->address[0] == 169u && address->address[1] == 254u;
    }
    return address->family == UTP_ADDRESS_FAMILY_IPV6 && address->address[0] == 0xfeu &&
           (address->address[1] & 0xc0u) == 0x80u;
}

static bool utp_address_is_multicast(const utp_address_t* address)
{
    assert(address != NULL);
    return (address->family == UTP_ADDRESS_FAMILY_IPV4 && (address->address[0] & 0xf0u) == 224u) ||
           (address->family == UTP_ADDRESS_FAMILY_IPV6 && address->address[0] == 0xffu);
}

static uint8_t utp_address_candidate_priority(const utp_address_candidate_entry_t* entry)
{
    assert(entry != NULL);
    if (entry->address.family == UTP_ADDRESS_FAMILY_IPV4) {
        const uint8_t first  = entry->address.address[0];
        const uint8_t second = entry->address.address[1];

        return first == 10u || first == 100u || first == 127u || (first == 172u && second >= 16u && second <= 31u) ||
                       (first == 192u && second == 168u)
                   ? 1u
                   : 0u;
    }
    if (entry->address.family == UTP_ADDRESS_FAMILY_IPV6) {
        uint8_t priority = (entry->address.address[0] & 0xfeu) == 0xfcu ? 2u : 0u;

#if defined(__linux__)
        if ((entry->address_flags & IFA_F_TEMPORARY) != 0u) priority = (uint8_t)(priority + 1u);
#endif
        return priority;
    }
    return UINT8_MAX;
}

#if defined(__linux__)
/** @brief 从 rtnetlink 补充地址 flags，用于过滤 IPv6 temporary/deprecated/tentative 地址。 */
static void utp_address_load_flags(utp_address_candidate_entry_t* entries, size_t entry_count)
{
    struct {
        struct nlmsghdr  header;
        struct ifaddrmsg address;
    } request                = {{0}, {0}};
    struct sockaddr_nl local = {0};
    struct sockaddr_nl peer  = {0};
    uint8_t            buffer[8192];
    int                socket_fd;
    uint32_t           sequence = 1u;
    bool               done     = false;

    assert(entries != NULL || entry_count == 0u);
    socket_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (socket_fd < 0) return;
    local.nl_family = AF_NETLINK;
    if (bind(socket_fd, (struct sockaddr*)&local, sizeof(local)) != 0) {
        close(socket_fd);
        return;
    }
    request.header.nlmsg_len   = NLMSG_LENGTH(sizeof(request.address));
    request.header.nlmsg_type  = RTM_GETADDR;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq   = sequence;
    request.address.ifa_family = entries[0].address.family == UTP_ADDRESS_FAMILY_IPV4 ? AF_INET : AF_INET6;
    peer.nl_family             = AF_NETLINK;
    if (sendto(socket_fd, &request, request.header.nlmsg_len, 0, (struct sockaddr*)&peer, sizeof(peer)) < 0) {
        close(socket_fd);
        return;
    }
    while (!done) {
        const ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
        size_t        offset;

        if (received < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (received == 0) break;
        for (offset = 0u; offset + sizeof(struct nlmsghdr) <= (size_t)received;) {
            struct nlmsghdr*  message        = (struct nlmsghdr*)(buffer + offset);
            const size_t      message_length = (size_t)message->nlmsg_len;
            struct ifaddrmsg* address_message;
            struct rtattr*    attribute;
            size_t            attribute_length;
            const void*       address_bytes = NULL;
            uint32_t          address_flags = 0u;

            if (message_length < sizeof(*message) || message_length > (size_t)received - offset) {
                done = true;
                break;
            }
            offset += (message_length + 3u) & ~(size_t)3u;
            if (message->nlmsg_seq != sequence) continue;
            if (message->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (message->nlmsg_type == NLMSG_ERROR) {
                done = true;
                break;
            }
            if (message_length < NLMSG_LENGTH(sizeof(struct ifaddrmsg))) continue;
            address_message = (struct ifaddrmsg*)NLMSG_DATA(message);
            if (address_message->ifa_family != request.address.ifa_family) continue;
            attribute        = IFA_RTA(address_message);
            attribute_length = message_length - (size_t)NLMSG_LENGTH(sizeof(*address_message));
            while (attribute_length >= sizeof(*attribute) && attribute->rta_len >= sizeof(*attribute) &&
                   (size_t)attribute->rta_len <= attribute_length) {
                if ((address_message->ifa_family == AF_INET && attribute->rta_type == IFA_LOCAL) ||
                    (address_message->ifa_family == AF_INET6 && attribute->rta_type == IFA_ADDRESS)) {
                    address_bytes = RTA_DATA(attribute);
                } else if (attribute->rta_type == IFA_FLAGS && RTA_PAYLOAD(attribute) >= sizeof(uint32_t)) {
                    memcpy(&address_flags, RTA_DATA(attribute), sizeof(address_flags));
                }
                const size_t attribute_size = ((size_t)attribute->rta_len + 3u) & ~(size_t)3u;

                if (attribute_size > attribute_length) break;
                attribute         = (struct rtattr*)((uint8_t*)attribute + attribute_size);
                attribute_length -= attribute_size;
            }
            if (address_bytes == NULL) continue;
            for (size_t index = 0u; index < entry_count; ++index) {
                const size_t address_length = entries[index].address.family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;

                if (entries[index].ifindex == address_message->ifa_index &&
                    memcmp(entries[index].address.address, address_bytes, address_length) == 0) {
                    entries[index].address_flags = address_flags != 0u ? address_flags : address_message->ifa_flags;
                }
            }
        }
    }
    close(socket_fd);
}
#endif
#endif

size_t utp_address_collect_local_candidates(utp_address_family_t family, uint16_t port, const char* ifname,
                                            utp_address_t* candidates, size_t capacity)
{
#if !defined(_WIN32)
    struct ifaddrs*               interfaces = NULL;
    utp_address_candidate_entry_t entries[32];
    size_t                        entry_count        = 0u;
    size_t                        candidate_count    = 0u;
    const bool                    restrict_interface = ifname != NULL && ifname[0] != '\0';

    assert(candidates != NULL || capacity == 0u);
    if (capacity == 0u || getifaddrs(&interfaces) != 0) return 0u;
    for (const struct ifaddrs* interface = interfaces; interface != NULL && entry_count < 32u;
         interface                       = interface->ifa_next) {
        utp_address_candidate_entry_t* entry;

        if (interface->ifa_addr == NULL ||
            (interface->ifa_addr->sa_family != AF_INET && interface->ifa_addr->sa_family != AF_INET6) ||
            (interface->ifa_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) ||
            (interface->ifa_flags & IFF_LOOPBACK) != 0u ||
            (restrict_interface && strcmp(interface->ifa_name, ifname) != 0)) {
            continue;
        }
        entry  = &entries[entry_count];
        *entry = (utp_address_candidate_entry_t){0};
        if (utp_address_from_sockaddr(
                &entry->address, interface->ifa_addr,
                interface->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) !=
                UTP_INTERNAL_ERROR_OK ||
            entry->address.family != family || utp_address_is_unspecified(&entry->address) ||
            utp_address_is_link_local(&entry->address) || utp_address_is_multicast(&entry->address)) {
            continue;
        }
        entry->address.port = port;
        entry->ifindex      = if_nametoindex(interface->ifa_name);
        entry->priority     = UINT8_MAX;
        ++entry_count;
    }
    freeifaddrs(interfaces);
    if (entry_count == 0u) return 0u;
#if defined(__linux__)
    utp_address_load_flags(entries, entry_count);
#endif
    for (size_t index = 0u; index < entry_count; ++index) {
        entries[index].priority = utp_address_candidate_priority(&entries[index]);
#if defined(__linux__)
        if ((entries[index].address_flags & (IFA_F_DEPRECATED | IFA_F_TENTATIVE | IFA_F_DADFAILED)) != 0u) {
            entries[index].priority = UINT8_MAX;
        }
#endif
    }
    for (uint8_t pass = 0u; pass < 2u && candidate_count < capacity; ++pass) {
        for (;;) {
            size_t best = entry_count;

            for (size_t index = 0u; index < entry_count; ++index) {
                bool interface_selected = false;

                if (entries[index].selected || entries[index].priority == UINT8_MAX) continue;
                if (pass == 0u) {
                    for (size_t previous = 0u; previous < entry_count; ++previous) {
                        if (entries[previous].selected && entries[previous].ifindex == entries[index].ifindex) {
                            interface_selected = true;
                            break;
                        }
                    }
                    if (interface_selected) continue;
                }
                if (best == entry_count || entries[index].priority < entries[best].priority) best = index;
            }
            if (best == entry_count) break;
            entries[best].selected         = true;
            entries[best].address.scope_id = entries[best].ifindex;
            candidates[candidate_count]    = entries[best].address;
            ++candidate_count;
            if (candidate_count == capacity) break;
        }
    }
    return candidate_count;
#else
    (void)family;
    (void)port;
    (void)ifname;
    (void)candidates;
    (void)capacity;
    return 0u;
#endif
}

utp_internal_error_t utp_address_parse(utp_address_t* address, const char* text, uint16_t port)
{
    if (address == NULL || text == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_address_t parsed;

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

utp_internal_error_t utp_address_from_sockaddr(utp_address_t* address, const struct sockaddr* socket_address,
                                               size_t socket_address_length)
{
    if (address == NULL || socket_address == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_address_t parsed;

    memset(&parsed, 0, sizeof(parsed));
    if (socket_address->sa_family == AF_INET && socket_address_length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* ipv4 = (const struct sockaddr_in*)socket_address;

        parsed.family = UTP_ADDRESS_FAMILY_IPV4;
        parsed.port   = ntohs(ipv4->sin_port);
        memcpy(parsed.address, &ipv4->sin_addr, 4u);
    } else if (socket_address->sa_family == AF_INET6 && socket_address_length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6* ipv6 = (const struct sockaddr_in6*)socket_address;

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

utp_internal_error_t utp_address_to_sockaddr(const utp_address_t* address, struct sockaddr_storage* storage,
                                             size_t* storage_length)
{
    if (address == NULL || storage == NULL || storage_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(storage, 0, sizeof(*storage));
    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)storage;

        ipv4->sin_family = AF_INET;
        ipv4->sin_port   = htons(address->port);
        memcpy(&ipv4->sin_addr, address->address, 4u);
        *storage_length = sizeof(*ipv4);
    } else if (address->family == UTP_ADDRESS_FAMILY_IPV6) {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)storage;

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

bool utp_address_equal(const utp_address_t* left, const utp_address_t* right)
{
    if (left == NULL || right == NULL || left->family != right->family || left->port != right->port ||
        left->scope_id != right->scope_id) {
        return false;
    }
    size_t length;

    if (left->family == UTP_ADDRESS_FAMILY_IPV4) {
        length = 4u;
    } else if (left->family == UTP_ADDRESS_FAMILY_IPV6) {
        length = 16u;
    } else {
        return false;
    }
    return memcmp(left->address, right->address, length) == 0;
}

bool utp_address_is_unspecified_ipv6(const utp_address_t* address)
{
    return address != NULL && address->family == UTP_ADDRESS_FAMILY_IPV6 &&
           memcmp(address->address, k_unspecified_ipv6_address, sizeof(k_unspecified_ipv6_address)) == 0;
}

utp_internal_error_t utp_address_format(const utp_address_t* address, char* text, size_t capacity)
{
    int32_t family;

    if (address == NULL || text == NULL || capacity == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        family = AF_INET;
    } else if (address->family == UTP_ADDRESS_FAMILY_IPV6) {
        family = AF_INET6;
    } else {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return inet_ntop((int)family, address->address, text, (socklen_t)capacity) == NULL ? UTP_INTERNAL_ERROR_OVERFLOW
                                                                                       : UTP_INTERNAL_ERROR_OK;
}
