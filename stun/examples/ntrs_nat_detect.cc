#include <errno.h>
#include <ntrs_client.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <event2/event.h>
#include <event2/util.h>
#include <utils/CLI11.hpp>

namespace {

struct DetectArgs {
    std::string node_host;
    uint16_t    node_port = 0;
    std::string peer_id = "probe_peer";
    std::string bind_ip;
    std::string bind_device;
    std::string stun1;
    std::string stun2;
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

static void print_result(const ntrs_nat_info_t* nat, const std::string& stun1, const std::string& stun2)
{
    printf("Detection Result: %s\n", nat_type_title(nat->nat_class));
    printf("Summary: %s\n", nat_type_hint(nat));
    printf("Risk: %s\n", nat->nat_risk);
    printf("Signals: %s\n", format_flags(nat->nat_flags).c_str());
    printf("Mapping Behavior: %s\n", mapping_behavior_title(nat->mapping_behavior));
    printf("Filtering Behavior: %s\n", filtering_behavior_title(nat->filtering_behavior));
    printf("Probe Endpoints: stun1=%s stun2=%s\n", stun1.c_str(), stun2.empty() ? "-" : stun2.c_str());
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

struct NatProbeWait {
    event_base*             base;
    bool                    done;
    ntrs_nat_probe_result_t result;
};

static void nat_probe_callback(const ntrs_nat_probe_result_t* result, void* user_data)
{
    NatProbeWait* wait = static_cast<NatProbeWait*>(user_data);
    if (wait == NULL || result == NULL) {
        return;
    }

    wait->result = *result;
    wait->done = true;
    event_base_loopbreak(wait->base);
}

static bool wait_nat_probe_result(event_base* base, NatProbeWait* wait, int timeout_sec)
{
    time_t deadline = time(NULL) + timeout_sec;
    while (wait != NULL && !wait->done && time(NULL) < deadline) {
        if (event_base_loop(base, EVLOOP_ONCE) != 0) {
            break;
        }
    }
    return wait != NULL && wait->done;
}

}  // namespace

int main(int argc, char** argv)
{
    CLI::App                app{"NTRS NAT detection tool"};
    DetectArgs              args;
    ntrs_nat_probe_params_t probe_params;
    ntrs_nat_probe_flow_t*  probe_flow = NULL;
    ntrs_async_client_t*    async_client = NULL;
    event_base*             base = NULL;
    NatProbeWait            wait;

    app.add_option("node_host", args.node_host, "Node host or IP")->required();
    app.add_option("node_port", args.node_port, "Node control port")->required();
    app.add_option("--peer-id", args.peer_id, "peer_id used for authentication");
    app.add_option("--bind-ip", args.bind_ip, "Bind the probe socket to a local IPv4 address");
    app.add_option("--bind-device", args.bind_device, "Bind the probe socket to a network interface");
    app.add_option("--stun1", args.stun1, "Explicit STUN1 endpoint in host:port format");
    app.add_option("--stun2", args.stun2, "Explicit STUN2 endpoint in host:port format");
    app.add_flag("-v,--verbose", args.verbose, "Print step-by-step probe progress");

    CLI11_PARSE(app, argc, argv);

    memset(&wait, 0, sizeof(wait));
    ntrs_nat_probe_params_init(&probe_params);
    probe_params.node_host = args.node_host.c_str();
    probe_params.node_port = args.node_port;
    probe_params.peer_id = args.peer_id.c_str();
    probe_params.bind_ip = args.bind_ip.empty() ? NULL : args.bind_ip.c_str();
    probe_params.bind_device = args.bind_device.empty() ? NULL : args.bind_device.c_str();
    probe_params.stun1 = args.stun1.empty() ? NULL : args.stun1.c_str();
    probe_params.stun2 = args.stun2.empty() ? NULL : args.stun2.c_str();
    probe_params.detect_options.enable_filter_probe = true;
    probe_params.detect_options.verbose = args.verbose;

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

    probe_flow = ntrs_nat_probe_flow_create(async_client);
    if (probe_flow == NULL) {
        printf("Failed to create nat probe flow\n");
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    if (args.verbose) {
        verbose_log(true, "Connecting to node...");
        verbose_log(true, "Authenticating...");
        if (args.stun1.empty()) {
            verbose_log(true, "Requesting STUN endpoints...");
        }
        if (!args.bind_device.empty()) {
            printf("Binding probe socket to device: %s\n", args.bind_device.c_str());
        }
        if (!args.bind_ip.empty()) {
            printf("Binding probe socket to local IP: %s\n", args.bind_ip.c_str());
        }
        verbose_log(true, "Starting NAT detection...");
    }

    wait.base = base;
    if (!ntrs_nat_probe_flow_start(probe_flow, &probe_params, nat_probe_callback, &wait) ||
        !wait_nat_probe_result(base, &wait, 10) || !wait.result.success) {
        printf("NAT detection failed: %s\n", wait.result.error_message);
        ntrs_nat_probe_flow_destroy(probe_flow);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    args.stun1 = wait.result.stun1;
    args.stun2 = wait.result.stun2;
    print_result(&wait.result.nat_info, args.stun1, args.stun2);

    if (wait.result.udp_sock >= 0) {
        close(wait.result.udp_sock);
    }
    if (wait.result.control_fd >= 0) {
        close(wait.result.control_fd);
    }
    ntrs_nat_probe_flow_destroy(probe_flow);
    ntrs_async_client_destroy(async_client);
    event_base_free(base);
    return 0;
}
