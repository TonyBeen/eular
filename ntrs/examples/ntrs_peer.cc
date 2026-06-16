#include <ntrs_client.h>
#include <ntrs_binary_protocol.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <random>
#include <string>
#include <vector>
#include <inttypes.h>

#include <event2/event.h>
#include <event2/util.h>
#include <sys/socket.h>
#include <sys/time.h>
#if defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#endif

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

struct PunchFrameMeta {
    ntrs_binary_frame_type_t frame_type;
    uint32_t                 request_id;
    uint32_t                 sequence;
    uint8_t                  token[8];
    uint8_t                  token_len;
    char                     candidate_type[NTRS_MAX_TEXT_LEN];
};

/**
 * @brief 接收一帧探测相关 UDP 负载。
 */
static bool recv_probe_packet(int sock, char* buffer, size_t buffer_size, size_t* out_len,
                              struct sockaddr_storage* src, socklen_t* src_len, int timeout_ms);

/**
 * @brief 解析二进制打洞帧。
 */
static bool parse_punch_frame(const uint8_t* payload, size_t payload_len, PunchFrameMeta* out);

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
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    while (wait != NULL && !wait->done && std::chrono::steady_clock::now() < deadline) {
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

static uint64_t current_time_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000ull);
}

static bool parse_endpoint_text(const std::string& endpoint, std::string* host, uint16_t* port)
{
    size_t      colon = endpoint.rfind(':');
    std::string host_part;
    std::string port_part;
    int         parsed = 0;

    if (host == NULL || port == NULL || colon == std::string::npos) {
        return false;
    }

    host_part = endpoint.substr(0, colon);
    port_part = endpoint.substr(colon + 1);
    if (host_part.empty() || port_part.empty() || host_part.find(':') != std::string::npos) {
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
        int          sock = -1;
        struct ifreq ifr;
        char         ip_buffer[INET_ADDRSTRLEN] = {0};

        sock = socket(AF_INET, SOCK_DGRAM, 0);
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

static const char* punch_order_text(ntrs_punch_order_t order)
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

static const char* connect_role_text(ntrs_connect_role_t role)
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
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        fd_set         rfds;
        struct timeval tv;
        uint8_t        buffer[512];
        size_t         nread = 0;
        struct sockaddr_storage src;
        socklen_t      src_len = sizeof(src);
        PunchFrameMeta meta;

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        if (select(sock + 1, &rfds, NULL, NULL, &tv) <= 0 || !FD_ISSET(sock, &rfds)) {
            continue;
        }
        if (!recv_probe_packet(sock, (char*)buffer, sizeof(buffer), &nread, &src, &src_len, 20)) {
            continue;
        }
        if (parse_punch_frame(buffer, nread, &meta)) {
            return true;
        }
    }

    return false;
}

static void apply_signal_strategy_wait(int sock, const ntrs_session_signal_t* signal, const char* from)
{
    uint32_t wait_ms = 0;

    if (signal == NULL) {
        return;
    }
    printf("Punch strategy(%s): order=%s connect_role=%s warmup_rounds=%" PRIu32 " warmup_interval_ms=%" PRIu32 "\n",
           from, punch_order_text(signal->punch_order), connect_role_text(signal->connect_role),
           signal->warmup_rounds, signal->warmup_interval_ms);

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
        printf("Warmup packet received early (%s), continue immediately\n", from);
    } else {
        printf("Warmup window elapsed (%s): waited=%" PRIu32 "ms\n", from, wait_ms);
    }
}

