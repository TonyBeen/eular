#include <arpa/inet.h>
#include <event2/event.h>
#include <ntrs_client.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string>
#include <vector>

#include <kcp_error.h>
#include <kcp_log.h>
#include <kcpp.h>

#ifndef OS_LINUX
#error "This example requires a Linux environment."
#endif

struct AsyncWait {
    event_base*         base;
    bool                done;
    ntrs_async_result_t result;
};

static event_base* g_base = NULL;

static void async_cb(const ntrs_async_result_t* result, void* user_data)
{
    AsyncWait* wait = static_cast<AsyncWait*>(user_data);
    if (wait == NULL || result == NULL) {
        return;
    }
    wait->result = *result;
    wait->done = true;
    event_base_loopbreak(wait->base);
}

static bool wait_async(event_base* base, AsyncWait* wait, int timeout_sec)
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

static bool parse_endpoint(const std::string& endpoint, std::string* host, uint16_t* port)
{
    size_t pos = endpoint.rfind(':');
    if (pos == std::string::npos || pos + 1 >= endpoint.size()) {
        return false;
    }
    int parsed_port = atoi(endpoint.c_str() + pos + 1);
    if (parsed_port <= 0 || parsed_port > 65535) {
        return false;
    }
    *host = endpoint.substr(0, pos);
    *port = static_cast<uint16_t>(parsed_port);
    return !host->empty();
}

static bool resolve_ipv4(const char* host, uint16_t port, sockaddr_t* out)
{
    if (host == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sin.sin_family = AF_INET;
    out->sin.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &out->sin.sin_addr) == 1) {
        return true;
    }
    return false;
}

static kcp_p2p_candidate_type_t candidate_type_from_text(const char* type)
{
    if (type == NULL) {
        return KCP_P2P_CANDIDATE_UNKNOWN;
    }
    if (strcmp(type, "host_local") == 0) {
        return KCP_P2P_CANDIDATE_HOST_LOCAL;
    }
    if (strcmp(type, "srflx_primary") == 0) {
        return KCP_P2P_CANDIDATE_SRFLX_PRIMARY;
    }
    if (strcmp(type, "srflx_secondary") == 0) {
        return KCP_P2P_CANDIDATE_SRFLX_SECONDARY;
    }
    return KCP_P2P_CANDIDATE_UNKNOWN;
}

static bool same_sockaddr(const sockaddr_t* a, const sockaddr_t* b)
{
    return a->sin.sin_family == b->sin.sin_family &&
           a->sin.sin_port == b->sin.sin_port &&
           a->sin.sin_addr.s_addr == b->sin.sin_addr.s_addr;
}

static bool same_candidate_endpoint(const ntrs_peer_candidate_t* a, const ntrs_peer_candidate_t* b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return a->port == b->port && strcmp(a->ip, b->ip) == 0;
}

static bool append_candidate_from_peer(const ntrs_peer_candidate_t* src, kcp_p2p_candidate_t* out, uint32_t out_cap,
                                       uint32_t* count)
{
    kcp_p2p_candidate_t candidate;

    if (src == NULL || out == NULL || count == NULL || *count >= out_cap || src->ip[0] == '\0' || src->port == 0) {
        return false;
    }

    memset(&candidate, 0, sizeof(candidate));
    if (!resolve_ipv4(src->ip, src->port, &candidate.addr)) {
        return false;
    }
    for (uint32_t i = 0; i < *count; ++i) {
        if (same_sockaddr(&out[i].addr, &candidate.addr)) {
            return false;
        }
    }

    candidate.type = candidate_type_from_text(src->type);
    candidate.priority = static_cast<uint8_t>(*count);
    out[(*count)++] = candidate;
    return true;
}

static bool candidate_is_local_only(const ntrs_peer_candidate_t* candidate)
{
    if (candidate == NULL) {
        return false;
    }
    return strcmp(candidate->type, "host_local") == 0;
}

