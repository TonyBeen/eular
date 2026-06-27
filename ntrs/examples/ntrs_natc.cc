#include <errno.h>
#include <ntrs/ntrs.h>
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

static const char* NatTypeTitle(ntrs_nat_class_t nat_class)
{
    switch (nat_class) {
    case NTRS_NAT_CLASS_OPEN_PUBLIC:
        return "Open Public";
    case NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL:
        return "Open Public With Firewall";
    case NTRS_NAT_CLASS_FULL_CONE:
        return "Full Cone NAT";
    case NTRS_NAT_CLASS_IP_RESTRICTED:
        return "IP Restricted NAT";
    case NTRS_NAT_CLASS_PORT_RESTRICTED:
        return "Port Restricted NAT";
    case NTRS_NAT_CLASS_SYMMETRIC:
        return "Symmetric NAT";
    case NTRS_NAT_CLASS_SYMMETRIC_MULTI_LINE:
        return "Symmetric Multi-Line NAT";
    case NTRS_NAT_CLASS_UDP_BLOCKED:
        return "UDP Blocked";
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
        return "The host appears to be directly reachable on a public address.";
    case NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL:
        return "The host has a public address, but changed-source UDP replies are filtered.";
    case NTRS_NAT_CLASS_FULL_CONE:
        return "Mapping is stable and filtering is permissive; UDP hole punching should be easier.";
    case NTRS_NAT_CLASS_IP_RESTRICTED:
        return "Mapping is stable, but return traffic usually requires a prior packet to the peer IP.";
    case NTRS_NAT_CLASS_PORT_RESTRICTED:
        return "Mapping is stable, but return traffic usually requires a prior packet to the peer IP and port.";
    case NTRS_NAT_CLASS_SYMMETRIC:
        return "Different destinations produce different public mappings; UDP hole punching is difficult.";
    case NTRS_NAT_CLASS_SYMMETRIC_MULTI_LINE:
        return "Multiple external mappings were observed; UDP hole punching is difficult.";
    case NTRS_NAT_CLASS_UDP_BLOCKED:
        return "Basic UDP probes failed; verify that UDP is allowed on this network.";
    default:
        return "The current sample is insufficient for a stronger conclusion.";
    }
}

static std::string FormatEndpointText(const char* ip, uint16_t port)
{
    const std::string host = ip == NULL ? "" : ip;

    if (host.find(':') != std::string::npos && (host.empty() || host[0] != '[')) {
        return "[" + host + "]:" + std::to_string((unsigned)port);
    }
    return host + ":" + std::to_string((unsigned)port);
}