static bool run_probe_session(event_base* base, ntrs_async_client_t* async_client, const std::string& node_host,
                              uint16_t node_port, const std::string& peer_id, const std::string& bind_ip,
                              const std::string& bind_device, const std::string& explicit_probe1,
                              const std::string& explicit_probe2, bool verbose, ProbeSession* out)
{
    int                      control_fd = -1;
    int                      udp_sock = -1;
    char                     session_token[NTRS_MAX_TEXT_LEN];
    uint32_t                 lease_default_sec = 0;
    char                     probe1[NTRS_MAX_TEXT_LEN];
    char                     probe2[NTRS_MAX_TEXT_LEN];
    std::string              probe1_host;
    std::string              probe2_host;
    uint16_t                 probe1_port = 0;
    uint16_t                 probe2_port = 0;
    ntrs_detect_nat_options_t detect_options;
    AsyncResultWait          wait_detect;
    uint64_t                 request_id = 0;

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

    control_fd = ntrs_connect_control(node_host.c_str(), node_port);
    if (control_fd < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "connect control failed");
        return false;
    }
    if (!ntrs_auth(control_fd, peer_id.c_str(), "ntrs-dev-secret", session_token, sizeof(session_token),
                   &lease_default_sec)) {
        snprintf(out->error_message, sizeof(out->error_message), "auth failed");
        close(control_fd);
        return false;
    }

    if (!explicit_probe1.empty()) {
        snprintf(probe1, sizeof(probe1), "%s", explicit_probe1.c_str());
        snprintf(probe2, sizeof(probe2), "%s", explicit_probe2.c_str());
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

    udp_sock = create_udp_probe_socket(bind_ip, bind_device);
    if (udp_sock < 0) {
        snprintf(out->error_message, sizeof(out->error_message), "create probe socket failed");
        close(control_fd);
        return false;
    }

    ntrs_detect_nat_options_init(&detect_options);
    detect_options.enable_filter_probe = true;
    detect_options.verbose = verbose;

    memset(&wait_detect, 0, sizeof(wait_detect));
    wait_detect.base = base;
    if (!ntrs_async_detect_nat(async_client, &request_id, udp_sock, probe1_host.c_str(), probe1_port,
                               probe2[0] == '\0' ? NULL : probe2_host.c_str(), probe2_port, control_fd, session_token,
                               &detect_options, async_result_callback, &wait_detect) ||
        !wait_async_result(base, &wait_detect, 8) || !wait_detect.result.success) {
        snprintf(out->error_message, sizeof(out->error_message), "%s",
                 wait_detect.result.error_message[0] != '\0' ? wait_detect.result.error_message
                                                             : "async NAT detect failed");
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
    out->nat_info = wait_detect.result.nat_info;
    return true;
}

static bool peer_candidate_to_sockaddr(const ntrs_peer_candidate_t* candidate, struct sockaddr_storage* out,
                                      socklen_t* out_len)
{
    struct sockaddr_in* addr4 = NULL;

    if (candidate == NULL || out == NULL || out_len == NULL || candidate->ip[0] == '\0' || candidate->port == 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    addr4 = (struct sockaddr_in*)out;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(candidate->port);
    if (inet_pton(AF_INET, candidate->ip, &addr4->sin_addr) != 1) {
        return false;
    }
    *out_len = sizeof(*addr4);
    return true;
}

static uint64_t steady_now_us()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static std::string make_probe_token()
{
    static std::mt19937_64 rng(
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ (uint64_t)getpid());
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016" PRIx64, (uint64_t)rng());
    return std::string(buffer);
}

static bool sockaddr_matches_candidate(const struct sockaddr_storage* src, socklen_t src_len,
                                       const ntrs_peer_candidate_t* candidate)
{
    struct sockaddr_storage expected;
    socklen_t               expected_len = 0;
    char                    host[NI_MAXHOST];
    char                    service[NI_MAXSERV];

    if (src == NULL || candidate == NULL ||
        !peer_candidate_to_sockaddr(candidate, &expected, &expected_len)) {
        return false;
    }
    if (src->ss_family == expected.ss_family && src_len == expected_len &&
        memcmp(src, &expected, expected_len) == 0) {
        return true;
    }
    if (getnameinfo((const struct sockaddr*)src, src_len, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }
    return strcmp(host, candidate->ip) == 0 && (uint16_t)atoi(service) == candidate->port;
}

static std::string build_probe_packet(const char* prefix, const char* owner_peer, const char* token, uint32_t seq,
                                      uint64_t value, size_t payload_size)
{
    char        header[256];
    std::string packet;
    int         header_len = snprintf(header, sizeof(header), "%s|%s|%s|%" PRIu32 "|%" PRIu64 "|", prefix,
                              owner_peer == NULL ? "" : owner_peer, token == NULL ? "" : token, seq, value);

    if (header_len <= 0) {
        return packet;
    }
    packet.assign(header, (size_t)header_len);
    if (packet.size() < payload_size) {
        packet.resize(payload_size, 'X');
    }
    return packet;
}

static bool recv_probe_packet(int sock, char* buffer, size_t buffer_size, size_t* out_len,
                              struct sockaddr_storage* src, socklen_t* src_len, int timeout_ms)
{
    fd_set         rfds;
    struct timeval tv;
    ssize_t        nread = 0;

    if (buffer == NULL || buffer_size < 2 || out_len == NULL || src == NULL || src_len == NULL) {
        return false;
    }

    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(sock + 1, &rfds, NULL, NULL, &tv) <= 0 || !FD_ISSET(sock, &rfds)) {
        return false;
    }

    *src_len = sizeof(*src);
    nread = recvfrom(sock, buffer, buffer_size - 1, 0, (struct sockaddr*)src, src_len);
    if (nread <= 0) {
        return false;
    }
    buffer[nread] = '\0';
    *out_len = (size_t)nread;
    return true;
}

struct ProbePacketHeader {
    char     type[32];
    char     owner_peer[NTRS_MAX_TEXT_LEN];
    char     token[32];
    uint32_t seq;
    uint64_t value;
};

static bool parse_punch_frame(const uint8_t* payload, size_t payload_len, PunchFrameMeta* out)
{
    ntrs_binary_frame_view view;
    ntrs_binary_tlv_view   tlv;
    size_t                 cursor = 0;

    if (payload == NULL || out == NULL || !ntrs_binary_frame_parse(payload, payload_len, &view)) {
        return false;
    }
    if (view.header.phase != NTRS_BINARY_PHASE_PUNCH) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->frame_type = (ntrs_binary_frame_type_t)view.header.frame_type;
    out->request_id = view.header.request_id;
    out->sequence = view.header.sequence;

    while (ntrs_binary_frame_next_tlv(&view, &cursor, &tlv)) {
        if (tlv.type == NTRS_BINARY_TLV_PROBE_TOKEN) {
            if (tlv.value_len > sizeof(out->token)) {
                return false;
            }
            memcpy(out->token, tlv.value, tlv.value_len);
            out->token_len = (uint8_t)tlv.value_len;
        } else if (tlv.type == NTRS_BINARY_TLV_CANDIDATE_TYPE) {
            size_t copy_len = tlv.value_len;
            if (copy_len >= sizeof(out->candidate_type)) {
                copy_len = sizeof(out->candidate_type) - 1u;
            }
            memcpy(out->candidate_type, tlv.value, copy_len);
            out->candidate_type[copy_len] = '\0';
        }
    }

    return out->frame_type == NTRS_BINARY_FRAME_PUNCH_REQ || out->frame_type == NTRS_BINARY_FRAME_PUNCH_ACK;
}

static bool build_punch_ack_frame(const PunchFrameMeta* req_meta, uint8_t* out, size_t out_cap, size_t* out_len)
{
    ntrs_binary_frame_t frame;

    if (req_meta == NULL || out == NULL || out_len == NULL || !ntrs_binary_frame_init(&frame, out, out_cap)) {
        return false;
    }
    if (!ntrs_binary_frame_set_header(&frame, NTRS_BINARY_FRAME_PUNCH_ACK, NTRS_BINARY_PHASE_PUNCH, 0u,
                                      req_meta->request_id, req_meta->sequence, current_time_ms())) {
        return false;
    }
    if (req_meta->token_len > 0 &&
        !ntrs_binary_frame_add_tlv(&frame, NTRS_BINARY_TLV_PROBE_TOKEN, req_meta->token, req_meta->token_len)) {
        return false;
    }
    if (req_meta->candidate_type[0] != '\0' &&
        !ntrs_binary_frame_add_tlv(&frame, NTRS_BINARY_TLV_CANDIDATE_TYPE, req_meta->candidate_type,
                                   (uint16_t)strlen(req_meta->candidate_type))) {
        return false;
    }

    *out_len = frame.length;
    return true;
}

static bool parse_probe_packet_header(const char* buffer, ProbePacketHeader* out)
{
    char type[32];
    char owner_peer[NTRS_MAX_TEXT_LEN];
    char token[32];
    unsigned long long seq = 0;
    unsigned long long value = 0;

    if (buffer == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (sscanf(buffer, "%31[^|]|%127[^|]|%31[^|]|%llu|%llu|", type, owner_peer, token, &seq, &value) != 5) {
        return false;
    }
    snprintf(out->type, sizeof(out->type), "%s", type);
    snprintf(out->owner_peer, sizeof(out->owner_peer), "%s", owner_peer);
    snprintf(out->token, sizeof(out->token), "%s", token);
    out->seq = (uint32_t)seq;
    out->value = (uint64_t)value;
    return true;
}

static bool handle_probe_request(int sock, const char* buffer, size_t buffer_len, const struct sockaddr_storage* src,
                                 socklen_t src_len)
{
    ProbePacketHeader header;
    std::string       reply;
    PunchFrameMeta    punch_meta;

    if (buffer == NULL || src == NULL) {
        return false;
    }
    if (parse_punch_frame((const uint8_t*)buffer, buffer_len, &punch_meta) &&
        punch_meta.frame_type == NTRS_BINARY_FRAME_PUNCH_REQ) {
        uint8_t ack[256];
        size_t  ack_len = 0;
        if (build_punch_ack_frame(&punch_meta, ack, sizeof(ack), &ack_len)) {
            sendto(sock, ack, ack_len, 0, (const struct sockaddr*)src, src_len);
        }
        return true;
    }
    if (parse_punch_frame((const uint8_t*)buffer, buffer_len, &punch_meta) &&
        punch_meta.frame_type == NTRS_BINARY_FRAME_PUNCH_ACK) {
        return true;
    }
    if (!parse_probe_packet_header(buffer, &header)) {
        return false;
    }
    if (strcmp(header.type, "NTRS_PROBE_PING") == 0) {
        reply = build_probe_packet("NTRS_PROBE_PONG", header.owner_peer, header.token, header.seq, header.value, 96);
        sendto(sock, reply.data(), reply.size(), 0, (const struct sockaddr*)src, src_len);
        return true;
    }
    if (strcmp(header.type, "NTRS_MTU_PROBE") == 0 && (uint64_t)strlen(buffer) <= header.value) {
        reply = build_probe_packet("NTRS_MTU_ACK", header.owner_peer, header.token, header.seq, header.value, 96);
        sendto(sock, reply.data(), reply.size(), 0, (const struct sockaddr*)src, src_len);
        return true;
    }
    return false;
}

static void run_probe_responder_tail(int sock, int timeout_ms)
{
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        char                    buffer[2048];
        size_t                  buffer_len = 0;
        struct sockaddr_storage src;
        socklen_t               src_len = sizeof(src);
        if (!recv_probe_packet(sock, buffer, sizeof(buffer), &buffer_len, &src, &src_len, 100)) {
            continue;
        }
        handle_probe_request(sock, buffer, buffer_len, &src, src_len);
    }
}

static void run_latency_loss_probe(int sock, const ntrs_peer_candidate_t* candidate, const char* local_peer_id,
                                   const char* token, const char* from)
{
    const int ping_count = 10;
    uint32_t  received = 0;
    uint64_t  rtt_min_us = 0;
    uint64_t  rtt_max_us = 0;
    uint64_t  rtt_sum_us = 0;
    struct sockaddr_storage dst;
    socklen_t               dst_len = 0;

    if (candidate == NULL || !peer_candidate_to_sockaddr(candidate, &dst, &dst_len)) {
        printf("UDP stats (%s): invalid candidate\n", from);
        return;
    }

    for (int i = 0; i < ping_count; ++i) {
        uint32_t seq = (uint32_t)(i + 1);
        uint64_t sent_us = steady_now_us();
        std::string packet = build_probe_packet("NTRS_PROBE_PING", local_peer_id, token, seq, sent_us, 128);
        sendto(sock, packet.data(), packet.size(), 0, (struct sockaddr*)&dst, dst_len);

        for (;;) {
            char                    buffer[2048];
            size_t                  buffer_len = 0;
            struct sockaddr_storage src;
            socklen_t               src_len = sizeof(src);
            ProbePacketHeader       header;

            if (!recv_probe_packet(sock, buffer, sizeof(buffer), &buffer_len, &src, &src_len, 1000)) {
                break;
            }
            if (handle_probe_request(sock, buffer, buffer_len, &src, src_len)) {
                continue;
            }
            if (!sockaddr_matches_candidate(&src, src_len, candidate)) {
                continue;
            }
            if (!parse_probe_packet_header(buffer, &header)) {
                continue;
            }
            if (strcmp(header.type, "NTRS_PROBE_PONG") == 0 && strcmp(header.owner_peer, local_peer_id) == 0 &&
                strcmp(header.token, token) == 0 && header.seq == seq && header.value == sent_us) {
                uint64_t rtt_us = steady_now_us() - sent_us;
                if (received == 0 || rtt_us < rtt_min_us) {
                    rtt_min_us = rtt_us;
                }
                if (received == 0 || rtt_us > rtt_max_us) {
                    rtt_max_us = rtt_us;
                }
                rtt_sum_us += rtt_us;
                received++;
                break;
            }
        }
    }

    uint32_t lost = ping_count - received;
    double   avg_ms = received == 0 ? 0.0 : (double)rtt_sum_us / (double)received / 1000.0;
    double   min_ms = received == 0 ? 0.0 : (double)rtt_min_us / 1000.0;
    double   max_ms = received == 0 ? 0.0 : (double)rtt_max_us / 1000.0;
    double   loss_pct = ping_count == 0 ? 0.0 : (double)lost * 100.0 / (double)ping_count;

    printf("UDP stats (%s): sent=%d recv=%" PRIu32 " lost=%" PRIu32 " loss=%.1f%% rtt_ms(min/avg/max)=%.3f/%.3f/%.3f\n",
           from, ping_count, received, lost, loss_pct, min_ms, avg_ms, max_ms);
}

static bool set_mtu_probe_df_mode(int sock, int family, int* previous_value, bool* changed)
{
    if (previous_value == NULL || changed == NULL) {
        return false;
    }
    *previous_value = 0;
    *changed = false;

    if (family == AF_INET) {
#ifdef IP_MTU_DISCOVER
#ifdef IP_PMTUDISC_DO
        socklen_t optlen = sizeof(*previous_value);
        if (getsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, previous_value, &optlen) == 0) {
            int value = IP_PMTUDISC_DO;
            if (setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, &value, sizeof(value)) == 0) {
                *changed = true;
                return true;
            }
        }
#endif
#endif
        return false;
    }

    if (family == AF_INET6) {
#if defined(IPV6_DONTFRAG)
        socklen_t optlen = sizeof(*previous_value);
        if (getsockopt(sock, IPPROTO_IPV6, IPV6_DONTFRAG, previous_value, &optlen) == 0) {
            int value = 1;
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_DONTFRAG, &value, sizeof(value)) == 0) {
                *changed = true;
                return true;
            }
        }
