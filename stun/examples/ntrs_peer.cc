#include <ntrs_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include <event2/event.h>
#include <event2/util.h>
#include <sys/socket.h>
#include <sys/time.h>

struct AsyncResultWait {
    event_base*         base;
    bool                done;
    ntrs_async_result_t result;
};

struct NatProbeWait {
    event_base*             base;
    bool                    done;
    ntrs_nat_probe_result_t result;
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

static bool wait_async_result(event_base* base, AsyncResultWait* wait, int timeout_sec)
{
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (wait != NULL && !wait->done && std::chrono::steady_clock::now() < deadline) {
        if (event_base_loop(base, EVLOOP_ONCE) != 0) {
            break;
        }
    }
    return wait != NULL && wait->done;
}

static bool wait_nat_probe_result(event_base* base, NatProbeWait* wait, int timeout_sec)
{
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (wait != NULL && !wait->done && std::chrono::steady_clock::now() < deadline) {
        if (event_base_loop(base, EVLOOP_ONCE) != 0) {
            break;
        }
    }
    return wait != NULL && wait->done;
}

static void try_hole_punch_from_async_signal(ntrs_async_client_t* client, event_base* base, int sock,
                                             const ntrs_session_signal_t* signal, const char* from, bool verbose)
{
    AsyncResultWait wait_punch;
    uint64_t        request_id = 0;

    if (signal == NULL) {
        return;
    }

    printf("Session signal(%s): peer=%s class=%u flags=0x%04x mapping=%u filtering=%u nat=%s candidates=%u\n", from,
           signal->peer_id, signal->peer_nat_class, signal->peer_nat_flags, signal->peer_mapping_behavior,
           signal->peer_filtering_behavior, signal->peer_nat_type, signal->candidate_count);
    if (verbose) {
        for (uint32_t i = 0; i < signal->candidate_count; ++i) {
            printf("  candidate[%u]: type=%s endpoint=%s:%u\n", i,
                   signal->candidates[i].type[0] == '\0' ? "candidate" : signal->candidates[i].type,
                   signal->candidates[i].ip, signal->candidates[i].port);
        }
    }

    memset(&wait_punch, 0, sizeof(wait_punch));
    wait_punch.base = base;
    if (ntrs_async_try_udp_hole_punch(client, &request_id, sock, signal->candidates, signal->candidate_count, 8, 200,
                                      async_result_callback, &wait_punch) &&
        wait_async_result(base, &wait_punch, 3) && wait_punch.result.success) {
        printf("KCP hole punch result (%s): ok selected=%s:%u type=%s\n", from,
               wait_punch.result.selected_candidate.ip, wait_punch.result.selected_candidate.port,
               wait_punch.result.selected_candidate.type[0] == '\0' ? "candidate"
                                                                    : wait_punch.result.selected_candidate.type);
    } else {
        printf("KCP hole punch result (%s): failed\n", from);
    }
}

int main(int argc, char** argv)
{
    bool                     verbose = false;
    std::string              bind_ip;
    std::string              bind_device;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
            continue;
        }
        if ((strcmp(argv[i], "--bind-ip") == 0 || strcmp(argv[i], "--bind-device") == 0) && i + 1 < argc) {
            if (strcmp(argv[i], "--bind-ip") == 0) {
                bind_ip = argv[++i];
            } else {
                bind_device = argv[++i];
            }
            continue;
        }
        positional.push_back(argv[i]);
    }

    if (positional.size() < 2) {
        printf(
            "Usage: %s [-v|--verbose] [--bind-ip <ip>] [--bind-device <ifname>] <ntrs_ip> <ntrs_port> "
            "[peer_id device_id [dst_peer|-] [stun1_host:port] [stun2_host:port]]\n",
            argv[0]);
        return 1;
    }

    std::string ntrs_ip = positional[0];
    int         ntrs_port = atoi(positional[1].c_str());
    bool        probe_only = positional.size() < 4;
    std::string peer_id = (positional.size() > 2) ? positional[2] : "probe_peer";
    std::string device_id = (positional.size() > 3) ? positional[3] : "probe_device";
    std::string dst_peer = (positional.size() > 4) ? positional[4] : "";
    std::string stun1 = (positional.size() > 5) ? positional[5] : "";
    std::string stun2 = (positional.size() > 6) ? positional[6] : "";
    if (dst_peer == "-") {
        dst_peer.clear();
    }

    event_base* base = event_base_new();
    if (base == NULL) {
        printf("create event_base failed\n");
        return 1;
    }
    ntrs_async_client_t* async_client = ntrs_async_client_create(base);
    if (async_client == NULL) {
        printf("create ntrs async client failed\n");
        event_base_free(base);
        return 1;
    }

    uint64_t                request_id = 0;
    ntrs_nat_probe_flow_t*  nat_flow = ntrs_nat_probe_flow_create(async_client);
    ntrs_nat_probe_params_t probe_params;
    NatProbeWait            wait_nat;
    int                     fd = -1;
    int                     probe_sock = -1;
    std::string             control_session_token;
    const ntrs_nat_info_t*  nat = NULL;

    if (nat_flow == NULL) {
        printf("create nat probe flow failed\n");
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    ntrs_nat_probe_params_init(&probe_params);
    probe_params.node_host = ntrs_ip.c_str();
    probe_params.node_port = (uint16_t)ntrs_port;
    probe_params.peer_id = peer_id.c_str();
    probe_params.bind_ip = bind_ip.empty() ? NULL : bind_ip.c_str();
    probe_params.bind_device = bind_device.empty() ? NULL : bind_device.c_str();
    probe_params.stun1 = stun1.empty() ? NULL : stun1.c_str();
    probe_params.stun2 = stun2.empty() ? NULL : stun2.c_str();
    probe_params.detect_options.enable_filter_probe = true;
    probe_params.detect_options.verbose = verbose;

    memset(&wait_nat, 0, sizeof(wait_nat));
    wait_nat.base = base;
    if (!ntrs_nat_probe_flow_start(nat_flow, &probe_params, nat_probe_callback, &wait_nat) ||
        !wait_nat_probe_result(base, &wait_nat, 8) || !wait_nat.result.success) {
        printf("async NAT detect failed: %s\n", wait_nat.result.error_message);
        ntrs_nat_probe_flow_destroy(nat_flow);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    fd = wait_nat.result.control_fd;
    probe_sock = wait_nat.result.udp_sock;
    control_session_token = wait_nat.result.session_token;
    stun1 = wait_nat.result.stun1;
    stun2 = wait_nat.result.stun2;
    printf("NTRS probe endpoints: stun1=%s stun2=%s\n", stun1.c_str(), stun2.empty() ? "-" : stun2.c_str());

    nat = &wait_nat.result.nat_info;
    printf("NAT summary: local=%s:%u srflx=%s:%u srflx2=%s:%u class=%u flags=0x%04x mapping=%u filtering=%u type=%s "
           "risk=%s\n",
           nat->local_ip, nat->local_port, nat->srflx_ip, nat->srflx_port, nat->srflx_ip_2, nat->srflx_port_2,
           nat->nat_class, nat->nat_flags, nat->mapping_behavior, nat->filtering_behavior, nat->nat_type,
           nat->nat_risk);

    ntrs_nat_probe_flow_destroy(nat_flow);

    if (probe_only) {
        close(probe_sock);
        close(fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 0;
    }

    AsyncResultWait wait_register;
    memset(&wait_register, 0, sizeof(wait_register));
    wait_register.base = base;
    if (!ntrs_async_register_peer(async_client, &request_id, fd, peer_id.c_str(), device_id.c_str(),
                                  control_session_token.c_str(), nat, async_result_callback, &wait_register) ||
        !wait_async_result(base, &wait_register, 3) || !wait_register.result.success) {
        printf("async register failed: %s\n", wait_register.result.error_message);
        close(probe_sock);
        close(fd);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }
    printf("REGISTER ok peer=%s device=%s\n", peer_id.c_str(), device_id.c_str());

    if (!dst_peer.empty()) {
        AsyncResultWait wait_session;
        memset(&wait_session, 0, sizeof(wait_session));
        wait_session.base = base;
        if (ntrs_async_create_session(async_client, &request_id, fd, peer_id.c_str(), dst_peer.c_str(),
                                      control_session_token.c_str(), async_result_callback, &wait_session) &&
            wait_async_result(base, &wait_session, 5) && wait_session.result.success) {
            try_hole_punch_from_async_signal(async_client, base, probe_sock, &wait_session.result.session_signal,
                                             "session_create_rsp", verbose);
        } else {
            printf("async create session failed: %s\n", wait_session.result.error_message);
        }
    } else {
        AsyncResultWait wait_signal;
        memset(&wait_signal, 0, sizeof(wait_signal));
        wait_signal.base = base;
        printf("waiting for session signal...\n");
        if (ntrs_async_wait_for_signal(async_client, &request_id, fd, 15000, async_result_callback, &wait_signal) &&
            wait_async_result(base, &wait_signal, 16) && wait_signal.result.success) {
            try_hole_punch_from_async_signal(async_client, base, probe_sock, &wait_signal.result.session_signal,
                                             "session_notify", verbose);
        }
    }

    AsyncResultWait wait_unregister;
    memset(&wait_unregister, 0, sizeof(wait_unregister));
    wait_unregister.base = base;
    ntrs_async_unregister_peer(async_client, &request_id, fd, peer_id.c_str(), control_session_token.c_str(),
                               "client_exit", async_result_callback, &wait_unregister);
    wait_async_result(base, &wait_unregister, 2);

    close(probe_sock);
    close(fd);
    ntrs_async_client_destroy(async_client);
    event_base_free(base);
    return 0;
}
