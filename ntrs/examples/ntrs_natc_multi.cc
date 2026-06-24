#include <errno.h>
#include <ntrs_client.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#if defined(__has_include)
#if __has_include(<net/if.h>)
#include <net/if.h>
#endif
#endif
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <event2/event.h>
#include <event2/util.h>
#include <utils/CLI11.hpp>
#if defined(__linux__)
#include <sys/ioctl.h>
#endif

namespace {

static const int kControlConnectTimeoutMs = 2000;

struct DetectArgs {
    std::string node_host;
    uint16_t    node_port = 0;
    std::string peer_id = "probe_peer";
    std::string bind_ip;
    std::string bind_device;
    std::string probe1;
    std::string probe2;
    bool        verbose = false;
    int         address_family = AF_INET;
};

enum class RouteBehavior {
    SINGLE_ROUTE = 0,
    MULTI_HOMED_ROUTE,
    ROUTE_UNKNOWN,
};

struct ProbeEndpoints {
    std::string probe1;
    std::string probe2;
    std::string probe1_host;
    std::string probe2_host;
    uint16_t    probe1_port = 0;
    uint16_t    probe2_port = 0;
};

struct RouteObservation {
    std::string target_host;
    std::string target_ip;
    uint16_t    target_port = 0;
    std::string local_ip;
    uint16_t    local_port = 0;
    std::string iface;
    int         family = AF_UNSPEC;
};

struct RouteGroup {
    std::string                   key;
    std::string                   local_ip;
    std::string                   iface;
    int                           family = AF_UNSPEC;
    std::vector<RouteObservation> observations;
};

static const char* RouteBehaviorTitle(RouteBehavior behavior)
{
    switch (behavior) {
    case RouteBehavior::SINGLE_ROUTE:
        return "SINGLE_ROUTE";
    case RouteBehavior::MULTI_HOMED_ROUTE:
        return "MULTI_HOMED_ROUTE";
    default:
        return "ROUTE_UNKNOWN";
    }
}

static const char* NatTypeTitle(ntrs_nat_class_t nat_class)
{
    switch (nat_class) {
    case NTRS_NAT_CLASS_OPEN_PUBLIC:
        return "Open Public";
    case NTRS_NAT_CLASS_FULL_CONE:
        return "Full Cone NAT";
    case NTRS_NAT_CLASS_IP_RESTRICTED:
        return "IP Restricted NAT";
    case NTRS_NAT_CLASS_PORT_RESTRICTED:
        return "Port Restricted NAT";
    case NTRS_NAT_CLASS_SYMMETRIC:
        return "Symmetric NAT";
    case NTRS_NAT_CLASS_IPV6_OPEN_PUBLIC:
        return "IPv6 Open Public";
    case NTRS_NAT_CLASS_IPV6_OPEN_PUBLIC_WITH_FIREWALL:
        return "IPv6 Open Public With Firewall";
    case NTRS_NAT_CLASS_IPV6_UDP_BLOCKED:
        return "IPv6 UDP Blocked";
    default:
        return "Unknown";
    }
}

static const char* NatTypeHint(const ntrs_nat_info_t* nat)
{
    if (nat == NULL) {
        return "";
    }

    switch (nat->nat_class) {
    case NTRS_NAT_CLASS_OPEN_PUBLIC:
        if ((nat->nat_flags & NTRS_NAT_FLAG_UDP_BLOCKED) != 0) {
            return "The host has a public address, but UDP probes indicate transport blocking.";
        }
        return "The host appears to be directly reachable on a public address.";
    case NTRS_NAT_CLASS_FULL_CONE:
        return "Mapping is stable and filtering is permissive; UDP hole punching should be easier.";
    case NTRS_NAT_CLASS_IP_RESTRICTED:
        return "Mapping is stable, but return traffic usually requires a prior packet to the peer IP.";
    case NTRS_NAT_CLASS_PORT_RESTRICTED:
        return "Mapping is stable, but return traffic usually requires a prior packet to the peer IP and port.";
    case NTRS_NAT_CLASS_SYMMETRIC:
        return "Different destinations produce different public mappings; UDP hole punching is difficult.";
    case NTRS_NAT_CLASS_IPV6_OPEN_PUBLIC:
        return "IPv6 UDP is reachable and ordinary probe replies are received.";
    case NTRS_NAT_CLASS_IPV6_OPEN_PUBLIC_WITH_FIREWALL:
        return "IPv6 UDP is reachable, but changed-source replies are filtered.";
    case NTRS_NAT_CLASS_IPV6_UDP_BLOCKED:
        return "IPv6 UDP probes failed; verify that IPv6 UDP is allowed on this network.";
    default:
        if ((nat->nat_flags & NTRS_NAT_FLAG_UDP_BLOCKED) != 0) {
            return "Basic UDP probes failed; verify that UDP is allowed on this network.";
        }
        return "The current sample is insufficient for a stronger conclusion.";
    }
}

static const char* MappingBehaviorTitle(ntrs_mapping_behavior_t behavior)
{
    switch (behavior) {
    case NTRS_MAPPING_ENDPOINT_INDEPENDENT:
        return "Endpoint Independent";
    case NTRS_MAPPING_ADDRESS_DEPENDENT:
        return "Changes Across Destinations";
    case NTRS_MAPPING_ADDRESS_AND_PORT_DEPENDENT:
        return "Changes Across Destination Ports";
    case NTRS_MAPPING_UNSTABLE:
        return "Unstable";
    default:
        return "Unknown";
    }
}

static const char* FilteringBehaviorTitle(ntrs_filtering_behavior_t behavior)
{
    switch (behavior) {
    case NTRS_FILTERING_ENDPOINT_INDEPENDENT:
        return "Endpoint Independent";
    case NTRS_FILTERING_ADDRESS_DEPENDENT:
        return "Address Dependent";
    case NTRS_FILTERING_ADDRESS_AND_PORT_DEPENDENT:
        return "Address and Port Dependent";
    case NTRS_FILTERING_BLOCKED:
        return "Blocked";
    default:
        return "Unknown";
    }
}

static void AppendFlagText(std::vector<std::string>* flags, bool enabled, const char* text)
{
    if (flags != NULL && enabled && text != NULL && text[0] != '\0') {
        flags->push_back(text);
    }
}

static std::string JoinFlags(const std::vector<std::string>& flags)
{
    std::string out;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += flags[i];
    }
    return out;
}