#endif
        return false;
    }

    return false;
}

static void restore_mtu_probe_df_mode(int sock, int family, int previous_value, bool changed)
{
    if (!changed) {
        return;
    }
    if (family == AF_INET) {
#ifdef IP_MTU_DISCOVER
        setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, &previous_value, sizeof(previous_value));
#endif
        return;
    }
    if (family == AF_INET6) {
#if defined(IPV6_DONTFRAG)
        setsockopt(sock, IPPROTO_IPV6, IPV6_DONTFRAG, &previous_value, sizeof(previous_value));
#endif
    }
}

static bool send_mtu_probe_once(int sock, const ntrs_peer_candidate_t* candidate, const char* local_peer_id,
                                const char* token, uint32_t seq, size_t payload_size)
{
    struct sockaddr_storage dst;
    socklen_t               dst_len = 0;
    std::string             packet;

    if (candidate == NULL || !peer_candidate_to_sockaddr(candidate, &dst, &dst_len)) {
        return false;
    }
    packet = build_probe_packet("NTRS_MTU_PROBE", local_peer_id, token, seq, (uint64_t)payload_size, payload_size);
    if (packet.size() != payload_size) {
        return false;
    }
    if (sendto(sock, packet.data(), packet.size(), 0, (struct sockaddr*)&dst, dst_len) < 0) {
        if (errno == EMSGSIZE) {
            return false;
        }
    }

    for (;;) {
        char                    buffer[2048];
        size_t                  buffer_len = 0;
        struct sockaddr_storage src;
        socklen_t               src_len = sizeof(src);
        ProbePacketHeader       header;

        if (!recv_probe_packet(sock, buffer, sizeof(buffer), &buffer_len, &src, &src_len, 1000)) {
            return false;
        }
        if (handle_probe_request(sock, buffer, buffer_len, &src, src_len)) {
            continue;
        }
        if (!sockaddr_matches_candidate(&src, src_len, candidate)) {
            continue;
        }
        if (!parse_probe_packet_header(buffer, &header)) {
            continue;
        }
        if (strcmp(header.type, "NTRS_MTU_ACK") == 0 && strcmp(header.owner_peer, local_peer_id) == 0 &&
            strcmp(header.token, token) == 0 && header.seq == seq && header.value == payload_size) {
            return true;
        }
    }
}