static uint32_t convert_candidates(const ntrs_session_signal_t* signal, const ntrs_peer_candidate_t* preferred,
                                   kcp_p2p_candidate_t* out, uint32_t out_cap)
{
    uint32_t count = 0;
    if (signal == NULL || out == NULL || out_cap == 0) {
        return 0;
    }

    if (preferred != NULL) {
        append_candidate_from_peer(preferred, out, out_cap, &count);
        if (!candidate_is_local_only(preferred)) {
            return count;
        }
    }

    for (uint32_t i = 0; i < signal->candidate_count && count < out_cap; ++i) {
        if (preferred != NULL && same_candidate_endpoint(&signal->candidates[i], preferred)) {
            continue;
        }
        append_candidate_from_peer(&signal->candidates[i], out, out_cap, &count);
    }
    return count;
}

static void print_candidates(const ntrs_session_signal_t* signal)
{
    if (signal == NULL) {
        return;
    }
    printf("Session signal: peer=%s class=%u flags=0x%04x mapping=%u filtering=%u nat=%s candidates=%u\n",
           signal->peer_id, signal->peer_nat_class, signal->peer_nat_flags, signal->peer_mapping_behavior,
           signal->peer_filtering_behavior, signal->peer_nat_type, signal->candidate_count);
    for (uint32_t i = 0; i < signal->candidate_count; ++i) {
        printf("  candidate[%u]: type=%s endpoint=%s:%u\n", i,
               signal->candidates[i].type[0] == '\0' ? "candidate" : signal->candidates[i].type,
               signal->candidates[i].ip, signal->candidates[i].port);
    }
}

static const char* punch_order_text(uint8_t order)
{
    switch (order) {
    case NTRS_PUNCH_ORDER_SEND_FIRST:
        return "send_first";
    case NTRS_PUNCH_ORDER_WAIT_FIRST:
        return "wait_first";
    case NTRS_PUNCH_ORDER_SIMULTANEOUS:
        return "simultaneous";
    default:
        return "unknown";
    }
}

static const char* connect_role_text(uint8_t role)
{
    switch (role) {
    case NTRS_CONNECT_ROLE_INITIATOR:
        return "initiator";
    case NTRS_CONNECT_ROLE_LISTENER:
        return "listener";
    default:
        return "unknown";
    }
}

static bool wait_for_warmup_packet(int sock, uint32_t wait_ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set         rfds;
        struct timeval tv;
        struct sockaddr_storage src;
        socklen_t      src_len = sizeof(src);
        uint8_t        buffer[512];
        ssize_t        nread = 0;

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        if (select(sock + 1, &rfds, NULL, NULL, &tv) <= 0 || !FD_ISSET(sock, &rfds)) {
            continue;
        }
        nread = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&src, &src_len);
        if (nread <= 0) {
            continue;
        }
        buffer[nread] = '\0';
        if (strstr((const char*)buffer, "NTRS_PUNCH_REQ|") == (const char*)buffer ||
            strstr((const char*)buffer, "NTRS_PUNCH_ACK|") == (const char*)buffer) {
            return true;
        }
    }

    return false;
}

static void apply_signal_strategy_wait(int sock, const ntrs_session_signal_t* signal)
{
    uint32_t wait_ms = 0;

    if (signal == NULL) {
        return;
    }
    printf("Punch strategy: order=%s connect_role=%s warmup_rounds=%" PRIu32 " warmup_interval_ms=%" PRIu32 "\n",
           punch_order_text(signal->punch_order), connect_role_text(signal->connect_role), signal->warmup_rounds,
           signal->warmup_interval_ms);

    if (signal->punch_order != NTRS_PUNCH_ORDER_WAIT_FIRST) {
        return;
    }

    {
        uint32_t interval_ms = signal->warmup_interval_ms > 0 ? signal->warmup_interval_ms : 100u;
        uint32_t rounds = 3u;
        wait_ms = rounds > 1 ? interval_ms * (rounds - 1u) : interval_ms;
    }
    if (wait_ms == 0) {
        wait_ms = 200;
    }
    if (wait_for_warmup_packet(sock, wait_ms)) {
        printf("Warmup packet received early, continue immediately\n");
    } else {
        printf("Warmup window elapsed: waited=%" PRIu32 "ms\n", wait_ms);
    }
}

static void on_kcp_error(struct KcpContext* kcp_ctx, struct KcpConnection* kcp_connection, int32_t code)
{
    (void)kcp_ctx;
    char address[SOCKADDR_STRING_LEN] = {0};
    fprintf(stderr, "KCP error code=%d conn=%p remote=%s\n", code, static_cast<void*>(kcp_connection),
            kcp_connection == NULL ? "-" : kcp_connection_remote_address(kcp_connection, address, sizeof(address)));
    if (kcp_connection != NULL) {
        kcp_shutdown(kcp_connection);
    }
}