static std::string FormatFlags(ntrs_nat_flags_t nat_flags)
{
    std::vector<std::string> flags;

    AppendFlagText(&flags, (nat_flags & NTRS_NAT_FLAG_UDP_BLOCKED) != 0, "udp_blocked");
    AppendFlagText(&flags, (nat_flags & NTRS_NAT_FLAG_PROBE_DEGRADED) != 0, "degraded_probe_coverage");
    AppendFlagText(&flags, (nat_flags & NTRS_NAT_FLAG_MAPPING_UNSTABLE) != 0,
                     "observed_mapping_instability");
    AppendFlagText(&flags, (nat_flags & NTRS_NAT_FLAG_MULTI_EXTERNAL_IP) != 0,
                     "multiple_external_ips_observed");
    AppendFlagText(&flags, (nat_flags & NTRS_NAT_FLAG_LOCAL_ADDR_PUBLIC) != 0, "local_public_address");

    if (flags.empty()) {
        return "none";
    }
    return JoinFlags(flags);
}

static void PrintResult(const ntrs_nat_info_t* nat, const std::string& probe1, const std::string& probe2)
{
    printf("Detection Result: %s\n", NatTypeTitle(nat->nat_class));
    printf("Summary: %s\n", NatTypeHint(nat));
    printf("Risk: %s\n", nat->nat_risk);
    printf("Signals: %s\n", FormatFlags(nat->nat_flags).c_str());
    printf("Mapping Behavior: %s\n", MappingBehaviorTitle(nat->mapping_behavior));
    printf("Filtering Behavior: %s\n", FilteringBehaviorTitle(nat->filtering_behavior));
    printf("Probe Endpoints: probe1=%s probe2=%s\n", probe1.c_str(), probe2.empty() ? "-" : probe2.c_str());
    printf("Local Address: %s:%u\n", nat->local_ip, nat->local_port);
    printf("Public Mapping #1: %s:%u\n", nat->srflx_ip, nat->srflx_port);
    printf("Public Mapping #2: %s:%u\n", nat->srflx_ip_2, nat->srflx_port_2);
    printf("Samples: rounds=%d p1=%d p2=%d p1_map=%d p2_map=%d rtt1=%dms rtt2=%dms\n", nat->probe_rounds,
           nat->probe1_success_count, nat->probe2_success_count, nat->probe1_distinct_mappings,
           nat->probe2_distinct_mappings, nat->probe1_rtt_ms, nat->probe2_rtt_ms);
}