static bool confirm_mtu_payload(int sock, const ntrs_peer_candidate_t* candidate, const char* local_peer_id,
                                const char* token, uint32_t* seq, size_t payload_size)
{
    const int attempts = 3;
    uint32_t  success_count = 0;

    if (seq == NULL) {
        return false;
    }

    for (int i = 0; i < attempts; ++i) {
        if (send_mtu_probe_once(sock, candidate, local_peer_id, token, (*seq)++, payload_size)) {
            success_count++;
            if (success_count >= 2) {
                return true;
            }
        }
        if ((attempts - (i + 1)) + (int)success_count < 2) {
            return false;
        }
    }

    return false;
}

static void run_mtu_probe(int sock, const ntrs_peer_candidate_t* candidate, const char* local_peer_id,
                          const char* token, const char* from)
{
    size_t   low = 1280;
    size_t   high = 1500;
    size_t   best = 0;
    uint32_t seq = 1000;
    int      previous_value = 0;
    bool     changed = false;
    int      family = AF_UNSPEC;

    if (candidate == NULL) {
        printf("UDP mtu (%s): invalid candidate\n", from);
        return;
    }
    {
        struct in_addr  addr4;
        struct in6_addr addr6;
        memset(&addr4, 0, sizeof(addr4));
        memset(&addr6, 0, sizeof(addr6));
        if (inet_pton(AF_INET, candidate->ip, &addr4) == 1) {
            family = AF_INET;
        } else if (inet_pton(AF_INET6, candidate->ip, &addr6) == 1) {
            family = AF_INET6;
        }
    }
    set_mtu_probe_df_mode(sock, family, &previous_value, &changed);

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        bool   ok = confirm_mtu_payload(sock, candidate, local_peer_id, token, &seq, mid);
        if (ok) {
            best = mid;
            low = mid + 1;
        } else {
            if (mid == 0) {
                break;
            }
            high = mid - 1;
        }
    }

    restore_mtu_probe_df_mode(sock, family, previous_value, changed);

    if (best == 0) {
        printf("UDP mtu (%s): no payload accepted in range [1280,1500]\n", from);
    } else {
        printf("UDP mtu (%s): payload_max=%zu range=[1280,1500]\n", from, best);
    }
}
static int compute_punch_interval_ms(const ntrs_session_signal_t* signal)
{
    if (signal != NULL && signal->warmup_interval_ms > 0) {
        return (int)signal->warmup_interval_ms;
    }
    return 100;
}