static void on_kcp_read(struct KcpConnection* conn, int32_t size)
{
    std::vector<char> data(static_cast<size_t>(size) + 1, 0);
    int32_t n = kcp_recv(conn, &data[0], static_cast<size_t>(size));
    if (n <= 0) {
        return;
    }
    printf("KCP RX: %.*s\n", n, &data[0]);
    const char reply[] = "kcp_peer reply";
    kcp_send(conn, reply, strlen(reply));
}

static void on_kcp_accepted(struct KcpContext* ctx, struct KcpConnection* conn, int32_t code)
{
    (void)ctx;
    if (code != NO_ERROR || conn == NULL) {
        fprintf(stderr, "KCP accept failed: %d\n", code);
        return;
    }
    char address[SOCKADDR_STRING_LEN] = {0};
    printf("KCP accepted remote=%s\n", kcp_connection_remote_address(conn, address, sizeof(address)));
    kcp_set_read_event_cb(conn, on_kcp_read);
}

static bool on_kcp_connect_request(struct KcpContext* ctx, const sockaddr_t* addr)
{
    int32_t status = kcp_accept(ctx, 5000);
    printf("KCP incoming SYN from %s:%u accept=%d\n", inet_ntoa(addr->sin.sin_addr), ntohs(addr->sin.sin_port), status);
    return status == NO_ERROR;
}

static void on_kcp_connected(struct KcpConnection* conn, int32_t code)
{
    if (code != NO_ERROR || conn == NULL) {
        fprintf(stderr, "KCP connect failed: %d\n", code);
        if (g_base != NULL) {
            event_base_loopbreak(g_base);
        }
        return;
    }
    char address[SOCKADDR_STRING_LEN] = {0};
    printf("KCP connected remote=%s\n", kcp_connection_remote_address(conn, address, sizeof(address)));
    kcp_set_read_event_cb(conn, on_kcp_read);
    const char hello[] = "kcp_peer hello";
    kcp_send(conn, hello, strlen(hello));
}

static void on_kcp_closed(struct KcpConnection* conn, int32_t code)
{
    char address[SOCKADDR_STRING_LEN] = {0};
    printf("KCP closed code=%d remote=%s\n", code, kcp_connection_remote_address(conn, address, sizeof(address)));
}

static bool run_punch(ntrs_async_client_t* client, event_base* base, int udp_sock, const ntrs_session_signal_t* signal,
                      ntrs_peer_candidate_t* selected)
{
    AsyncWait wait;
    uint64_t request_id = 0;
    apply_signal_strategy_wait(udp_sock, signal);
    memset(&wait, 0, sizeof(wait));
    wait.base = base;
    if (!ntrs_async_try_udp_hole_punch(client, &request_id, udp_sock, signal->candidates, signal->candidate_count, 8,
                                       200, async_cb, &wait) ||
        !wait_async(base, &wait, 4) || !wait.result.success) {
        printf("UDP hole punch failed\n");
        return false;
    }
    printf("UDP hole punch ok selected=%s:%u type=%s\n", wait.result.selected_candidate.ip,
           wait.result.selected_candidate.port,
           wait.result.selected_candidate.type[0] == '\0' ? "candidate" : wait.result.selected_candidate.type);
    if (selected != NULL) {
        *selected = wait.result.selected_candidate;
    }
    return true;
}