static void PrintRouteGroups(const std::vector<RouteGroup>& groups)
{
    if (groups.empty()) {
        return;
    }

    printf("Route Groups:\n");
    for (size_t i = 0; i < groups.size(); ++i) {
        const RouteGroup& group = groups[i];
        printf("  group[%zu]: local=%s iface=%s family=%s samples=%zu\n", i, group.local_ip.c_str(),
               group.iface.empty() ? "unknown" : group.iface.c_str(), group.family == AF_INET ? "ipv4" : "unknown",
               group.observations.size());
    }
}

static void VerboseLog(bool verbose, const char* message)
{
    if (verbose && message != NULL) {
        printf("%s\n", message);
    }
}

struct AsyncResultWait {
    event_base*         base;
    bool                done;
    ntrs_async_result_t result;
};

struct ProbeSession {
    bool            success;
    char            error_message[NTRS_MAX_TEXT_LEN];
    char            session_token[NTRS_MAX_TEXT_LEN];
    uint32_t        lease_default_sec;
    char            probe1[NTRS_MAX_TEXT_LEN];
    char            probe2[NTRS_MAX_TEXT_LEN];
    ntrs_nat_info_t nat_info;
    int             control_fd;
    int             udp_sock;
};

struct ValidatedGroup {
    RouteGroup   route_group;
    ProbeSession probe;
};

static void CloseProbeSession(ProbeSession* probe)
{
    if (probe == NULL) {
        return;
    }
    if (probe->udp_sock >= 0) {
        close(probe->udp_sock);
        probe->udp_sock = -1;
    }
    if (probe->control_fd >= 0) {
        close(probe->control_fd);
        probe->control_fd = -1;
    }
}

static void AsyncResultCallback(const ntrs_async_result_t* result, void* user_data)
{
    AsyncResultWait* wait = static_cast<AsyncResultWait*>(user_data);
    if (wait == NULL || result == NULL) {
        return;
    }

    wait->result = *result;
    wait->done = true;
    event_base_loopbreak(wait->base);
}

static bool WaitAsyncResult(event_base* base, AsyncResultWait* wait, int timeout_sec)
{
    time_t deadline = time(NULL) + timeout_sec;
    while (wait != NULL && !wait->done && time(NULL) < deadline) {
        if (event_base_loop(base, EVLOOP_ONCE) != 0) {
            break;
        }
    }
    return wait != NULL && wait->done;
}

static bool SetNonblockingFd(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool SetBlockingFd(int fd, bool blocking)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0;
}