static int compute_punch_rounds(const ntrs_session_signal_t*)
{
    return 3;
}

static int compute_punch_wait_timeout_sec(const ntrs_session_signal_t* signal)
{
    int rounds = compute_punch_rounds(signal);
    int interval_ms = compute_punch_interval_ms(signal);
    int timeout_ms = rounds * interval_ms + 1000;
    int timeout_sec = (timeout_ms + 999) / 1000;
    return timeout_sec < 3 ? 3 : timeout_sec;
}

static void try_hole_punch_from_async_signal(ntrs_async_client_t* client, event_base* base, int sock,
                                             const ntrs_session_signal_t* signal, const char* local_peer_id,
                                             const char* from, bool verbose)
{
    AsyncResultWait wait_punch;
    uint64_t        request_id = 0;
    std::string     probe_token;

    if (signal == NULL) {
        return;
    }

    printf("Session signal(%s): peer=%s device=%s class=%u flags=0x%04x mapping=%u filtering=%u nat=%s candidates=%u\n",
           from, signal->peer_id, signal->peer_device_id[0] == '\0' ? "-" : signal->peer_device_id,
           signal->peer_nat_class, signal->peer_nat_flags, signal->peer_mapping_behavior,
           signal->peer_filtering_behavior, signal->peer_nat_type, signal->candidate_count);
    if (verbose) {
        for (uint32_t i = 0; i < signal->candidate_count; ++i) {
            printf("  candidate[%u]: type=%s endpoint=%s:%u\n", i,
                   signal->candidates[i].type[0] == '\0' ? "candidate" : signal->candidates[i].type,
                   signal->candidates[i].ip, signal->candidates[i].port);
        }
    }

    apply_signal_strategy_wait(sock, signal, from);

    memset(&wait_punch, 0, sizeof(wait_punch));
    wait_punch.base = base;
    int punch_rounds = compute_punch_rounds(signal);
    int punch_interval_ms = compute_punch_interval_ms(signal);
    int punch_wait_timeout_sec = compute_punch_wait_timeout_sec(signal);
    if (ntrs_async_try_udp_hole_punch(client, &request_id, sock, signal->candidates, signal->candidate_count,
                                      punch_rounds, punch_interval_ms, async_result_callback, &wait_punch) &&
        wait_async_result(base, &wait_punch, punch_wait_timeout_sec) && wait_punch.result.success) {
        printf("UDP hole punch result (%s): ok selected=%s:%u type=%s\n", from,
               wait_punch.result.selected_candidate.ip, wait_punch.result.selected_candidate.port,
               wait_punch.result.selected_candidate.type[0] == '\0' ? "candidate"
                                                                    : wait_punch.result.selected_candidate.type);
        probe_token = make_probe_token();
        run_latency_loss_probe(sock, &wait_punch.result.selected_candidate, local_peer_id, probe_token.c_str(), from);
        run_mtu_probe(sock, &wait_punch.result.selected_candidate, local_peer_id, probe_token.c_str(), from);
        run_probe_responder_tail(sock, 1000);
    } else {
        printf("UDP hole punch result (%s): failed\n", from);
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
            "[peer_id device_id [dst_peer|-] [dst_device|-] [probe1_host:port] [probe2_host:port]]\n",
            argv[0]);
        return 1;
    }

    std::string ntrs_ip = positional[0];
    int         ntrs_port = atoi(positional[1].c_str());
    bool        probe_only = positional.size() < 4;
    std::string peer_id = (positional.size() > 2) ? positional[2] : "probe_peer";
    std::string device_id = (positional.size() > 3) ? positional[3] : "probe_device";
    std::string dst_peer = (positional.size() > 4) ? positional[4] : "";
    std::string dst_device = (positional.size() > 5) ? positional[5] : "";
    std::string probe1 = (positional.size() > 6) ? positional[6] : "";
    std::string probe2 = (positional.size() > 7) ? positional[7] : "";
    if (dst_peer == "-") {
        dst_peer.clear();
    }
    if (dst_device == "-") {
        dst_device.clear();
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

    uint64_t              request_id = 0;
    ProbeSession          probe;
    int                   fd = -1;
    int                   probe_sock = -1;
    std::string           control_session_token;
    const ntrs_nat_info_t* nat = NULL;

    if (!run_probe_session(base, async_client, ntrs_ip, (uint16_t)ntrs_port, peer_id, bind_ip, bind_device, probe1,
                           probe2, verbose, &probe)) {
        printf("async NAT detect failed: %s\n", probe.error_message);
        ntrs_async_client_destroy(async_client);
        event_base_free(base);
        return 1;
    }

    fd = probe.control_fd;
    probe_sock = probe.udp_sock;
    control_session_token = probe.session_token;
    probe1 = probe.probe1;
    probe2 = probe.probe2;
    printf("NTRS probe endpoints: probe1=%s probe2=%s\n", probe1.c_str(), probe2.empty() ? "-" : probe2.c_str());
    nat = &probe.nat_info;
    printf("NAT summary: local=%s:%u srflx=%s:%u srflx2=%s:%u class=%u flags=0x%04x mapping=%u filtering=%u type=%s "
           "risk=%s\n",
           nat->local_ip, nat->local_port, nat->srflx_ip, nat->srflx_port, nat->srflx_ip_2, nat->srflx_port_2,
           nat->nat_class, nat->nat_flags, nat->mapping_behavior, nat->filtering_behavior, nat->nat_type,
           nat->nat_risk);

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
        if (ntrs_async_create_session(async_client, &request_id, fd, peer_id.c_str(), device_id.c_str(),
                                      dst_peer.c_str(), dst_device.c_str(), control_session_token.c_str(),
                                      async_result_callback, &wait_session) &&
            wait_async_result(base, &wait_session, 5) && wait_session.result.success) {
            try_hole_punch_from_async_signal(async_client, base, probe_sock, &wait_session.result.session_signal,
                                             peer_id.c_str(), "session_create_rsp", verbose);
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
                                             peer_id.c_str(), "session_notify", verbose);
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
