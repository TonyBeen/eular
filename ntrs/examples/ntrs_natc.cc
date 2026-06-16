#include <errno.h>
#include <ntrs_client.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <event2/event.h>
#include <event2/util.h>
#include <utils/CLI11.hpp>
#if defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#endif

namespace {

struct DetectArgs {
    std::string node_host;
    uint16_t    node_port = 0;
    std::string peer_id = "probe_peer";
    std::string bind_ip;
    std::string bind_device;
    std::string probe1;
    std::string probe2;
    bool        verbose = false;
};

static const char* nat_type_title(ntrs_nat_class_t nat_class)
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
    default:
        return "Unknown";
    }
}

static const char* nat_type_hint(const ntrs_nat_info_t* nat)
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
    default:
        if ((nat->nat_flags & NTRS_NAT_FLAG_UDP_BLOCKED) != 0) {
            return "Basic UDP probes failed; verify that UDP is allowed on this network.";
        }
        return "The current sample is insufficient for a stronger conclusion.";
    }
}

static const char* mapping_behavior_title(ntrs_mapping_behavior_t behavior)
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

static const char* filtering_behavior_title(ntrs_filtering_behavior_t behavior)
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

static void append_flag_text(std::vector<std::string>* flags, bool enabled, const char* text)
{
    if (flags != NULL && enabled && text != NULL && text[0] != '\0') {
        flags->push_back(text);
    }
}

static std::string join_flags(const std::vector<std::string>& flags)
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

static std::string format_flags(ntrs_nat_flags_t nat_flags)
{
    std::vector<std::string> flags;

    append_flag_text(&flags, (nat_flags & NTRS_NAT_FLAG_UDP_BLOCKED) != 0, "udp_blocked");
    append_flag_text(&flags, (nat_flags & NTRS_NAT_FLAG_PROBE_DEGRADED) != 0, "degraded_probe_coverage");
    append_flag_text(&flags, (nat_flags & NTRS_NAT_FLAG_MAPPING_UNSTABLE) != 0,
                     "observed_mapping_instability");
    append_flag_text(&flags, (nat_flags & NTRS_NAT_FLAG_MULTI_EXTERNAL_IP) != 0,
                     "multiple_external_ips_observed");
    append_flag_text(&flags, (nat_flags & NTRS_NAT_FLAG_LOCAL_ADDR_PUBLIC) != 0, "local_public_address");

    if (flags.empty()) {
        return "none";
    }
    return join_flags(flags);
}

static void print_result(const ntrs_nat_info_t* nat, const std::string& probe1, const std::string& probe2)
{
    printf("Detection Result: %s\n", nat_type_title(nat->nat_class));
    printf("Summary: %s\n", nat_type_hint(nat));
    printf("Risk: %s\n", nat->nat_risk);
    printf("Signals: %s\n", format_flags(nat->nat_flags).c_str());
    printf("Mapping Behavior: %s\n", mapping_behavior_title(nat->mapping_behavior));
    printf("Filtering Behavior: %s\n", filtering_behavior_title(nat->filtering_behavior));
    printf("Probe Endpoints: probe1=%s probe2=%s\n", probe1.c_str(), probe2.empty() ? "-" : probe2.c_str());
    printf("Local Address: %s:%u\n", nat->local_ip, nat->local_port);
    printf("Public Mapping #1: %s:%u\n", nat->srflx_ip, nat->srflx_port);
    printf("Public Mapping #2: %s:%u\n", nat->srflx_ip_2, nat->srflx_port_2);
    printf("Samples: rounds=%d p1=%d p2=%d p1_map=%d p2_map=%d rtt1=%dms rtt2=%dms\n", nat->probe_rounds,
           nat->probe1_success_count, nat->probe2_success_count, nat->probe1_distinct_mappings,
           nat->probe2_distinct_mappings, nat->probe1_rtt_ms, nat->probe2_rtt_ms);
}

static void verbose_log(bool verbose, const char* message)
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

static void async_result_callback(const ntrs_async_result_t* result, void* user_data)
{
    AsyncResultWait* wait = static_cast<AsyncResultWait*>(user_data);
    if (wait == NULL || result == NULL) {
        return;
    }

    wait->result = *result;
    wait->done = true;
    event_base_loopbreak(wait->base);
}

static bool wait_async_result(event_base* base, AsyncResultWait* wait, int timeout_sec)
{
    time_t deadline = time(NULL) + timeout_sec;
    while (wait != NULL && !wait->done && time(NULL) < deadline) {
        if (event_base_loop(base, EVLOOP_ONCE) != 0) {
            break;
        }
    }
    return wait != NULL && wait->done;
}