static bool WaitWritableFd(int fd, int timeout_ms)
{
    fd_set wfds;
    struct timeval tv;

    if (fd < 0 || timeout_ms < 0) {
        return false;
    }

    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    for (;;) {
        int ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (ret > 0) {
            return true;
        }
        if (ret == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
    }
}

static bool SetSocketTimeoutsMs(int fd, int timeout_ms)
{
    struct timeval tv;

    if (fd < 0 || timeout_ms < 0) {
        return false;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

static bool IsUsableInterfaceIpv6(const struct in6_addr& addr)
{
    return !IN6_IS_ADDR_UNSPECIFIED(&addr) && !IN6_IS_ADDR_LOOPBACK(&addr) && !IN6_IS_ADDR_LINKLOCAL(&addr);
}

static bool ParseEndpointText(const std::string& endpoint, std::string* host, uint16_t* port)
{
    std::string host_part;
    std::string port_part;
    int         parsed = 0;

    if (host == NULL || port == NULL || endpoint.empty()) {
        return false;
    }
    if (endpoint[0] == '[') {
        size_t close = endpoint.find(']');
        if (close == std::string::npos || close + 1 >= endpoint.size() || endpoint[close + 1] != ':') {
            return false;
        }
        host_part = endpoint.substr(1, close - 1);
        port_part = endpoint.substr(close + 2);
    } else {
        size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos) {
            return false;
        }
        host_part = endpoint.substr(0, colon);
        port_part = endpoint.substr(colon + 1);
        if (host_part.find(':') != std::string::npos) {
            return false;
        }
    }
    if (host_part.empty() || port_part.empty()) {
        return false;
    }
    parsed = atoi(port_part.c_str());
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }
    *host = host_part;
    *port = (uint16_t)parsed;
    return true;
}

static bool ResolveInterfaceIpv4(const std::string& bind_device, std::string* resolved_ip)
{
    struct ifaddrs* addrs = NULL;

    if (resolved_ip == NULL || bind_device.empty()) {
        return false;
    }

    resolved_ip->clear();
    if (getifaddrs(&addrs) != 0) {
        return false;
    }

    for (struct ifaddrs* it = addrs; it != NULL; it = it->ifa_next) {
        char current_ip[INET_ADDRSTRLEN] = {0};
        const struct sockaddr_in* addr = NULL;

        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET || it->ifa_name == NULL) {
            continue;
        }
        if (bind_device != it->ifa_name) {
            continue;
        }
        addr = reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, current_ip, sizeof(current_ip)) == NULL) {
            continue;
        }
        *resolved_ip = current_ip;
        freeifaddrs(addrs);
        return true;
    }

    freeifaddrs(addrs);
    return false;
}

static bool ApplyBindDeviceIfSupported(int sock, const std::string& bind_device)
{
    if (sock < 0 || bind_device.empty()) {
        return true;
    }

#if defined(__linux__) && defined(SO_BINDTODEVICE)
    return setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, bind_device.c_str(), bind_device.size() + 1) == 0;
#else
    (void)sock;
    (void)bind_device;
    return true;
#endif
}

static std::string RouteGroupKey(const RouteObservation& observation)
{
    return std::string(observation.family == AF_INET ? "ipv4" : "unknown") + "|" + observation.local_ip + "|" +
           (observation.iface.empty() ? "unknown" : observation.iface);
}

static bool EnumerateIpv4RouteGroups(std::vector<RouteGroup>* groups)
{
    struct ifaddrs* addrs = NULL;

    if (groups == NULL) {
        return false;
    }

    groups->clear();
    if (getifaddrs(&addrs) != 0) {
        return false;
    }

    for (struct ifaddrs* it = addrs; it != NULL; it = it->ifa_next) {
        char current_ip[INET_ADDRSTRLEN] = {0};
        const struct sockaddr_in* addr = NULL;
        RouteObservation observation;
        std::string key;
        bool inserted = false;

        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        addr = reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, current_ip, sizeof(current_ip)) == NULL) {
            continue;
        }

        observation.local_ip = current_ip;
        observation.iface = it->ifa_name == NULL ? "" : it->ifa_name;
        observation.family = AF_INET;
        key = RouteGroupKey(observation);

        for (size_t group_index = 0; group_index < groups->size(); ++group_index) {
            if ((*groups)[group_index].key == key) {
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            RouteGroup group;
            group.key = key;
            group.local_ip = observation.local_ip;
            group.iface = observation.iface;
            group.family = AF_INET;
            groups->push_back(group);
        }
    }

    freeifaddrs(addrs);
    return !groups->empty();
}


static bool ResolveBindIpv4(const std::string& bind_ip, const std::string& bind_device, std::string* resolved_ip)
{
    if (resolved_ip == NULL) {
        return false;
    }
    if (!bind_ip.empty()) {
        struct in_addr addr;
        if (inet_pton(AF_INET, bind_ip.c_str(), &addr) != 1) {
            return false;
        }
        *resolved_ip = bind_ip;
        return true;
    }

    if (!bind_device.empty()) {
        return ResolveInterfaceIpv4(bind_device, resolved_ip);
    }

    resolved_ip->clear();
    return true;
}