int main(int argc, char** argv)
{
    bool                     verbose = false;
    std::string              bind_ip;
    std::string              bind_device;
    std::string              auth_token = "ntrs-dev-secret";
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
        if (strcmp(argv[i], "--auth-token") == 0 && i + 1 < argc) {
            auth_token = argv[++i];
            continue;
        }
        positional.push_back(argv[i]);
    }

    if (positional.size() < 5) {
        printf("Usage: %s [-v] [--bind-ip ip] [--bind-device ifname] [--auth-token token] <node_host> <node_port> <peer_id> <device_id> <dst_peer|-> [dst_device|-]\n",
               argv[0]);
        return 1;
    }

    std::string node_host = positional[0];
    uint16_t    node_port = static_cast<uint16_t>(atoi(positional[1].c_str()));
    std::string peer_id = positional[2];
    std::string device_id = positional[3];
    std::string dst_peer = positional[4];
    std::string dst_device = positional.size() > 5 ? positional[5] : "";
    if (dst_peer == "-") {
        dst_peer.clear();
    }
    if (dst_device == "-") {
        dst_device.clear();
    }

    kcp_log_level(verbose ? LOG_LEVEL_INFO : LOG_LEVEL_WARN);
    g_base = event_base_new();
    if (g_base == NULL) {
        fprintf(stderr, "event_base_new failed\n");
        return 1;
    }

    struct KcpContext* kcp = kcp_context_create(g_base, on_kcp_error, NULL);
    if (kcp == NULL) {
        fprintf(stderr, "kcp_context_create failed\n");
        event_base_free(g_base);
        return 1;
    }

    sockaddr_t local;
    memset(&local, 0, sizeof(local));
    local.sin.sin_family = AF_INET;
    local.sin.sin_port = 0;
    local.sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!bind_ip.empty() && inet_pton(AF_INET, bind_ip.c_str(), &local.sin.sin_addr) != 1) {
        fprintf(stderr, "invalid bind ip: %s\n", bind_ip.c_str());
        return 1;
    }
    int32_t status = kcp_bind(kcp, &local, bind_device.empty() ? NULL : bind_device.c_str());
    if (status != NO_ERROR) {
        fprintf(stderr, "kcp_bind failed: %d\n", status);
        return 1;
    }
    int udp_sock = kcp_context_udp_socket(kcp);

    ntrs_async_client_t* client = ntrs_async_client_create(g_base);
    if (client == NULL) {
        fprintf(stderr, "ntrs_async_client_create failed\n");
        return 1;
    }

    uint64_t  request_id = 0;
    AsyncWait wait;
    memset(&wait, 0, sizeof(wait));
    wait.base = g_base;
    if (!ntrs_async_connect_control(client, &request_id, node_host.c_str(), node_port, 3000, async_cb, &wait) ||
        !wait_async(g_base, &wait, 5) || !wait.result.success) {
        fprintf(stderr, "connect node failed: %s\n", wait.result.error_message);
        return 1;
    }
    int control_fd = wait.result.control_fd;

    memset(&wait, 0, sizeof(wait));
    wait.base = g_base;
    if (!ntrs_async_auth(client, &request_id, control_fd, peer_id.c_str(), auth_token.c_str(), async_cb, &wait) ||
        !wait_async(g_base, &wait, 5) || !wait.result.success) {
        fprintf(stderr, "auth failed: %s\n", wait.result.error_message);
        return 1;
    }
    std::string session_token = wait.result.session_token;

    memset(&wait, 0, sizeof(wait));
    wait.base = g_base;
    if (!ntrs_async_request_probe_endpoints(client, &request_id, control_fd, session_token.c_str(), async_cb, &wait) ||
        !wait_async(g_base, &wait, 5) || !wait.result.success) {
        fprintf(stderr, "request probe endpoints failed: %s\n", wait.result.error_message);
        return 1;
    }
    std::string probe1 = wait.result.probe1;
    std::string probe2 = wait.result.probe2;
    std::string probe1_host;
    std::string probe2_host;
    uint16_t    probe1_port = 0;
    uint16_t    probe2_port = 0;
    if (!parse_endpoint(probe1, &probe1_host, &probe1_port) || !parse_endpoint(probe2, &probe2_host, &probe2_port)) {
        fprintf(stderr, "invalid probe endpoints: %s %s\n", probe1.c_str(), probe2.c_str());
        return 1;
    }
    printf("NTRS probe endpoints: probe1=%s probe2=%s\n", probe1.c_str(), probe2.c_str());

    ntrs_detect_nat_options_t detect_options;
    ntrs_detect_nat_options_init(&detect_options);
    detect_options.enable_filter_probe = true;
    detect_options.verbose = verbose;
    memset(&wait, 0, sizeof(wait));
    wait.base = g_base;
    if (!ntrs_async_detect_nat(client, &request_id, udp_sock, probe1_host.c_str(), probe1_port, probe2_host.c_str(),
                               probe2_port, control_fd, session_token.c_str(), &detect_options, async_cb, &wait) ||
        !wait_async(g_base, &wait, 10) || !wait.result.success) {
        fprintf(stderr, "detect NAT failed: %s\n", wait.result.error_message);
        return 1;
    }
    ntrs_nat_info_t nat = wait.result.nat_info;
    printf("NAT summary: local=%s:%u srflx=%s:%u srflx2=%s:%u class=%u flags=0x%04x mapping=%u filtering=%u type=%s risk=%s\n",
           nat.local_ip, nat.local_port, nat.srflx_ip, nat.srflx_port, nat.srflx_ip_2, nat.srflx_port_2,
           nat.nat_class, nat.nat_flags, nat.mapping_behavior, nat.filtering_behavior, nat.nat_type, nat.nat_risk);

    memset(&wait, 0, sizeof(wait));
    wait.base = g_base;
    if (!ntrs_async_register_peer(client, &request_id, control_fd, peer_id.c_str(), device_id.c_str(),
                                  session_token.c_str(), &nat, async_cb, &wait) ||
        !wait_async(g_base, &wait, 5) || !wait.result.success) {
        fprintf(stderr, "register failed: %s\n", wait.result.error_message);
        return 1;
    }
    printf("REGISTER ok peer=%s device=%s\n", peer_id.c_str(), device_id.c_str());

    kcp_set_accept_cb(kcp, on_kcp_accepted);
    kcp_set_close_cb(kcp, on_kcp_closed);
    if (dst_peer.empty()) {
        if (kcp_listen(kcp, on_kcp_connect_request) != NO_ERROR) {
            fprintf(stderr, "kcp_listen failed\n");
            return 1;
        }
        printf("KCP listening for incoming SYN\n");
    }

    ntrs_session_signal_t signal;
    ntrs_peer_candidate_t selected_candidate;
    memset(&signal, 0, sizeof(signal));
    memset(&selected_candidate, 0, sizeof(selected_candidate));
    if (dst_peer.empty()) {
        printf("waiting for session signal...\n");
        memset(&wait, 0, sizeof(wait));
        wait.base = g_base;
        if (!ntrs_async_wait_for_signal(client, &request_id, control_fd, 30000, async_cb, &wait) ||
            !wait_async(g_base, &wait, 31) || !wait.result.success) {
            fprintf(stderr, "wait signal failed: %s\n", wait.result.error_message);
            return 1;
        }
        signal = wait.result.session_signal;
        print_candidates(&signal);
        run_punch(client, g_base, udp_sock, &signal, &selected_candidate);
    } else {
        memset(&wait, 0, sizeof(wait));
        wait.base = g_base;
        if (!ntrs_async_create_session(client, &request_id, control_fd, peer_id.c_str(), device_id.c_str(),
                                       dst_peer.c_str(), dst_device.c_str(), session_token.c_str(), async_cb, &wait) ||
            !wait_async(g_base, &wait, 10) || !wait.result.success) {
            fprintf(stderr, "create session failed: %s\n", wait.result.error_message);
            return 1;
        }
        signal = wait.result.session_signal;
        print_candidates(&signal);
        run_punch(client, g_base, udp_sock, &signal, &selected_candidate);
        kcp_p2p_candidate_t candidates[KCP_P2P_MAX_CANDIDATES];
        uint32_t count = convert_candidates(&signal,
                                            selected_candidate.ip[0] == '\0' ? NULL : &selected_candidate,
                                            candidates, KCP_P2P_MAX_CANDIDATES);
        if (count == 0) {
            fprintf(stderr, "no valid KCP candidates\n");
            return 1;
        }
        status = kcp_connect_candidates(kcp, candidates, count, 5000, on_kcp_connected);
        if (status != NO_ERROR) {
            fprintf(stderr, "kcp_connect_candidates failed: %d\n", status);
            return 1;
        }
    }

    event_base_dispatch(g_base);

    memset(&wait, 0, sizeof(wait));
    wait.base = g_base;
    ntrs_async_unregister_peer(client, &request_id, control_fd, peer_id.c_str(), session_token.c_str(), "client_exit",
                               async_cb, &wait);
    wait_async(g_base, &wait, 2);
    close(control_fd);
    ntrs_async_client_destroy(client);
    kcp_context_destroy(kcp);
    event_base_free(g_base);
    return 0;
}