static void PrintResult(const ntrs_nat_info_t* nat, const std::string& probe1, const std::string& probe2)
{
    printf("Detection Result: %s\n", NatTypeTitle(nat->nat_class));
    printf("Summary: %s\n", NatTypeHint(nat));
    printf("Probe Endpoints: probe1=%s probe2=%s\n", probe1.c_str(), probe2.empty() ? "-" : probe2.c_str());
    printf("Local Address: %s\n", FormatEndpointText(nat->local_ip, nat->local_port).c_str());
    printf("Public Mapping #1: %s\n", FormatEndpointText(nat->srflx_ip, nat->srflx_port).c_str());
    printf("Public Mapping #2: %s\n", FormatEndpointText(nat->srflx_ip_2, nat->srflx_port_2).c_str());
    printf("Samples: rounds=%d p1=%d p2=%d p1_map=%d p2_map=%d rtt1=%dms rtt2=%dms\n", nat->probe_rounds,
           nat->probe1_success_count, nat->probe2_success_count, nat->probe1_distinct_mappings,
           nat->probe2_distinct_mappings, nat->probe1_rtt_ms, nat->probe2_rtt_ms);
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

static bool IsUsableInterfaceIpv6(const struct in6_addr& addr)
{
    return !IN6_IS_ADDR_UNSPECIFIED(&addr) && !IN6_IS_ADDR_LOOPBACK(&addr) && !IN6_IS_ADDR_LINKLOCAL(&addr);
}

static bool ResolveInterfaceIp(const std::string& bind_device, int family, std::string* resolved_ip)
{
    struct ifaddrs* addrs = NULL;

    if (resolved_ip == NULL || bind_device.empty() || (family != AF_INET && family != AF_INET6)) {
        return false;
    }

    resolved_ip->clear();
    if (getifaddrs(&addrs) != 0) {
        return false;
    }

    for (struct ifaddrs* it = addrs; it != NULL; it = it->ifa_next) {
        char current_ip[INET6_ADDRSTRLEN] = {0};

        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != family || it->ifa_name == NULL) {
            continue;
        }
        if (bind_device != it->ifa_name) {
            continue;
        }
        if (family == AF_INET &&
            inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr)->sin_addr, current_ip,
                      sizeof(current_ip)) == NULL) {
            continue;
        }
        if (family == AF_INET6) {
            const struct sockaddr_in6* addr6 = reinterpret_cast<const struct sockaddr_in6*>(it->ifa_addr);
            if (!IsUsableInterfaceIpv6(addr6->sin6_addr) ||
                inet_ntop(AF_INET6, &addr6->sin6_addr, current_ip, sizeof(current_ip)) == NULL) {
                continue;
            }
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

static bool ResolveBindIp(const std::string& bind_ip, const std::string& bind_device, int family,
                          std::string* resolved_ip)
{
    if (resolved_ip == NULL) {
        return false;
    }
    if (!bind_ip.empty()) {
        uint8_t addr[sizeof(struct in6_addr)];
        if ((family != AF_INET && family != AF_INET6) || inet_pton(family, bind_ip.c_str(), addr) != 1) {
            return false;
        }
        *resolved_ip = bind_ip;
        return true;
    }

    if (!bind_device.empty()) {
        return ResolveInterfaceIp(bind_device, family, resolved_ip);
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

static int ConnectControlWithBind(const DetectArgs& args, std::string* error_message)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    char port_text[16];
    std::string bind_ip;
    int fd = -1;

    if (error_message == NULL) {
        return -1;
    }
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

        if (!SetNonblockingFd(fd)) {
            close(fd);
            fd = -1;
            continue;
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) != 0) {
            if (errno != EINPROGRESS) {
                close(fd);
                fd = -1;
                continue;
            }
            if (!WaitWritableFd(fd, kControlConnectTimeoutMs)) {
                close(fd);
                fd = -1;
                continue;
            }

            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
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

static bool RunProbeSession(event_base* base, ntrs_async_client_t* async_client, const DetectArgs& args,
                              ProbeSession* out)
{
    int                       control_fd = -1;
    int                       udp_sock = -1;
    char                      session_token[NTRS_MAX_TEXT_LEN];
    uint32_t                  lease_default_sec = 0;
    char                      probe1[NTRS_MAX_TEXT_LEN];
    char                      probe2[NTRS_MAX_TEXT_LEN];
    std::string               connect_error;
    std::string               probe1_host;
    std::string               probe2_host;
    uint16_t                  probe1_port = 0;
    uint16_t                  probe2_port = 0;
    ntrs_detect_nat_options_t detect_options;
    AsyncResultWait           wait;
    uint64_t                  request_id = 0;

    if (base == NULL || async_client == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->control_fd = -1;
    out->udp_sock = -1;
    ntrs_nat_info_init(&out->nat_info);
    memset(session_token, 0, sizeof(session_token));
    memset(probe1, 0, sizeof(probe1));
    memset(probe2, 0, sizeof(probe2));

    control_fd = ConnectControlWithBind(args, &connect_error);
    if (control_fd < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "%s",
                 connect_error.empty() ? "connect control failed" : connect_error.c_str());
        return false;
    }
    if (!ntrs_auth(control_fd, args.peer_id.c_str(), "ntrs-dev-secret", session_token, sizeof(session_token),
                   &lease_default_sec)) {
        snprintf(out->error_message, sizeof(out->error_message), "auth failed");
        close(control_fd);
        return false;
    }

    if (!args.probe1.empty()) {
        snprintf(probe1, sizeof(probe1), "%s", args.probe1.c_str());
        snprintf(probe2, sizeof(probe2), "%s", args.probe2.c_str());
    } else if (!ntrs_request_probe_endpoints(control_fd, session_token, probe1, sizeof(probe1), probe2, sizeof(probe2))) {
        snprintf(out->error_message, sizeof(out->error_message), "request probe endpoints failed");
        close(control_fd);
        return false;
    }

    if (!ParseEndpointText(probe1, &probe1_host, &probe1_port)) {
        snprintf(out->error_message, sizeof(out->error_message), "invalid probe1 endpoint");
        close(control_fd);
        return false;
    }
    if (probe2[0] != '\0' && !ParseEndpointText(probe2, &probe2_host, &probe2_port)) {
        snprintf(out->error_message, sizeof(out->error_message), "invalid probe2 endpoint");
        close(control_fd);
        return false;
    }

    udp_sock = CreateUdpProbeSocket(args.bind_ip, args.bind_device, args.address_family);
    if (udp_sock < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "create probe socket failed");
        close(control_fd);
        return false;
    }

    ntrs_detect_nat_options_init(&detect_options);
    detect_options.enable_filter_probe = true;
    detect_options.verbose = args.verbose;
    memset(&wait, 0, sizeof(wait));
    wait.base = base;
    if (!ntrs_async_detect_nat(async_client, &request_id, udp_sock, probe1_host.c_str(), probe1_port,
                               probe2[0] == '\0' ? NULL : probe2_host.c_str(), probe2_port, control_fd, session_token,
                               &detect_options, AsyncResultCallback, &wait)) {
        snprintf(out->error_message, sizeof(out->error_message), "submit async NAT detect failed");
        close(udp_sock);
        close(control_fd);
        return false;
    }
    if (!WaitAsyncResult(base, &wait, 10)) {
        ntrs_async_client_cancel(async_client, request_id);
        snprintf(out->error_message, sizeof(out->error_message), "async NAT detect timed out");
        close(udp_sock);
        close(control_fd);
        return false;
    }
    if (!wait.result.success) {
        snprintf(out->error_message, sizeof(out->error_message), "%s",
                 wait.result.error_message[0] != '\0' ? wait.result.error_message : "async NAT detect failed");
        close(udp_sock);
        close(control_fd);
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
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    CLI::App                app{"NTRS NAT detection tool"};
    DetectArgs              args;
    ntrs_async_client_t*    async_client = NULL;
    event_base*             base = NULL;
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

    base = event_base_new();
    if (base == NULL) {
        printf("Failed to create event base\n");
        return 1;
    }

    async_client = ntrs_async_client_create(base);
    if (async_client == NULL) {
        printf("Failed to create async client\n");
        event_base_free(base);
        return 1;
    }

    if (args.verbose) {
        if (!args.bind_device.empty()) {
            printf("Binding probe socket to device: %s\n", args.bind_device.c_str());
        }
        if (!args.bind_ip.empty()) {
            printf("Binding probe socket to local IP: %s\n", args.bind_ip.c_str());
        }
    }

    if (!RunProbeSession(base, async_client, args, &probe)) {
        printf("NAT detection failed: %s\n", probe.error_message);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    args.probe1 = probe.probe1;
    args.probe2 = probe.probe2;
    PrintResult(&probe.nat_info, args.probe1, args.probe2);

    if (probe.udp_sock >= 0) {
        close(probe.udp_sock);
    }
    if (probe.control_fd >= 0) {
        close(probe.control_fd);
    }
    ntrs_async_client_destroy(async_client);
    event_base_free(base);
    return 0;
}