static bool ResolveBindIp(const std::string& bind_ip, const std::string& bind_device, int family,
                          std::string* resolved_ip)
{
    if (family == AF_INET) {
        return ResolveBindIpv4(bind_ip, bind_device, resolved_ip);
    }
    if (resolved_ip == NULL || family != AF_INET6) {
        return false;
    }
    if (!bind_ip.empty()) {
        struct in6_addr addr;
        if (inet_pton(AF_INET6, bind_ip.c_str(), &addr) != 1) {
            return false;
        }
        *resolved_ip = bind_ip;
        return true;
    }
    if (!bind_device.empty()) {
        struct ifaddrs* addrs = NULL;
        if (getifaddrs(&addrs) != 0) {
            return false;
        }
        for (struct ifaddrs* it = addrs; it != NULL; it = it->ifa_next) {
            char current_ip[INET6_ADDRSTRLEN] = {0};
            const struct sockaddr_in6* addr6 = NULL;
            if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET6 || it->ifa_name == NULL ||
                bind_device != it->ifa_name) {
                continue;
            }
            addr6 = reinterpret_cast<const struct sockaddr_in6*>(it->ifa_addr);
            if (!IsUsableInterfaceIpv6(addr6->sin6_addr) ||
                inet_ntop(AF_INET6, &addr6->sin6_addr, current_ip, sizeof(current_ip)) == NULL) {
                continue;
            }
            *resolved_ip = current_ip;
            freeifaddrs(addrs);
            return true;
        }
        freeifaddrs(addrs);
        return false;
    }
    resolved_ip->clear();
    return true;
}

static bool BindAnyOrIp(int fd, int family, const std::string& bind_ip)
{
    if (family == AF_INET) {
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(0);
        if (!bind_ip.empty()) {
            if (inet_pton(AF_INET, bind_ip.c_str(), &local_addr.sin_addr) != 1) {
                return false;
            }
        } else {
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        return bind(fd, reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr)) == 0;
    }
    if (family == AF_INET6) {
        struct sockaddr_in6 local_addr;
        int                 v6only = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin6_family = AF_INET6;
        local_addr.sin6_port = htons(0);
        if (!bind_ip.empty()) {
            if (inet_pton(AF_INET6, bind_ip.c_str(), &local_addr.sin6_addr) != 1) {
                return false;
            }
        } else {
            local_addr.sin6_addr = in6addr_any;
        }
        return bind(fd, reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr)) == 0;
    }
    return false;
}

static int CreateUdpProbeSocket(const std::string& bind_ip, const std::string& bind_device, int family)
{
    int         sock = -1;
    std::string resolved_ip;

    if (!ResolveBindIp(bind_ip, bind_device, family, &resolved_ip)) {
        return -1;
    }

    sock = socket(family, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    if (!ApplyBindDeviceIfSupported(sock, bind_device)) {
        close(sock);
        return -1;
    }
    if (!SetNonblockingFd(sock)) {
        close(sock);
        return -1;
    }

    if (!BindAnyOrIp(sock, family, resolved_ip)) {
        close(sock);
        return -1;
    }

    return sock;
}

static int ConnectControlWithBind(const DetectArgs& args, const char* phase, std::string* resolved_node_ip,
                                     std::string* error_message)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    char port_text[16];
    std::string bind_ip;
    int fd = -1;

    if (resolved_node_ip == NULL || error_message == NULL) {
        return -1;
    }

    resolved_node_ip->clear();
    error_message->clear();

    if (!ResolveBindIp(args.bind_ip, args.bind_device, args.address_family, &bind_ip)) {
        *error_message = "resolve bind IP failed";
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = args.address_family;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)args.node_port);
    if (getaddrinfo(args.node_host.c_str(), port_text, &hints, &result) != 0 || result == NULL) {
        *error_message = "resolve control host failed";
        return -1;
    }

    for (struct addrinfo* it = result; it != NULL; it = it->ai_next) {
        char node_ip[NI_MAXHOST] = {0};

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (!ApplyBindDeviceIfSupported(fd, args.bind_device)) {
            close(fd);
            fd = -1;
            continue;
        }

        if (!BindAnyOrIp(fd, it->ai_family, bind_ip)) {
            close(fd);
            fd = -1;
            continue;
        }

        if (getnameinfo(it->ai_addr, it->ai_addrlen, node_ip, sizeof(node_ip), NULL, 0, NI_NUMERICHOST) == 0) {
            *resolved_node_ip = node_ip;
        }
        if (args.verbose) {
            printf("%s: connect control %s:%u resolved=%s bind_ip=%s\n", phase, args.node_host.c_str(),
                   (unsigned)args.node_port, resolved_node_ip->empty() ? "-" : resolved_node_ip->c_str(),
                   bind_ip.empty() ? "-" : bind_ip.c_str());
        }

        if (!SetNonblockingFd(fd)) {
            close(fd);
            fd = -1;
            continue;
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) {
                if (args.verbose) {
                    printf("%s: connect immediate failure bind_ip=%s errno=%d\n", phase,
                           bind_ip.empty() ? "-" : bind_ip.c_str(), errno);
                }
                close(fd);
                fd = -1;
                continue;
            }
            if (!WaitWritableFd(fd, kControlConnectTimeoutMs)) {
                if (args.verbose) {
                    printf("%s: connect timeout bind_ip=%s timeout_ms=%d\n", phase,
                           bind_ip.empty() ? "-" : bind_ip.c_str(), kControlConnectTimeoutMs);
                }
                close(fd);
                fd = -1;
                continue;
            }

            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
                if (args.verbose) {
                    printf("%s: connect completion failure bind_ip=%s so_error=%d\n", phase,
                           bind_ip.empty() ? "-" : bind_ip.c_str(), so_error);
                }
                close(fd);
                fd = -1;
                continue;
            }
        }

        SetBlockingFd(fd, true);
        SetSocketTimeoutsMs(fd, kControlConnectTimeoutMs);
        freeaddrinfo(result);
        return fd;
    }

    freeaddrinfo(result);
    *error_message = "connect control failed";
    return -1;
}