static bool set_nonblocking_fd(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool parse_endpoint_text(const std::string& endpoint, std::string* host, uint16_t* port)
{
    size_t colon = endpoint.rfind(':');
    int    parsed = 0;

    if (host == NULL || port == NULL || colon == std::string::npos) {
        return false;
    }
    *host = endpoint.substr(0, colon);
    if (host->empty() || host->find(':') != std::string::npos) {
        return false;
    }
    parsed = atoi(endpoint.substr(colon + 1).c_str());
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

static bool resolve_bind_ipv4(const std::string& bind_ip, const std::string& bind_device, std::string* resolved_ip)
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

#if defined(SIOCGIFADDR)
    if (!bind_device.empty()) {
        int          sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;
        char         ip_buffer[INET_ADDRSTRLEN] = {0};

        if (sock < 0) {
            return false;
        }
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, bind_device.c_str(), IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
        ifr.ifr_addr.sa_family = AF_INET;
        if (ioctl(sock, SIOCGIFADDR, &ifr) != 0) {
            close(sock);
            return false;
        }
        close(sock);
        if (inet_ntop(AF_INET, &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr, ip_buffer, sizeof(ip_buffer)) ==
            NULL) {
            return false;
        }
        *resolved_ip = ip_buffer;
        return true;
    }
#else
    (void)bind_device;
#endif

    resolved_ip->clear();
    return true;
}

static int create_udp_probe_socket(const std::string& bind_ip, const std::string& bind_device)
{
    int                sock = -1;
    std::string        resolved_ip;
    struct sockaddr_in local_addr;

    if (!resolve_bind_ipv4(bind_ip, bind_device, &resolved_ip)) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }
    if (!set_nonblocking_fd(sock)) {
        close(sock);
        return -1;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(0);
    if (!resolved_ip.empty()) {
        if (inet_pton(AF_INET, resolved_ip.c_str(), &local_addr.sin_addr) != 1) {
            close(sock);
            return -1;
        }
    } else {
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(sock, (const struct sockaddr*)&local_addr, sizeof(local_addr)) != 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool run_probe_session(event_base* base, ntrs_async_client_t* async_client, const DetectArgs& args,
                              ProbeSession* out)
{
    int                       control_fd = -1;
    int                       udp_sock = -1;
    char                      session_token[NTRS_MAX_TEXT_LEN];
    uint32_t                  lease_default_sec = 0;
    char                      probe1[NTRS_MAX_TEXT_LEN];
    char                      probe2[NTRS_MAX_TEXT_LEN];
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

    control_fd = ntrs_connect_control(args.node_host.c_str(), args.node_port);
    if (control_fd < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "connect control failed");
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

    if (!parse_endpoint_text(probe1, &probe1_host, &probe1_port)) {
        snprintf(out->error_message, sizeof(out->error_message), "invalid probe1 endpoint");
        close(control_fd);
        return false;
    }
    if (probe2[0] != '\0' && !parse_endpoint_text(probe2, &probe2_host, &probe2_port)) {
        snprintf(out->error_message, sizeof(out->error_message), "invalid probe2 endpoint");
        close(control_fd);
        return false;
    }

    udp_sock = create_udp_probe_socket(args.bind_ip, args.bind_device);
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
                               &detect_options, async_result_callback, &wait) ||
        !wait_async_result(base, &wait, 10) || !wait.result.success) {
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
    app.add_option("--bind-ip", args.bind_ip, "Bind the probe socket to a local IPv4 address");
    app.add_option("--bind-device", args.bind_device, "Bind the probe socket to a network interface");
    app.add_option("--probe1", args.probe1, "Explicit probe1 endpoint in host:port format");
    app.add_option("--probe2", args.probe2, "Explicit probe2 endpoint in host:port format");
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
        verbose_log(true, "Connecting to node...");
        verbose_log(true, "Authenticating...");
        if (args.probe1.empty()) {
            verbose_log(true, "Requesting probe endpoints...");
        }
        if (!args.bind_device.empty()) {
            printf("Binding probe socket to device: %s\n", args.bind_device.c_str());
        }
        if (!args.bind_ip.empty()) {
            printf("Binding probe socket to local IP: %s\n", args.bind_ip.c_str());
        }
        verbose_log(true, "Starting NAT detection...");
    }

    if (!run_probe_session(base, async_client, args, &probe)) {
        printf("NAT detection failed: %s\n", probe.error_message);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    args.probe1 = probe.probe1;
    args.probe2 = probe.probe2;
    print_result(&probe.nat_info, args.probe1, args.probe2);

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