static bool RunProbeSession(const DetectArgs& args, ProbeSession* out)
{
    event_base*                base = NULL;
    ntrs_async_client_t*       async_client = NULL;
    int                       control_fd = -1;
    int                       udp_sock = -1;
    char                      session_token[NTRS_MAX_TEXT_LEN];
    uint32_t                  lease_default_sec = 0;
    char                      probe1[NTRS_MAX_TEXT_LEN];
    char                      probe2[NTRS_MAX_TEXT_LEN];
    std::string               probe1_host;
    std::string               probe2_host;
    std::string               connect_error;
    std::string               resolved_node_ip;
    uint16_t                  probe1_port = 0;
    uint16_t                  probe2_port = 0;
    ntrs_detect_nat_options_t detect_options;
    AsyncResultWait           wait;
    uint64_t                  request_id = 0;

    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->control_fd = -1;
    out->udp_sock = -1;
    ntrs_nat_info_init(&out->nat_info);
    memset(session_token, 0, sizeof(session_token));
    memset(probe1, 0, sizeof(probe1));
    memset(probe2, 0, sizeof(probe2));

    base = event_base_new();
    if (base == NULL) {
        snprintf(out->error_message, sizeof(out->error_message), "create event base failed");
        return false;
    }
    async_client = ntrs_async_client_create(base);
    if (async_client == NULL) {
        snprintf(out->error_message, sizeof(out->error_message), "create async client failed");
        event_base_free(base);
        return false;
    }

    control_fd = ConnectControlWithBind(args, "Probe Session", &resolved_node_ip, &connect_error);
    if (control_fd < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "%s",
                 connect_error.empty() ? "connect control failed" : connect_error.c_str());
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (args.verbose) {
        printf("Probe Session: auth peer_id=%s resolved=%s\n", args.peer_id.c_str(),
               resolved_node_ip.empty() ? "-" : resolved_node_ip.c_str());
    }
    if (!ntrs_auth(control_fd, args.peer_id.c_str(), "ntrs-dev-secret", session_token, sizeof(session_token),
                   &lease_default_sec)) {
        snprintf(out->error_message, sizeof(out->error_message), "auth failed");
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (args.verbose) {
        printf("Probe Session: auth ok lease_default_sec=%u\n", (unsigned)lease_default_sec);
    }

    if (!args.probe1.empty()) {
        snprintf(probe1, sizeof(probe1), "%s", args.probe1.c_str());
        snprintf(probe2, sizeof(probe2), "%s", args.probe2.c_str());
    } else if (!ntrs_request_probe_endpoints(control_fd, session_token, probe1, sizeof(probe1), probe2, sizeof(probe2))) {
        snprintf(out->error_message, sizeof(out->error_message), "request probe endpoints failed");
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (args.verbose) {
        printf("Probe Session: probe1=%s probe2=%s\n", probe1, probe2[0] == '\0' ? "-" : probe2);
    }

    if (!ParseEndpointText(probe1, &probe1_host, &probe1_port)) {
        snprintf(out->error_message, sizeof(out->error_message), "invalid probe1 endpoint");
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (probe2[0] != '\0' && !ParseEndpointText(probe2, &probe2_host, &probe2_port)) {
        snprintf(out->error_message, sizeof(out->error_message), "invalid probe2 endpoint");
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }

    udp_sock = CreateUdpProbeSocket(args.bind_ip, args.bind_device, args.address_family);
    if (udp_sock < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "create probe socket failed");
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (args.verbose) {
        printf("Probe Session: udp socket ready bind_ip=%s bind_device=%s\n",
               args.bind_ip.empty() ? "-" : args.bind_ip.c_str(),
               args.bind_device.empty() ? "-" : args.bind_device.c_str());
    }

    ntrs_detect_nat_options_init(&detect_options);
    detect_options.enable_filter_probe = true;
    detect_options.verbose = args.verbose;
    memset(&wait, 0, sizeof(wait));
    wait.base = base;
    if (args.verbose) {
        printf("Probe Session: async detect start\n");
    }
    if (!ntrs_async_detect_nat(async_client, &request_id, udp_sock, probe1_host.c_str(), probe1_port,
                               probe2[0] == '\0' ? NULL : probe2_host.c_str(), probe2_port, control_fd, session_token,
                               &detect_options, AsyncResultCallback, &wait)) {
        snprintf(out->error_message, sizeof(out->error_message), "submit async NAT detect failed");
        close(udp_sock);
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (!WaitAsyncResult(base, &wait, 10)) {
        ntrs_async_client_cancel(async_client, request_id);
        snprintf(out->error_message, sizeof(out->error_message), "async NAT detect timed out");
        close(udp_sock);
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }
    if (!wait.result.success) {
        snprintf(out->error_message, sizeof(out->error_message), "%s",
                 wait.result.error_message[0] != '\0' ? wait.result.error_message : "async NAT detect failed");
        close(udp_sock);
        close(control_fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return false;
    }

    out->success = true;
    out->control_fd = control_fd;
    out->udp_sock = udp_sock;
    out->lease_default_sec = lease_default_sec;
    snprintf(out->session_token, sizeof(out->session_token), "%s", session_token);
    snprintf(out->probe1, sizeof(out->probe1), "%s", probe1);
    snprintf(out->probe2, sizeof(out->probe2), "%s", probe2);
    out->nat_info = wait.result.nat_info;
    ntrs_async_client_destroy(async_client);
    event_base_free(base);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    CLI::App                app{"NTRS NAT detection (multi-egress trial) tool"};
    DetectArgs              args;
    std::vector<RouteGroup> route_groups;
    std::vector<ValidatedGroup> validated_groups;
    ProbeSession            probe;

    app.add_option("node_host", args.node_host, "Node host or IP")->required();
    app.add_option("node_port", args.node_port, "Node control port")->required();
    app.add_option("--peer-id", args.peer_id, "peer_id used for authentication");
    app.add_option("--bind-ip", args.bind_ip, "Bind the probe socket to a local IP address");
    app.add_option("--bind-device", args.bind_device, "Bind the probe socket to a network interface");
    app.add_option("--probe1", args.probe1, "Explicit probe1 endpoint in host:port or [ipv6]:port format");
    app.add_option("--probe2", args.probe2, "Explicit probe2 endpoint in host:port or [ipv6]:port format");
    app.add_flag_callback("-4,--ipv4", [&args]() { args.address_family = AF_INET; }, "Use IPv4 probing");
    app.add_flag_callback("-6,--ipv6", [&args]() { args.address_family = AF_INET6; }, "Use IPv6 probing");
    app.add_flag("-v,--verbose", args.verbose, "Print step-by-step probe progress");

    CLI11_PARSE(app, argc, argv);

    if (args.verbose) {
        VerboseLog(true, "Connecting to node...");
        VerboseLog(true, "Authenticating...");
        if (args.probe1.empty()) {
            VerboseLog(true, "Requesting probe endpoints...");
        }
        if (!args.bind_device.empty()) {
            printf("Binding probe socket to device: %s\n", args.bind_device.c_str());
        }
        if (!args.bind_ip.empty()) {
            printf("Binding probe socket to local IP: %s\n", args.bind_ip.c_str());
        }
        VerboseLog(true, "Starting NAT detection (multi-egress trial)...");
    }

    if (args.bind_ip.empty() && args.bind_device.empty() && args.address_family == AF_INET) {
        if (EnumerateIpv4RouteGroups(&route_groups)) {
            if (args.verbose) {
                printf("Candidate IPv4 Route Groups:\n");
                PrintRouteGroups(route_groups);
            }
        } else if (args.verbose) {
            printf("Candidate IPv4 Route Groups: none\n");
        }
    }

    if (args.bind_ip.empty() && args.bind_device.empty() && args.address_family == AF_INET) {
        for (size_t i = 0; i < route_groups.size(); ++i) {
            DetectArgs group_args = args;
            ValidatedGroup validated;

            group_args.bind_ip = route_groups[i].local_ip;
            group_args.bind_device = route_groups[i].iface;
            if (args.verbose) {
                printf("\n== Validate Route Group %zu ==\n", i);
                printf("bind_ip=%s bind_device=%s iface=%s\n", route_groups[i].local_ip.c_str(),
                       route_groups[i].iface.empty() ? "-" : route_groups[i].iface.c_str(),
                       route_groups[i].iface.empty() ? "unknown" : route_groups[i].iface.c_str());
            }
            if (!RunProbeSession(group_args, &validated.probe)) {
                if (args.verbose) {
                    printf("Route Group %zu rejected: %s\n", i, validated.probe.error_message);
                }
                continue;
            }
            validated.route_group = route_groups[i];
            validated_groups.push_back(validated);
        }
        if (validated_groups.empty()) {
            printf("Global Route Behavior: %s\n", RouteBehaviorTitle(RouteBehavior::ROUTE_UNKNOWN));
            printf("Global NAT Classification: UNKNOWN\n");
            printf("Global Reason: no bound local IPv4 candidate produced a valid probe response\n");
            return 1;
        }
        if (validated_groups.size() > 1) {
            printf("Global Route Behavior: %s\n", RouteBehaviorTitle(RouteBehavior::MULTI_HOMED_ROUTE));
            printf("Global NAT Classification: UNKNOWN\n");
            printf("Global Reason: multiple route groups produced valid probe responses; evaluate each bound result separately\n");
            for (size_t i = 0; i < validated_groups.size(); ++i) {
                printf("\n== Route Group %zu ==\n", i);
                printf("bind_ip=%s iface=%s\n", validated_groups[i].route_group.local_ip.c_str(),
                       validated_groups[i].route_group.iface.empty() ? "unknown" : validated_groups[i].route_group.iface.c_str());
                PrintResult(&validated_groups[i].probe.nat_info, validated_groups[i].probe.probe1,
                             validated_groups[i].probe.probe2);
                CloseProbeSession(&validated_groups[i].probe);
            }
            return 0;
        }

        printf("Route Behavior: %s\n", RouteBehaviorTitle(RouteBehavior::SINGLE_ROUTE));
        printf("Classification Reason: only one bound local IPv4 candidate produced valid probe responses\n");
        PrintResult(&validated_groups[0].probe.nat_info, validated_groups[0].probe.probe1,
                     validated_groups[0].probe.probe2);
        CloseProbeSession(&validated_groups[0].probe);
        return 0;
    }

    if (!RunProbeSession(args, &probe)) {
        printf("NAT detection (multi-egress trial) failed: %s\n", probe.error_message);
        return 1;
    }

    PrintResult(&probe.nat_info, probe.probe1, probe.probe2);

    CloseProbeSession(&probe);
    return 0;
}
