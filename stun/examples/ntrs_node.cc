#include <errno.h>
#include <fcntl.h>
#include <mqtt_client.h>
#include <netdb.h>
#include <ntrs_auth.h>
#include <ntrs_client.h>
#include <ntrs_codec.h>
#include <ntrs_io.h>
#include <socket_address.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stun.h>
#include <stun_types.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <ctime>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <event/loop.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "../3rd_party/include/stun/msg.h"

static void node_verbose_log(bool verbose, const char* fmt, ...)
{
    va_list args;

    if (!verbose || fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

struct PeerSession {
    std::string               peer_id;
    std::string               device_id;
    std::string               local_ip;
    uint16_t                  local_port;
    std::string               srflx_ip;
    uint16_t                  srflx_port;
    std::string               srflx_ip_2;
    uint16_t                  srflx_port_2;
    ntrs_nat_class_t          nat_class;
    ntrs_nat_flags_t          nat_flags;
    ntrs_mapping_behavior_t   mapping_behavior;
    ntrs_filtering_behavior_t filtering_behavior;
    std::string               nat_type;
    int                       fd;
    time_t                    expire_at;
};

struct SessionStrategyPlan {
    uint8_t  src_punch_order;
    uint8_t  dst_punch_order;
    uint8_t  src_connect_role;
    uint8_t  dst_connect_role;
    uint32_t src_warmup_rounds;
    uint32_t dst_warmup_rounds;
    uint32_t src_warmup_interval_ms;
    uint32_t dst_warmup_interval_ms;
};

static uint32_t nat_strictness_score(ntrs_nat_class_t nat_class)
{
    switch (nat_class) {
    case NTRS_NAT_CLASS_OPEN_PUBLIC:
        return 1;
    case NTRS_NAT_CLASS_FULL_CONE:
        return 2;
    case NTRS_NAT_CLASS_IP_RESTRICTED:
        return 3;
    case NTRS_NAT_CLASS_PORT_RESTRICTED:
        return 4;
    case NTRS_NAT_CLASS_SYMMETRIC:
        return 5;
    default:
        return 0;
    }
}

static SessionStrategyPlan build_session_strategy(const PeerSession& src, const PeerSession& dst)
{
    SessionStrategyPlan plan;
    uint32_t            src_score = nat_strictness_score(src.nat_class);
    uint32_t            dst_score = nat_strictness_score(dst.nat_class);

    memset(&plan, 0, sizeof(plan));
    plan.src_punch_order = (uint8_t)eular::ntrs::PunchOrderCode::SIMULTANEOUS;
    plan.dst_punch_order = (uint8_t)eular::ntrs::PunchOrderCode::SIMULTANEOUS;
    plan.src_connect_role = (uint8_t)eular::ntrs::RoleCode::INITIATOR;
    plan.dst_connect_role = (uint8_t)eular::ntrs::RoleCode::RESPONDER;
    plan.src_warmup_rounds = 4;
    plan.dst_warmup_rounds = 4;
    plan.src_warmup_interval_ms = 100;
    plan.dst_warmup_interval_ms = 100;

    if (src_score == dst_score) {
        return plan;
    }

    plan.dst_warmup_rounds = 4;
    plan.src_warmup_interval_ms = 100;
    plan.dst_warmup_interval_ms = 100;

    if (src_score < dst_score) {
        plan.src_punch_order = (uint8_t)eular::ntrs::PunchOrderCode::SEND_FIRST;
        plan.dst_punch_order = (uint8_t)eular::ntrs::PunchOrderCode::WAIT_FIRST;
        plan.src_connect_role = (uint8_t)eular::ntrs::RoleCode::RESPONDER;
        plan.dst_connect_role = (uint8_t)eular::ntrs::RoleCode::INITIATOR;
        return plan;
    }

    plan.src_punch_order = (uint8_t)eular::ntrs::PunchOrderCode::WAIT_FIRST;
    plan.dst_punch_order = (uint8_t)eular::ntrs::PunchOrderCode::SEND_FIRST;
    plan.src_connect_role = (uint8_t)eular::ntrs::RoleCode::INITIATOR;
    plan.dst_connect_role = (uint8_t)eular::ntrs::RoleCode::RESPONDER;
    return plan;
}

struct ControlClientRxState {
    std::vector<uint8_t> buffer;
};

enum class AsyncFederationJobType {
    FETCH_STUN,
    SEND_PROBE,
    SEND_STUN,
};

struct AsyncFederationJob {
    AsyncFederationJobType   type;
    int                      fd;
    uint64_t                 client_generation;
    uint32_t                 request_id;
    std::vector<std::string> controls;
    std::string              auth_secret;
    std::string              federation_peer_id;
    std::string              target_ip;
    uint16_t                 target_port;
    std::string              probe_token;
    std::string              owner_peer_id;
    uint64_t                 probe_expire_at;
    std::string              probe_auth;
    bool                     same_ip_diff_port;
    std::vector<uint8_t>     stun_txid;
    bool                     use_alt_port;
};

struct AsyncFederationResult {
    AsyncFederationJobType type;
    int                    fd;
    uint64_t               client_generation;
    uint32_t               request_id;
    bool                   ok;
    std::string            selected_control;
    std::string            peer_stun;
    bool                   same_ip_diff_port;
    bool                   diff_ip;
};

enum class FederationState {
    RESOLVING,
    CONNECTING,
    SENDING_AUTH,
    READING_AUTH,
    SENDING_REQUEST,
    READING_RESPONSE,
};

struct FederationRequest {
    AsyncFederationJob                job;
    AsyncFederationResult             result;
    FederationState                   state;
    int                               fd;
    size_t                            control_index;
    struct evutil_addrinfo*           resolved_addrs;
    struct evutil_addrinfo*           next_addr;
    struct evdns_getaddrinfo_request* dns_request;
    std::vector<uint8_t>              tx_buffer;
    size_t                            tx_offset;
    ControlClientRxState              rx_state;
    std::string                       session_token;
    time_t                            deadline;

    FederationRequest()
        : state(FederationState::CONNECTING),
          fd(-1),
          control_index(0),
          resolved_addrs(NULL),
          next_addr(NULL),
          dns_request(NULL),
          tx_offset(0),
          deadline(0)
    {
    }
};

static const int             kLeaseSec = 30;
static volatile sig_atomic_t g_stop = 0;

static void on_process_signal(int) { g_stop = 1; }

static bool parse_endpoint(const std::string& input, std::string* host, uint16_t* port)
{
    size_t      pos = std::string::npos;
    std::string host_part;
    std::string port_part;

    if (host == NULL || port == NULL || input.empty()) {
        return false;
    }

    if (input[0] == '[') {
        size_t end = input.find(']');
        if (end == std::string::npos || end + 1 >= input.size() || input[end + 1] != ':') {
            return false;
        }
        host_part = input.substr(1, end - 1);
        port_part = input.substr(end + 2);
    } else {
        pos = input.rfind(':');
        if (pos == std::string::npos || pos == 0 || pos == input.size() - 1) {
            return false;
        }
        host_part = input.substr(0, pos);
        port_part = input.substr(pos + 1);
        if (host_part.find(':') != std::string::npos) {
            return false;
        }
    }

    int parsed = atoi(port_part.c_str());
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }

    *host = host_part;
    *port = (uint16_t)parsed;
    return !host->empty();
}

static uint16_t endpoint_port(const std::string& endpoint)
{
    std::string host;
    uint16_t    port = 0;
    if (!parse_endpoint(endpoint, &host, &port)) {
        return 0;
    }
    return port;
}

static std::string format_endpoint(const std::string& host, uint16_t port)
{
    if (host.find(':') != std::string::npos && (host.empty() || host[0] != '[')) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

static void print_usage(const char* program)
{
    printf(
        "Usage:\n"
        "  %s --hub <hub_host:port> --node-id <node_id> --public-host <public_ip_or_name>\n"
        "     [--control-port 19000] [--stun-port 3478] [--stun-alt-port 3479] [--region default]\n"
        "     [--mqtt-username user] [--mqtt-password pass] [--auth-secret secret] [--verbose]\n"
        "\n"
        "Legacy:\n"
        "  %s <control_port> <self_stun_host:port> [peer_node_control_host:port]\n"
        "     [mqtt_broker] [mqtt_port] [node_id] [region] [mqtt_username] [mqtt_password] [auth_secret]\n",
        program, program);
}

static std::string now_iso8601()
{
    time_t    t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

static std::string topic_for(const std::string& node_id, const std::string& suffix)
{
    return std::string("ntrs/node/") + node_id + "/" + suffix;
}

static const char* msg_str_tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag)
{
    const char* value = eular::ntrs::messageGetStringByTag(&msg, tag);
    return value == NULL ? "" : value;
}

static uint16_t msg_u16_tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, uint16_t default_value = 0)
{
    uint16_t value = default_value;
    if (eular::ntrs::messageGetU16ByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static uint32_t msg_u32_tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, uint32_t default_value = 0)
{
    uint32_t value = default_value;
    if (eular::ntrs::messageGetU32ByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static bool msg_bool_tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, bool default_value = false)
{
    bool value = default_value;
    if (eular::ntrs::messageGetBoolByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static int32_t msg_i32_tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, int32_t default_value = 0)
{
    int32_t value = default_value;
    if (eular::ntrs::messageGetI32ByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static bool mqtt_publish_message(eular::orion::MqttClient* mqtt, const std::string& topic,
                                 const eular::ntrs::Message& msg, bool retain)
{
    uint8_t buf[8192];
    size_t  len = 0;
    if (mqtt == NULL || eular::ntrs::encodeMessage(msg, buf, sizeof(buf), &len) != 0) {
        printf("mqtt publish encode failed topic=%s retain=%s\n", topic.c_str(), retain ? "true" : "false");
        return false;
    }
    if (!mqtt->publish(topic, buf, len, 1, retain)) {
        printf("mqtt publish failed topic=%s len=%zu retain=%s\n", topic.c_str(), len, retain ? "true" : "false");
        return false;
    }
    return true;
}

static bool resolve_ipv4_literal(const std::string& host, std::string* ip_out)
{
    struct addrinfo  hints;
    struct addrinfo* result = NULL;

    if (ip_out == NULL || host.empty()) {
        return false;
    }

    struct in_addr addr4;
    if (inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
        *ip_out = host;
        return true;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), NULL, &hints, &result) != 0 || result == NULL) {
        return false;
    }

    char                ip_buffer[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ip_buffer, sizeof(ip_buffer));
    *ip_out = ip_buffer;
    freeaddrinfo(result);
    return !ip_out->empty();
}

static bool resolve_probe_other_address(const std::vector<std::string>& probe_controls, uint16_t stun_port,
                                        std::string* other_ip, uint16_t* other_port)
{
    size_t i = 0;

    if (other_ip == NULL || other_port == NULL) {
        return false;
    }

    for (i = 0; i < probe_controls.size(); ++i) {
        std::string host;
        uint16_t    port = 0;
        std::string resolved_ip;

        if (!parse_endpoint(probe_controls[i], &host, &port)) {
            continue;
        }
        if (!resolve_ipv4_literal(host, &resolved_ip)) {
            resolved_ip = host;
        }
        if (!resolved_ip.empty()) {
            *other_ip = resolved_ip;
            *other_port = stun_port;
            return true;
        }
    }

    return false;
}

static void publish_presence(eular::orion::MqttClient* mqtt, const std::string& node_id, const std::string& boot_id,
                             const std::string& status, const std::string& reason, uint64_t request_id)
{
    eular::ntrs::Message msg;
    eular::ntrs::NodeStatusCode status_code = eular::ntrs::NodeStatusCode::UNKNOWN;
    eular::ntrs::ReasonCode     reason_code = eular::ntrs::ReasonCode::NONE;
    if (status == "online") {
        status_code = eular::ntrs::NodeStatusCode::ONLINE;
    } else if (status == "offline") {
        status_code = eular::ntrs::NodeStatusCode::OFFLINE;
    } else if (status == "registered") {
        status_code = eular::ntrs::NodeStatusCode::REGISTERED;
    }
    if (reason == "lwt") {
        reason_code = eular::ntrs::ReasonCode::LWT;
    } else if (reason == "startup") {
        reason_code = eular::ntrs::ReasonCode::STARTUP;
    } else if (reason == "client_exit") {
        reason_code = eular::ntrs::ReasonCode::CLIENT_EXIT;
    }
    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::NODE_PRESENCE, (uint32_t)request_id);
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
    eular::ntrs::messageAddU8ByTag(&msg, eular::ntrs::FieldTag::STATUS, (uint8_t)status_code);
    eular::ntrs::messageAddU8ByTag(&msg, eular::ntrs::FieldTag::REASON, (uint8_t)reason_code);
    const std::string ts = now_iso8601();
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    mqtt_publish_message(mqtt, topic_for(node_id, "presence"), msg, true);
}

static void append_unique(std::vector<std::string>* out, const std::string& value)
{
    if (value.empty()) {
        return;
    }
    for (size_t i = 0; i < out->size(); ++i) {
        if (out->at(i) == value) {
            return;
        }
    }
    out->push_back(value);
}

static std::vector<std::string> build_probe_controls(const std::string& p1, const std::string& p2,
                                                     const std::string& b1, const std::string& fallback)
{
    std::vector<std::string> out;
    append_unique(&out, p1);
    append_unique(&out, p2);
    append_unique(&out, b1);
    append_unique(&out, fallback);
    return out;
}

static void erase_peer_for_fd(std::map<std::string, PeerSession>* peers, int fd)
{
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end();) {
        if (it->second.fd == fd) {
            printf("peer offline: %s\n", it->first.c_str());
            std::map<std::string, PeerSession>::iterator to_erase = it++;
            peers->erase(to_erase);
        } else {
            ++it;
        }
    }
}

static void sweep_expired_peers(std::map<std::string, PeerSession>* peers)
{
    time_t now = time(NULL);
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end();) {
        if (it->second.expire_at <= now) {
            printf("peer lease expired: %s\n", it->first.c_str());
            std::map<std::string, PeerSession>::iterator to_erase = it++;
            peers->erase(to_erase);
        } else {
            ++it;
        }
    }
}

static bool send_all(int fd, const void* buf, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t         left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    return true;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool send_message(int fd, const eular::ntrs::Message& msg)
{
    uint8_t buf[8192];
    size_t  len = 0;
    if (eular::ntrs::encodeMessage(msg, buf, sizeof(buf), &len) != 0) {
        return false;
    }
    return send_all(fd, buf, len);
}

static bool drain_control_messages(int fd, ControlClientRxState* state, std::vector<eular::ntrs::Message>* messages,
                                   bool* peer_closed)
{
    uint8_t chunk[2048];

    if (state == NULL || messages == NULL || peer_closed == NULL) {
        return false;
    }

    *peer_closed = false;
    for (;;) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (n > 0) {
            state->buffer.insert(state->buffer.end(), chunk, chunk + n);
            continue;
        }
        if (n == 0) {
            *peer_closed = true;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        *peer_closed = true;
        break;
    }

    while (state->buffer.size() >= eular::ntrs::FRAME_HDR_SIZE) {
        uint32_t             frame_size = 0;
        eular::ntrs::Message msg;

        if (!eular::ntrs::frameSizeFromHeader(state->buffer.data(), eular::ntrs::FRAME_HDR_SIZE, &frame_size) ||
            frame_size < eular::ntrs::FRAME_HDR_SIZE || frame_size > 8192u) {
            *peer_closed = true;
            return false;
        }
        if (state->buffer.size() < frame_size) {
            break;
        }
        if (!eular::ntrs::decodeMessage(state->buffer.data(), frame_size, &msg)) {
            *peer_closed = true;
            return false;
        }

        messages->push_back(msg);
        state->buffer.erase(state->buffer.begin(), state->buffer.begin() + frame_size);
    }

    return true;
}

static int create_stun_socket(const std::string& self_stun_endpoint)
{
    uint16_t port = endpoint_port(self_stun_endpoint);
    if (port == 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int create_stun_socket_with_port(uint16_t port)
{
    if (port == 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool send_udp_filter_probe(int udp_fd, const std::string& target_ip, uint16_t target_port,
                                  const std::string& token, const std::string& tag)
{
    static const int kFilterProbeBurstCount = 3;
    if (udp_fd < 0 || target_ip.empty() || target_port == 0 || token.empty()) {
        return false;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(target_port);
    if (inet_pton(AF_INET, target_ip.c_str(), &dst.sin_addr) != 1) {
        return false;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "NTRS_FILTER_PROBE|%s|%s", token.c_str(), tag.c_str());
    for (int i = 0; i < kFilterProbeBurstCount; ++i) {
        ssize_t n = sendto(udp_fd, payload, strlen(payload), 0, (struct sockaddr*)&dst, sizeof(dst));
        if (n <= 0) {
            return false;
        }
    }
    return true;
}

static bool peer_ipv4_string(int fd, std::string* ip_out)
{
    struct sockaddr_storage addr;
    socklen_t               addr_len = sizeof(addr);
    char                    host[NI_MAXHOST];

    if (ip_out == NULL) {
        return false;
    }
    if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) != 0) {
        return false;
    }
    if (getnameinfo((struct sockaddr*)&addr, addr_len, host, sizeof(host), NULL, 0, NI_NUMERICHOST) != 0) {
        return false;
    }

    if (addr.ss_family == AF_INET6) {
        const struct sockaddr_in6* addr6 = (const struct sockaddr_in6*)&addr;
        if (IN6_IS_ADDR_V4MAPPED(&addr6->sin6_addr)) {
            struct in_addr v4_addr;
            char           v4_host[INET_ADDRSTRLEN] = {0};

            memcpy(&v4_addr, &addr6->sin6_addr.s6_addr[12], sizeof(v4_addr));
            if (inet_ntop(AF_INET, &v4_addr, v4_host, sizeof(v4_host)) != NULL) {
                *ip_out = v4_host;
                return true;
            }
        }
    }

    *ip_out = host;
    return !ip_out->empty();
}

static void init_federation_result(FederationRequest* request)
{
    request->result.type = request->job.type;
    request->result.fd = request->job.fd;
    request->result.client_generation = request->job.client_generation;
    request->result.request_id = request->job.request_id;
    request->result.ok = false;
    request->result.same_ip_diff_port = request->job.same_ip_diff_port;
    request->result.diff_ip = false;
}

static bool build_stun_binding_response(const uint8_t* txid, const struct sockaddr_in* mapped_addr,
                                        const std::string& response_origin_ip, uint16_t response_origin_port,
                                        const std::string& other_address_ip, uint16_t other_address_port,
                                        std::vector<uint8_t>* out)
{
    eular::stun::StunMsgBuilder builder;

    if (txid == NULL || mapped_addr == NULL || out == NULL || response_origin_ip.empty()) {
        return false;
    }

    builder.setMsgType(ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_RESPONSE));
    builder.setTransactionId(txid);
    builder.addAttribute(STUN_ATTR_XOR_MAPPED_ADDRESS, eular::stun::SocketAddress((const sockaddr*)mapped_addr));
    builder.addAttribute(STUN_ATTR_RESPONSE_ORIGIN,
                         eular::stun::SocketAddress(response_origin_ip, response_origin_port));
    if (!other_address_ip.empty() && other_address_port != 0) {
        builder.addAttribute(STUN_ATTR_OTHER_ADDRESS, eular::stun::SocketAddress(other_address_ip, other_address_port));
    }

    *out = builder.message();
    return !out->empty();
}

static bool send_stun_binding_response(int stun_fd, const struct sockaddr_in* target_addr, const uint8_t* txid,
                                       const struct sockaddr_in* mapped_addr, const std::string& response_origin_ip,
                                       uint16_t response_origin_port, const std::string& other_address_ip,
                                       uint16_t other_address_port)
{
    std::vector<uint8_t> rsp;

    if (stun_fd < 0 || target_addr == NULL || mapped_addr == NULL) {
        return false;
    }
    if (!build_stun_binding_response(txid, mapped_addr, response_origin_ip, response_origin_port, other_address_ip,
                                     other_address_port, &rsp)) {
        return false;
    }

    return sendto(stun_fd, rsp.data(), rsp.size(), 0, (const struct sockaddr*)target_addr, sizeof(*target_addr)) > 0;
}

static std::string endpoint_text(const struct sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string((unsigned)ntohs(addr.sin_port));
}

static bool encode_tx_message(const eular::ntrs::Message& msg, std::vector<uint8_t>* out)
{
    uint8_t buf[8192];
    size_t  len = 0;
    if (out == NULL || eular::ntrs::encodeMessage(msg, buf, sizeof(buf), &len) != 0) {
        return false;
    }
    out->assign(buf, buf + len);
    return true;
}

static void federation_dns_cb(int result, struct evutil_addrinfo* res, void* arg)
{
    FederationRequest* request = static_cast<FederationRequest*>(arg);
    if (request == NULL) {
        if (res != NULL) {
            evutil_freeaddrinfo(res);
        }
        return;
    }

    request->dns_request = NULL;
    if (result != 0 || res == NULL) {
        request->control_index++;
        request->state = FederationState::CONNECTING;
        return;
    }

    request->resolved_addrs = res;
    request->next_addr = res;
    request->state = FederationState::CONNECTING;
}

static bool start_federation_attempt(FederationRequest* request, struct evdns_base* dns_base)
{
    while (request != NULL && request->control_index < request->job.controls.size()) {
        if (request->resolved_addrs == NULL) {
            std::string            host;
            uint16_t               port = 0;
            char                   port_text[16];
            struct evutil_addrinfo hints;

            if (!parse_endpoint(request->job.controls[request->control_index], &host, &port)) {
                request->control_index++;
                continue;
            }

            memset(&hints, 0, sizeof(hints));
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_family = AF_UNSPEC;
            snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
            request->state = FederationState::RESOLVING;
            request->deadline = time(NULL) + 3;
            request->dns_request =
                evdns_getaddrinfo(dns_base, host.c_str(), port_text, &hints, federation_dns_cb, request);
            if (request->state == FederationState::RESOLVING && request->dns_request == NULL &&
                request->resolved_addrs == NULL) {
                request->control_index++;
                continue;
            }
            return true;
        }

        if (request->next_addr == NULL) {
            evutil_freeaddrinfo(request->resolved_addrs);
            request->resolved_addrs = NULL;
            request->control_index++;
            continue;
        }

        struct evutil_addrinfo* addr = request->next_addr;
        request->next_addr = request->next_addr->ai_next;
        request->fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (request->fd < 0) {
            continue;
        }
        set_nonblocking(request->fd);

        int rc = connect(request->fd, addr->ai_addr, addr->ai_addrlen);
        if (rc == 0) {
            request->state = FederationState::SENDING_AUTH;
        } else if (errno == EINPROGRESS) {
            request->state = FederationState::CONNECTING;
        } else {
            close(request->fd);
            request->fd = -1;
            request->control_index++;
            continue;
        }

        request->tx_buffer.clear();
        request->tx_offset = 0;
        request->rx_state.buffer.clear();
        request->deadline = time(NULL) + 2;
        return true;
    }

    return false;
}

static bool prepare_federation_auth(FederationRequest* request)
{
    eular::ntrs::Message req;
    eular::ntrs::messageInit(&req, eular::ntrs::MessageType::AUTH_REQ, 1);
    eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, request->job.federation_peer_id.c_str());
    eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, request->job.auth_secret.c_str());
    request->tx_offset = 0;
    return encode_tx_message(req, &request->tx_buffer);
}

static bool prepare_federation_request(FederationRequest* request)
{
    eular::ntrs::Message req;
    if (request->job.type == AsyncFederationJobType::FETCH_STUN) {
        eular::ntrs::messageInit(&req, eular::ntrs::MessageType::SERVER_INFO_REQ, 2);
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::QUERY, "stun_endpoint");
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, request->session_token.c_str());
    } else if (request->job.type == AsyncFederationJobType::SEND_PROBE) {
        eular::ntrs::messageInit(&req, eular::ntrs::MessageType::SERVER_SEND_PROBE_REQ, 2);
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::TARGET_IP, request->job.target_ip.c_str());
        eular::ntrs::messageAddU16ByTag(&req, eular::ntrs::FieldTag::TARGET_PORT, request->job.target_port);
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, request->job.probe_token.c_str());
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID,
                                           request->job.federation_peer_id.c_str());
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::SRC_PEER_ID,
                                           request->job.owner_peer_id.c_str());
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::QUERY, request->job.probe_auth.c_str());
        eular::ntrs::messageAddU32ByTag(&req, eular::ntrs::FieldTag::EXPIRE_AT,
                                        (uint32_t)request->job.probe_expire_at);
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::SESSION_ID, request->session_token.c_str());
    } else {
        eular::ntrs::messageInit(&req, eular::ntrs::MessageType::SERVER_STUN_REQ, 2);
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::TARGET_IP, request->job.target_ip.c_str());
        eular::ntrs::messageAddU16ByTag(&req, eular::ntrs::FieldTag::TARGET_PORT, request->job.target_port);
        eular::ntrs::messageAddBytesByTag(&req, eular::ntrs::FieldTag::STUN_TXID, request->job.stun_txid.data(),
                                     (uint16_t)request->job.stun_txid.size());
        eular::ntrs::messageAddBoolByTag(&req, eular::ntrs::FieldTag::USE_ALT_PORT, request->job.use_alt_port);
        eular::ntrs::messageAddStringByTag(&req, eular::ntrs::FieldTag::SESSION_ID, request->session_token.c_str());
    }
    request->tx_offset = 0;
    return encode_tx_message(req, &request->tx_buffer);
}

static bool flush_federation_tx(FederationRequest* request)
{
    while (request->tx_offset < request->tx_buffer.size()) {
        ssize_t n = send(request->fd, request->tx_buffer.data() + request->tx_offset,
                         request->tx_buffer.size() - request->tx_offset, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (n <= 0) {
            return false;
        }
        request->tx_offset += (size_t)n;
    }
    return true;
}

static void close_federation_socket(FederationRequest* request)
{
    if (request->fd >= 0) {
        close(request->fd);
        request->fd = -1;
    }
}

static void close_federation_attempt(FederationRequest* request)
{
    close_federation_socket(request);
    if (request->dns_request != NULL) {
        evdns_getaddrinfo_cancel(request->dns_request);
        request->dns_request = NULL;
    }
    if (request->resolved_addrs != NULL) {
        evutil_freeaddrinfo(request->resolved_addrs);
        request->resolved_addrs = NULL;
        request->next_addr = NULL;
    }
    request->tx_buffer.clear();
    request->tx_offset = 0;
    request->rx_state.buffer.clear();
    request->session_token.clear();
}

static bool federation_result_from_response(FederationRequest* request, const eular::ntrs::Message& rsp)
{
    if (request->job.type == AsyncFederationJobType::FETCH_STUN) {
        if (rsp.type != eular::ntrs::MessageType::SERVER_INFO_RSP) {
            return false;
        }
        std::string endpoint = msg_str_tag(rsp, eular::ntrs::FieldTag::STUN_ENDPOINT);
        if (endpoint.empty()) {
            return false;
        }
        request->result.ok = true;
        request->result.selected_control = request->job.controls[request->control_index];
        request->result.peer_stun = endpoint;
        return true;
    }

    if (request->job.type == AsyncFederationJobType::SEND_STUN) {
        uint8_t result_code = (uint8_t)eular::ntrs::ResultCode::UNKNOWN;
        eular::ntrs::messageGetU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT, &result_code);
        if (rsp.type != eular::ntrs::MessageType::SERVER_STUN_RSP ||
            result_code != (uint8_t)eular::ntrs::ResultCode::OK) {
            return false;
        }
        request->result.ok = true;
        request->result.selected_control = request->job.controls[request->control_index];
        return true;
    }

    uint8_t result_code = (uint8_t)eular::ntrs::ResultCode::UNKNOWN;
    eular::ntrs::messageGetU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT, &result_code);
    if (rsp.type != eular::ntrs::MessageType::SERVER_SEND_PROBE_RSP ||
        result_code != (uint8_t)eular::ntrs::ResultCode::OK) {
        return false;
    }
    request->result.ok = true;
    request->result.diff_ip = true;
    request->result.selected_control = request->job.controls[request->control_index];
    return true;
}

static void fail_federation_attempt(FederationRequest* request, struct evdns_base* dns_base)
{
    close_federation_socket(request);
    request->tx_buffer.clear();
    request->tx_offset = 0;
    request->rx_state.buffer.clear();
    request->session_token.clear();
    if (request->next_addr == NULL) {
        if (request->resolved_addrs != NULL) {
            evutil_freeaddrinfo(request->resolved_addrs);
            request->resolved_addrs = NULL;
        }
        request->control_index++;
    }
    start_federation_attempt(request, dns_base);
}

static bool advance_federation_request(FederationRequest* request, bool readable, bool writable,
                                       AsyncFederationResult* completed, struct evdns_base* dns_base)
{
    if (request == NULL || completed == NULL) {
        return false;
    }

    if (time(NULL) > request->deadline) {
        if (request->dns_request != NULL) {
            evdns_getaddrinfo_cancel(request->dns_request);
            request->dns_request = NULL;
        }
        fail_federation_attempt(request, dns_base);
    }
    if (request->state == FederationState::CONNECTING && request->fd < 0) {
        start_federation_attempt(request, dns_base);
    }
    if (request->state == FederationState::RESOLVING) {
        return false;
    }
    if (request->fd < 0) {
        *completed = request->result;
        return true;
    }

    if (request->state == FederationState::CONNECTING && writable) {
        int       err = 0;
        socklen_t err_len = sizeof(err);
        if (getsockopt(request->fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
            fail_federation_attempt(request, dns_base);
        } else {
            request->state = FederationState::SENDING_AUTH;
        }
    }

    if (request->state == FederationState::SENDING_AUTH) {
        if (request->tx_buffer.empty() && !prepare_federation_auth(request)) {
            fail_federation_attempt(request, dns_base);
        } else if (writable && !flush_federation_tx(request)) {
            fail_federation_attempt(request, dns_base);
        } else if (request->tx_offset == request->tx_buffer.size()) {
            request->state = FederationState::READING_AUTH;
            request->tx_buffer.clear();
            request->tx_offset = 0;
        }
    }

    if ((request->state == FederationState::READING_AUTH || request->state == FederationState::READING_RESPONSE) &&
        readable) {
        std::vector<eular::ntrs::Message> messages;
        bool                              peer_closed = false;
        if (!drain_control_messages(request->fd, &request->rx_state, &messages, &peer_closed) || peer_closed) {
            fail_federation_attempt(request, dns_base);
        } else {
            for (size_t i = 0; i < messages.size(); ++i) {
                if (request->state == FederationState::READING_AUTH) {
                    if (messages[i].type != eular::ntrs::MessageType::AUTH_RSP ||
                        msg_str_tag(messages[i], eular::ntrs::FieldTag::TOKEN)[0] == '\0') {
                        fail_federation_attempt(request, dns_base);
                        break;
                    }
                    request->session_token = msg_str_tag(messages[i], eular::ntrs::FieldTag::TOKEN);
                    if (!prepare_federation_request(request)) {
                        fail_federation_attempt(request, dns_base);
                        break;
                    }
                    request->state = FederationState::SENDING_REQUEST;
                } else if (federation_result_from_response(request, messages[i])) {
                    close_federation_attempt(request);
                    *completed = request->result;
                    return true;
                } else {
                    fail_federation_attempt(request, dns_base);
                    break;
                }
            }
        }
    }

    if (request->state == FederationState::SENDING_REQUEST) {
        if (writable && !flush_federation_tx(request)) {
            fail_federation_attempt(request, dns_base);
        } else if (request->tx_offset == request->tx_buffer.size()) {
            request->state = FederationState::READING_RESPONSE;
            request->tx_buffer.clear();
            request->tx_offset = 0;
        }
    }

    if (request->fd < 0) {
        *completed = request->result;
        return true;
    }

    return false;
}

static bool start_federation_job(const AsyncFederationJob& job, std::list<FederationRequest>* requests,
                                 AsyncFederationResult* immediate_result, struct evdns_base* dns_base)
{
    requests->push_back(FederationRequest());
    FederationRequest* request = &requests->back();
    request->job = job;
    init_federation_result(request);
    if (!start_federation_attempt(request, dns_base)) {
        if (immediate_result != NULL) {
            *immediate_result = request->result;
        }
        requests->pop_back();
        return false;
    }
    return true;
}

static void handle_stun_packet(int stun_fd, bool is_alt_socket, int stun_primary_fd, int stun_alt_fd,
                               const std::string& self_stun_ip, uint16_t self_stun_port, uint16_t stun_alt_bind_port,
                               const std::vector<std::string>& probe_controls, const std::string& auth_secret,
                               std::list<FederationRequest>* federation_requests,
                               std::deque<AsyncFederationResult>* async_results, struct evdns_base* federation_dns_base,
                               bool verbose)
{
    uint8_t            buf[2048];
    struct sockaddr_in peer_addr;
    socklen_t          peer_len = sizeof(peer_addr);

    (void)is_alt_socket;
    (void)stun_primary_fd;

    ssize_t            n = recvfrom(stun_fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer_addr, &peer_len);
    if (n <= 0) {
        return;
    }

    eular::stun::StunMsgParser parser;
    if (!parser.parse(buf, (size_t)n)) {
        return;
    }

    if (parser.msgType() != ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST)) {
        return;
    }

    const uint8_t* trx = parser.transactionId();
    if (trx == NULL) {
        return;
    }
    node_verbose_log(verbose, "STUN request fd=%d src=%s txid=%02x%02x%02x%02x\n", stun_fd,
                     endpoint_text(peer_addr).c_str(), trx[0], trx[1], trx[2], trx[3]);
    uint32_t change_request = 0;
    bool     change_ip = false;
    bool     change_port = false;
    const eular::any* change_any =
        parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_CHANGE_REQUEST));
    if (change_any != NULL) {
        const uint32_t* value = eular::any_cast<uint32_t>(change_any);
        if (value != NULL) {
            change_request = *value;
            change_ip = (change_request & 0x04u) != 0;
            change_port = (change_request & 0x02u) != 0;
        }
    }
    node_verbose_log(verbose, "STUN request parsed fd=%d src=%s change_ip=%s change_port=%s\n", stun_fd,
                     endpoint_text(peer_addr).c_str(), change_ip ? "true" : "false",
                     change_port ? "true" : "false");

    if (!change_ip && !change_port) {
        std::string       other_ip;
        uint16_t          other_port = 0;
        if (!resolve_probe_other_address(probe_controls, self_stun_port, &other_ip, &other_port)) {
            other_ip = self_stun_ip;
            other_port = stun_alt_bind_port;
        }
        node_verbose_log(verbose, "STUN response local fd=%d src=%s other=%s:%u\n", stun_fd,
                         endpoint_text(peer_addr).c_str(), other_ip.c_str(), (unsigned)other_port);
        if (!send_stun_binding_response(stun_fd, &peer_addr, trx, &peer_addr, self_stun_ip, self_stun_port, other_ip,
                                        other_port)) {
            printf("STUN rsp send failed errno=%d(%s)\n", errno, strerror(errno));
        }
        return;
    }

    if (!change_ip && change_port) {
        if (stun_alt_fd < 0) {
            return;
        }
        node_verbose_log(verbose, "STUN response alt-port fd=%d src=%s alt_port=%u\n", stun_alt_fd,
                         endpoint_text(peer_addr).c_str(), (unsigned)stun_alt_bind_port);
        if (!send_stun_binding_response(stun_alt_fd, &peer_addr, trx, &peer_addr, self_stun_ip, stun_alt_bind_port,
                                        self_stun_ip, self_stun_port)) {
            printf("STUN change-port rsp send failed errno=%d(%s)\n", errno, strerror(errno));
        }
        return;
    }

    if (probe_controls.empty() || federation_requests == NULL || federation_dns_base == NULL) {
        return;
    }

    AsyncFederationJob job;
    job.type = AsyncFederationJobType::SEND_STUN;
    job.fd = -1;
    job.client_generation = 0;
    job.request_id = 0;
    job.controls = probe_controls;
    job.auth_secret = auth_secret;
    job.federation_peer_id = "service_node_federation";
    job.target_ip = inet_ntoa(peer_addr.sin_addr);
    job.target_port = ntohs(peer_addr.sin_port);
    job.same_ip_diff_port = false;
    job.use_alt_port = change_port;
    job.stun_txid.assign(trx, trx + STUN_TRX_ID_SIZE);
    node_verbose_log(verbose, "STUN delegate fd=%d target=%s:%u use_alt_port=%s\n", stun_fd, job.target_ip.c_str(),
                     (unsigned)job.target_port, change_port ? "true" : "false");

    AsyncFederationResult result;
    if (!start_federation_job(job, federation_requests, &result, federation_dns_base)) {
        if (async_results != NULL) {
            async_results->push_back(result);
        }
    }
}

static void send_error(int fd, uint64_t req, const char* code, const char* message)
{
    eular::ntrs::Message rsp;
    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::ERROR_RSP, (uint32_t)req);
    eular::ntrs::messageAddString(&rsp, "code", code);
    eular::ntrs::messageAddString(&rsp, "message", message);
    send_message(fd, rsp);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    int         port = 19000;
    uint16_t    stun_bind_port = 3478;
    uint16_t    stun_alt_bind_port = 3479;
    std::string public_host;
    std::string self_stun_endpoint;
    std::string peer_node_control_endpoint;
    std::string mqtt_broker;
    int         mqtt_port = 1883;
    std::string node_id;
    std::string region = "default";
    std::string mqtt_username;
    std::string mqtt_password;
    std::string auth_secret = "ntrs-dev-secret";
    bool        verbose = false;

    if (argc > 1 && strncmp(argv[1], "--", 2) == 0) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            const char* value = (i + 1 < argc) ? argv[i + 1] : NULL;
            if (arg == "--hub" && value != NULL) {
                std::string hub_host;
                uint16_t    hub_port = 0;
                if (!parse_endpoint(value, &hub_host, &hub_port)) {
                    printf("invalid --hub endpoint: %s\n", value);
                    return 1;
                }
                mqtt_broker = hub_host;
                mqtt_port = hub_port;
                ++i;
            } else if (arg == "--node-id" && value != NULL) {
                node_id = value;
                ++i;
            } else if ((arg == "--public-host" || arg == "--advertise-host") && value != NULL) {
                public_host = value;
                ++i;
            } else if (arg == "--control-port" && value != NULL) {
                port = atoi(value);
                ++i;
            } else if (arg == "--stun-port" && value != NULL) {
                int parsed = atoi(value);
                if (parsed <= 0 || parsed > 65535) {
                    printf("invalid --stun-port: %s\n", value);
                    return 1;
                }
                stun_bind_port = (uint16_t)parsed;
                ++i;
            } else if (arg == "--stun-alt-port" && value != NULL) {
                int parsed = atoi(value);
                if (parsed <= 0 || parsed > 65535) {
                    printf("invalid --stun-alt-port: %s\n", value);
                    return 1;
                }
                stun_alt_bind_port = (uint16_t)parsed;
                ++i;
            } else if (arg == "--region" && value != NULL) {
                region = value;
                ++i;
            } else if (arg == "--mqtt-username" && value != NULL) {
                mqtt_username = value;
                ++i;
            } else if (arg == "--mqtt-password" && value != NULL) {
                mqtt_password = value;
                ++i;
            } else if (arg == "--auth-secret" && value != NULL) {
                auth_secret = value;
                ++i;
            } else if (arg == "--verbose" || arg == "-v") {
                verbose = true;
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }

        if (mqtt_broker.empty() || node_id.empty() || public_host.empty()) {
            print_usage(argv[0]);
            return 1;
        }
        if (port <= 0 || port > 65535) {
            printf("invalid --control-port: %d\n", port);
            return 1;
        }
        self_stun_endpoint = format_endpoint(public_host, stun_bind_port);
    } else {
        if (argc > 1) {
            port = atoi(argv[1]);
        }
        if (argc > 2) {
            self_stun_endpoint = argv[2];
        }
        if (argc > 3) {
            peer_node_control_endpoint = argv[3];
        }
        mqtt_broker = (argc > 4) ? argv[4] : "";
        mqtt_port = (argc > 5) ? atoi(argv[5]) : 1883;
        node_id = (argc > 6) ? argv[6] : "";
        region = (argc > 7) ? argv[7] : "default";
        mqtt_username = (argc > 8) ? argv[8] : "";
        mqtt_password = (argc > 9) ? argv[9] : "";
        auth_secret = (argc > 10) ? argv[10] : "ntrs-dev-secret";
    }

    if (self_stun_endpoint.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_process_signal);
    signal(SIGTERM, on_process_signal);

    eular::ntrs::ControlAuthManager auth_manager(auth_secret, kLeaseSec);

    bool        use_hub_assignment = (!mqtt_broker.empty() && !node_id.empty());
    std::string boot_id =
        std::to_string((unsigned long long)time(NULL)) + "-" + std::to_string((unsigned long long)getpid());
    std::string                               assignment_p1;
    std::string                               assignment_p2;
    std::string                               assignment_b1;
    uint32_t                                  assignment_version = 0;
    ev::EventLoop                             mqtt_loop;
    std::unique_ptr<eular::orion::MqttClient> mqtt_client;
    bool                                      mqtt_assignment_ready = false;

    if (use_hub_assignment) {
        mqtt_client.reset(new eular::orion::MqttClient(
            mqtt_broker, mqtt_port, "ntrs-node-" + node_id + "-" + std::to_string((unsigned long long)getpid()),
            mqtt_username, mqtt_password));
        if (!mqtt_client->attachEventLoop(mqtt_loop.loop())) {
            printf("hub mode disabled because mqtt attach event loop failed\n");
            mqtt_client.reset();
            use_hub_assignment = false;
        }
    }

    if (use_hub_assignment && mqtt_client) {
        eular::ntrs::Message will;
        uint8_t              will_buf[8192];
        size_t               will_len = 0;
        const std::string    will_ts = now_iso8601();
        eular::ntrs::messageInit(&will, eular::ntrs::MessageType::NODE_PRESENCE, 0);
        eular::ntrs::messageAddStringByTag(&will, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
        eular::ntrs::messageAddStringByTag(&will, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
        eular::ntrs::messageAddU8ByTag(&will, eular::ntrs::FieldTag::STATUS,
                                       (uint8_t)eular::ntrs::NodeStatusCode::OFFLINE);
        eular::ntrs::messageAddU8ByTag(&will, eular::ntrs::FieldTag::REASON,
                                       (uint8_t)eular::ntrs::ReasonCode::LWT);
        eular::ntrs::messageAddStringByTag(&will, eular::ntrs::FieldTag::TS, will_ts.c_str());
        if (eular::ntrs::encodeMessage(will, will_buf, sizeof(will_buf), &will_len) != 0) {
            printf("hub mode disabled because mqtt will encode failed\n");
            mqtt_client.reset();
            use_hub_assignment = false;
        } else {
            mqtt_client->setWillMessage(topic_for(node_id, "presence"), will_buf, will_len, 1, true);
        }
    }

    std::string self_stun_host;
    uint16_t    self_stun_port = 0;
    std::string self_stun_ip;
    if (!parse_endpoint(self_stun_endpoint, &self_stun_host, &self_stun_port)) {
        self_stun_host = "127.0.0.1";
    }
    if (!resolve_ipv4_literal(self_stun_host, &self_stun_ip)) {
        self_stun_ip = self_stun_host;
    }

    if (use_hub_assignment && mqtt_client) {
        const std::string assignment_topic = "ntrs/hub/node/" + node_id + "/assignment";
        auto              publish_node_registration = [&]() {
            eular::ntrs::Message reg;
            const std::string    reg_control = format_endpoint(self_stun_host, (uint16_t)port);
            const std::string    reg_ts = now_iso8601();
            eular::ntrs::messageInit(&reg, eular::ntrs::MessageType::NODE_REGISTER, 1);
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::REGION, region.c_str());
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::STUN_ENDPOINT, self_stun_endpoint.c_str());
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::CONTROL_ENDPOINT, reg_control.c_str());
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::NAT_TYPE, "service_node");
            eular::ntrs::messageAddU32ByTag(&reg, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 5);
            eular::ntrs::messageAddStringByTag(&reg, eular::ntrs::FieldTag::TS, reg_ts.c_str());
            node_verbose_log(verbose, "publishing node register node=%s control=%s stun=%s\n", node_id.c_str(),
                             reg_control.c_str(), self_stun_endpoint.c_str());
            mqtt_publish_message(mqtt_client.get(), topic_for(node_id, "register"), reg, true);
            publish_presence(mqtt_client.get(), node_id, boot_id, "online", "startup", 2);
        };

        mqtt_client->setMessageCallback(
            [&, assignment_topic](const std::string& topic, const uint8_t* payload, size_t payload_len) {
                eular::ntrs::Message msg;
                if (!eular::ntrs::decodeMessage(payload, payload_len, &msg)) {
                    printf("node mqtt decode failed topic=%s len=%zu\n", topic.c_str(), payload_len);
                    return;
                }
                uint8_t event_code = (uint8_t)eular::ntrs::EventCode::UNKNOWN;
                eular::ntrs::messageGetU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, &event_code);
                if (topic == assignment_topic && msg.type == eular::ntrs::MessageType::HUB_CLUSTER_EVENT &&
                    event_code == (uint8_t)eular::ntrs::EventCode::ASSIGNMENT) {
                    assignment_p1 = msg_str_tag(msg, eular::ntrs::FieldTag::PRIMARY1_CONTROL);
                    assignment_p2 = msg_str_tag(msg, eular::ntrs::FieldTag::PRIMARY2_CONTROL);
                    assignment_b1 = msg_str_tag(msg, eular::ntrs::FieldTag::BACKUP1_CONTROL);
                    assignment_version = msg.request_id;
                }
            });
        mqtt_client->setConnectCallback([&, assignment_topic]() {
            node_verbose_log(verbose, "node mqtt connected node=%s subscribing topic=%s\n", node_id.c_str(),
                             assignment_topic.c_str());
            if (!mqtt_client->subscribe(assignment_topic, 1)) {
                printf("node mqtt subscribe failed node=%s topic=%s\n", node_id.c_str(), assignment_topic.c_str());
            }
            publish_node_registration();
            if (!mqtt_assignment_ready) {
                mqtt_assignment_ready = true;
            }
        });
        mqtt_client->setDisconnectCallback(
            [&](int rc) { printf("node mqtt disconnected rc=%d node=%s\n", rc, node_id.c_str()); });
        if (!mqtt_client->connect()) {
            printf("hub mode disabled because mqtt connect failed: %s:%d\n", mqtt_broker.c_str(), mqtt_port);
            mqtt_client.reset();
            use_hub_assignment = false;
        }
    }

    int stun_fd = create_stun_socket(self_stun_endpoint);
    if (stun_fd < 0) {
        printf("failed to start built-in STUN on %s\n", self_stun_endpoint.c_str());
        return 1;
    }

    uint16_t stun_port = endpoint_port(self_stun_endpoint);
    int      stun_alt_fd = -1;
    if (stun_alt_bind_port == stun_port && stun_port > 0 && stun_port < 65535) {
        stun_alt_bind_port = (uint16_t)(stun_port + 1);
    }
    if (stun_alt_bind_port > 0) {
        stun_alt_fd = create_stun_socket_with_port(stun_alt_bind_port);
    }

    int listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    int off = 0;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    printf("NTRS node listening on :%d self_stun=%s alt_stun_port=%d peer_node_control=%s\n", port,
           self_stun_endpoint.c_str(), stun_alt_fd >= 0 ? (int)stun_alt_bind_port : -1,
           peer_node_control_endpoint.empty() ? "-" : peer_node_control_endpoint.c_str());

    std::set<int>                       clients;
    std::map<int, uint64_t>             client_generations;
    std::map<int, ControlClientRxState> client_rx_states;
    std::map<std::string, PeerSession>  peers;
    std::deque<AsyncFederationResult>   async_results;
    std::list<FederationRequest>        federation_requests;
    uint64_t                            next_client_generation = 1;
    time_t                              last_hb_ts = 0;
    event_base*                         federation_event_base = event_base_new();
    evdns_base*                         federation_dns_base = NULL;
    if (federation_event_base != NULL) {
        federation_dns_base = evdns_base_new(federation_event_base, 1);
    }
    if (federation_dns_base == NULL) {
        printf("failed to initialize federation evdns\n");
        if (federation_event_base != NULL) {
            event_base_free(federation_event_base);
        }
        return 1;
    }
    evdns_base_set_option(federation_dns_base, "timeout", "2");
    evdns_base_set_option(federation_dns_base, "attempts", "2");

    while (!g_stop) {
        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(listen_fd, &rfds);
        FD_SET(stun_fd, &rfds);
        int maxfd = listen_fd;
        if (stun_fd > maxfd) {
            maxfd = stun_fd;
        }
        if (stun_alt_fd >= 0) {
            FD_SET(stun_alt_fd, &rfds);
            if (stun_alt_fd > maxfd) {
                maxfd = stun_alt_fd;
            }
        }
        for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
            FD_SET(*it, &rfds);
            if (*it > maxfd) {
                maxfd = *it;
            }
        }
        for (std::list<FederationRequest>::iterator it = federation_requests.begin(); it != federation_requests.end();
             ++it) {
            int fd = it->fd;
            if (fd < 0) {
                continue;
            }
            if (it->state == FederationState::CONNECTING || it->state == FederationState::SENDING_AUTH ||
                it->state == FederationState::SENDING_REQUEST) {
                FD_SET(fd, &wfds);
            }
            if (it->state == FederationState::READING_AUTH || it->state == FederationState::READING_RESPONSE) {
                FD_SET(fd, &rfds);
            }
            if (fd > maxfd) {
                maxfd = fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        for (std::list<FederationRequest>::iterator it = federation_requests.begin(); it != federation_requests.end();
             ++it) {
            if (it->state == FederationState::RESOLVING) {
                tv.tv_sec = 0;
                tv.tv_usec = 100000;
                break;
            }
        }

        int ret = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            continue;
        }

        sweep_expired_peers(&peers);
        auth_manager.sweepExpired((uint64_t)time(NULL));
        event_base_loop(federation_event_base, EVLOOP_NONBLOCK | EVLOOP_NO_EXIT_ON_EMPTY);

        if (use_hub_assignment && mqtt_client) {
            event_base_loop(mqtt_loop.loop(), EVLOOP_NONBLOCK | EVLOOP_NO_EXIT_ON_EMPTY);
            time_t now = time(NULL);
            if (now - last_hb_ts >= 5) {
                eular::ntrs::Message hb;
                const std::string    hb_ts = now_iso8601();
                eular::ntrs::messageInit(&hb, eular::ntrs::MessageType::NODE_HEARTBEAT, (uint32_t)now);
                eular::ntrs::messageAddStringByTag(&hb, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
                eular::ntrs::messageAddStringByTag(&hb, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
                eular::ntrs::messageAddU8ByTag(&hb, eular::ntrs::FieldTag::STATUS,
                                               (uint8_t)eular::ntrs::NodeStatusCode::ONLINE);
                eular::ntrs::messageAddU32ByTag(&hb, eular::ntrs::FieldTag::LOAD, (uint32_t)clients.size());
                eular::ntrs::messageAddStringByTag(&hb, eular::ntrs::FieldTag::NAT_TYPE, "service_node");
                eular::ntrs::messageAddU32ByTag(&hb, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 5);
                eular::ntrs::messageAddU32ByTag(&hb, eular::ntrs::FieldTag::ASSIGNMENT_VERSION, assignment_version);
                eular::ntrs::messageAddStringByTag(&hb, eular::ntrs::FieldTag::TS, hb_ts.c_str());
                mqtt_publish_message(mqtt_client.get(), topic_for(node_id, "heartbeat"), hb, false);
                last_hb_ts = now;
            }
        }

        if (FD_ISSET(stun_fd, &rfds)) {
            handle_stun_packet(
                stun_fd, false, stun_fd, stun_alt_fd, self_stun_ip, self_stun_port, stun_alt_bind_port,
                build_probe_controls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint),
                auth_secret, &federation_requests, &async_results, federation_dns_base, verbose);
        }
        if (stun_alt_fd >= 0 && FD_ISSET(stun_alt_fd, &rfds)) {
            handle_stun_packet(
                stun_alt_fd, true, stun_fd, stun_alt_fd, self_stun_ip, self_stun_port, stun_alt_bind_port,
                build_probe_controls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint),
                auth_secret, &federation_requests, &async_results, federation_dns_base, verbose);
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                set_nonblocking(fd);
                clients.insert(fd);
                client_generations[fd] = next_client_generation++;
                client_rx_states[fd] = ControlClientRxState();
                printf("peer session connected fd=%d\n", fd);
            }
        }

        std::vector<int> closed;
        for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
            int fd = *it;
            if (!FD_ISSET(fd, &rfds)) {
                continue;
            }

            std::vector<eular::ntrs::Message> pending_messages;
            bool                              peer_closed = false;
            if (!drain_control_messages(fd, &client_rx_states[fd], &pending_messages, &peer_closed)) {
                peer_closed = true;
            }
            if (peer_closed) {
                closed.push_back(fd);
                continue;
            }
            if (pending_messages.empty()) {
                continue;
            }

            for (size_t msg_index = 0; msg_index < pending_messages.size(); ++msg_index) {
                eular::ntrs::Message& msg = pending_messages[msg_index];
                switch (msg.type) {
                case eular::ntrs::MessageType::AUTH_REQ: {
                    std::string                 peer_id = msg_str_tag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string                 token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    eular::ntrs::ControlSession session;
                    std::string                 reason;
                    if (!auth_manager.issueSession(peer_id, token, fd, (uint64_t)time(NULL), &session, &reason)) {
                        send_error(fd, msg.request_id, "AUTH_FAILED", reason.c_str());
                        break;
                    }

                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::AUTH_RSP, msg.request_id);
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                                   (uint8_t)eular::ntrs::ResultCode::OK);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::TOKEN, session.token.c_str());
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_DEFAULT_SEC,
                                                    auth_manager.sessionTtlSec());
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)session.expire_at_sec);
                    send_message(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::REGISTER_REQ: {
                    std::string peer_id = msg_str_tag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string device_id = msg_str_tag(msg, eular::ntrs::FieldTag::DEVICE_ID);
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (peer_id.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }
                    if (!auth_manager.validateSession(fd, peer_id, session_token, (uint64_t)time(NULL), &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    PeerSession s;
                    s.peer_id = peer_id;
                    s.device_id = device_id;
                    s.local_ip = msg_str_tag(msg, eular::ntrs::FieldTag::LOCAL_IP);
                    s.local_port = msg_u16_tag(msg, eular::ntrs::FieldTag::LOCAL_PORT);
                    s.srflx_ip = msg_str_tag(msg, eular::ntrs::FieldTag::SRFLX_IP);
                    s.srflx_port = msg_u16_tag(msg, eular::ntrs::FieldTag::SRFLX_PORT);
                    s.srflx_ip_2 = msg_str_tag(msg, eular::ntrs::FieldTag::SRFLX_IP_2);
                    s.srflx_port_2 = msg_u16_tag(msg, eular::ntrs::FieldTag::SRFLX_PORT_2);
                    s.nat_class = msg_u16_tag(msg, eular::ntrs::FieldTag::NAT_CLASS, NTRS_NAT_CLASS_UNKNOWN);
                    s.nat_flags = msg_u16_tag(msg, eular::ntrs::FieldTag::NAT_FLAGS, NTRS_NAT_FLAG_NONE);
                    s.mapping_behavior =
                        msg_u16_tag(msg, eular::ntrs::FieldTag::MAPPING_BEHAVIOR, NTRS_MAPPING_UNKNOWN);
                    s.filtering_behavior =
                        msg_u16_tag(msg, eular::ntrs::FieldTag::FILTERING_BEHAVIOR, NTRS_FILTERING_UNKNOWN);
                    s.nat_type = msg_str_tag(msg, eular::ntrs::FieldTag::NAT_TYPE);
                    s.fd = fd;
                    s.expire_at = time(NULL) + kLeaseSec;
                    peers[peer_id] = s;

                    printf(
                        "REGISTER peer=%s device=%s local=%s:%u srflx=%s:%u srflx2=%s:%u stable=%s risk=%s class=%u "
                        "flags=0x%04x mapping=%u filtering=%u type=%s "
                        "probe1_ok=%s probe2_ok=%s probe1_rtt_ms=%d probe2_rtt_ms=%d rounds=%u p1succ=%u p2succ=%u "
                        "p1distinct=%u p2distinct=%u f_same_ip_port=%s f_diff_ip=%s\n",
                        peer_id.c_str(), device_id.c_str(), msg_str_tag(msg, eular::ntrs::FieldTag::LOCAL_IP),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::LOCAL_PORT),
                        msg_str_tag(msg, eular::ntrs::FieldTag::SRFLX_IP),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::SRFLX_PORT),
                        msg_str_tag(msg, eular::ntrs::FieldTag::SRFLX_IP_2),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::SRFLX_PORT_2),
                        msg_bool_tag(msg, eular::ntrs::FieldTag::MAPPING_STABLE) ? "true" : "false",
                        msg_str_tag(msg, eular::ntrs::FieldTag::NAT_RISK),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::NAT_CLASS, NTRS_NAT_CLASS_UNKNOWN),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::NAT_FLAGS, NTRS_NAT_FLAG_NONE),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::MAPPING_BEHAVIOR, NTRS_MAPPING_UNKNOWN),
                        msg_u16_tag(msg, eular::ntrs::FieldTag::FILTERING_BEHAVIOR, NTRS_FILTERING_UNKNOWN),
                        msg_str_tag(msg, eular::ntrs::FieldTag::NAT_TYPE),
                        msg_bool_tag(msg, eular::ntrs::FieldTag::PROBE1_OK) ? "true" : "false",
                        msg_bool_tag(msg, eular::ntrs::FieldTag::PROBE2_OK) ? "true" : "false",
                        msg_i32_tag(msg, eular::ntrs::FieldTag::PROBE1_RTT_MS),
                        msg_i32_tag(msg, eular::ntrs::FieldTag::PROBE2_RTT_MS),
                        msg_u32_tag(msg, eular::ntrs::FieldTag::PROBE_ROUNDS),
                        msg_u32_tag(msg, eular::ntrs::FieldTag::PROBE1_SUCCESS_COUNT),
                        msg_u32_tag(msg, eular::ntrs::FieldTag::PROBE2_SUCCESS_COUNT),
                        msg_u32_tag(msg, eular::ntrs::FieldTag::PROBE1_DISTINCT_MAPPINGS),
                        msg_u32_tag(msg, eular::ntrs::FieldTag::PROBE2_DISTINCT_MAPPINGS),
                        msg_bool_tag(msg, eular::ntrs::FieldTag::FILTER_SAME_IP_DIFF_PORT_RX) ? "true" : "false",
                        msg_bool_tag(msg, eular::ntrs::FieldTag::FILTER_DIFF_IP_RX) ? "true" : "false");

                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::REGISTER_RSP, msg.request_id);
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_SEC, 30);
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 10);
                    send_message(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::HEARTBEAT_REQ: {
                    std::string peer_id = msg_str_tag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (peer_id.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }
                    if (!auth_manager.validateSession(fd, peer_id, session_token, (uint64_t)time(NULL), &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator pit = peers.find(peer_id);
                    if (pit == peers.end() || pit->second.fd != fd) {
                        send_error(fd, msg.request_id, "NOT_REGISTERED", "peer not registered");
                        break;
                    }
                    pit->second.expire_at = time(NULL) + kLeaseSec;

                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::HEARTBEAT_RSP, msg.request_id);
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_SEC, 30);
                    send_message(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::UNREGISTER_REQ: {
                    std::string peer_id = msg_str_tag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (peer_id.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }
                    if (!auth_manager.validateSession(fd, peer_id, session_token, (uint64_t)time(NULL), &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator pit = peers.find(peer_id);
                    if (pit != peers.end() && pit->second.fd == fd) {
                        uint8_t reason_code = (uint8_t)eular::ntrs::ReasonCode::NONE;
                        eular::ntrs::messageGetU8ByTag(&msg, eular::ntrs::FieldTag::REASON, &reason_code);
                        printf("UNREGISTER peer=%s reason=%s\n", peer_id.c_str(),
                               eular::ntrs::reason_code_name((eular::ntrs::ReasonCode)reason_code));
                        peers.erase(pit);
                    }

                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::UNREGISTER_RSP, msg.request_id);
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                                   (uint8_t)eular::ntrs::ResultCode::OK);
                    send_message(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::SESSION_CREATE_REQ: {
                    std::string                   src = msg_str_tag(msg, eular::ntrs::FieldTag::SRC_PEER_ID);
                    std::string                   dst = msg_str_tag(msg, eular::ntrs::FieldTag::DST_PEER_ID);
                    std::string                   session_token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string                   reason;
                    eular::ntrs::PeerSessionLease peer_session;
                    uint64_t                      now_sec = (uint64_t)time(NULL);
                    if (src.empty() || dst.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "src/dst peer required");
                        break;
                    }
                    if (!auth_manager.validateSession(fd, src, session_token, now_sec, &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator src_it = peers.find(src);
                    std::map<std::string, PeerSession>::iterator dst_it = peers.find(dst);
                    if (src_it == peers.end() || src_it->second.fd != fd) {
                        send_error(fd, msg.request_id, "NOT_REGISTERED", "src peer not registered on this connection");
                        break;
                    }
                    if (dst_it == peers.end()) {
                        send_error(fd, msg.request_id, "DST_OFFLINE", "dst peer not online");
                        break;
                    }

                    std::string         sid = eular::ntrs::mintPeerSessionId(src, dst, now_sec);
                    SessionStrategyPlan strategy;
                    if (!auth_manager.issuePeerSession(src, dst, sid, now_sec, 60, &peer_session, &reason)) {
                        send_error(fd, msg.request_id, "AUTH_FAILED", reason.c_str());
                        break;
                    }
                    strategy = build_session_strategy(src_it->second, dst_it->second);

                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::SESSION_CREATE_RSP, msg.request_id);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::SESSION_ID, sid.c_str());
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::ROLE,
                                                   (uint8_t)eular::ntrs::RoleCode::INITIATOR);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::TOKEN, peer_session.token.c_str());
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::PUNCH_ORDER, strategy.src_punch_order);
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::CONNECT_ROLE, strategy.src_connect_role);
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::WARMUP_ROUNDS, strategy.src_warmup_rounds);
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::WARMUP_INTERVAL_MS, strategy.src_warmup_interval_ms);
                    eular::ntrs::messageAddU32ByTag(&rsp, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)peer_session.expire_at_sec);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_ID,
                                                       dst_it->second.peer_id.c_str());
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_LOCAL_IP,
                                                       dst_it->second.local_ip.c_str());
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_LOCAL_PORT,
                                                    dst_it->second.local_port);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_IP,
                                                       dst_it->second.srflx_ip.c_str());
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_PORT,
                                                    dst_it->second.srflx_port);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_IP_2,
                                                       dst_it->second.srflx_ip_2.c_str());
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_PORT_2,
                                                    dst_it->second.srflx_port_2);
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_NAT_CLASS,
                                                    dst_it->second.nat_class);
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_NAT_FLAGS,
                                                    dst_it->second.nat_flags);
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_MAPPING_BEHAVIOR,
                                                    dst_it->second.mapping_behavior);
                    eular::ntrs::messageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_FILTERING_BEHAVIOR,
                                                    dst_it->second.filtering_behavior);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_NAT_TYPE,
                                                       dst_it->second.nat_type.c_str());
                    send_message(fd, rsp);

                    eular::ntrs::Message notify;
                    eular::ntrs::messageInit(&notify, eular::ntrs::MessageType::SESSION_NOTIFY, 0);
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::SESSION_ID, sid.c_str());
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::TOKEN, peer_session.token.c_str());
                    eular::ntrs::messageAddU8ByTag(&notify, eular::ntrs::FieldTag::PUNCH_ORDER, strategy.dst_punch_order);
                    eular::ntrs::messageAddU8ByTag(&notify, eular::ntrs::FieldTag::CONNECT_ROLE, strategy.dst_connect_role);
                    eular::ntrs::messageAddU32ByTag(&notify, eular::ntrs::FieldTag::WARMUP_ROUNDS, strategy.dst_warmup_rounds);
                    eular::ntrs::messageAddU32ByTag(&notify, eular::ntrs::FieldTag::WARMUP_INTERVAL_MS, strategy.dst_warmup_interval_ms);
                    eular::ntrs::messageAddU32ByTag(&notify, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)peer_session.expire_at_sec);
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::SRC_PEER_ID, src.c_str());
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::DST_PEER_ID, dst.c_str());
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_ID,
                                                       src_it->second.peer_id.c_str());
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_LOCAL_IP,
                                                       src_it->second.local_ip.c_str());
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_LOCAL_PORT,
                                                    src_it->second.local_port);
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_IP,
                                                       src_it->second.srflx_ip.c_str());
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_PORT,
                                                    src_it->second.srflx_port);
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_IP_2,
                                                       src_it->second.srflx_ip_2.c_str());
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_PORT_2,
                                                    src_it->second.srflx_port_2);
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_NAT_CLASS,
                                                    src_it->second.nat_class);
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_NAT_FLAGS,
                                                    src_it->second.nat_flags);
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_MAPPING_BEHAVIOR,
                                                    src_it->second.mapping_behavior);
                    eular::ntrs::messageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_FILTERING_BEHAVIOR,
                                                    src_it->second.filtering_behavior);
                    eular::ntrs::messageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_NAT_TYPE,
                                                       src_it->second.nat_type.c_str());
                    send_message(dst_it->second.fd, notify);
                    break;
                }
                case eular::ntrs::MessageType::NAT_PROBE_REQ: {
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (!auth_manager.validateSession(fd, "", session_token, (uint64_t)time(NULL), &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    AsyncFederationJob job;
                    job.type = AsyncFederationJobType::FETCH_STUN;
                    job.fd = fd;
                    job.client_generation = client_generations[fd];
                    job.request_id = msg.request_id;
                    job.controls =
                        build_probe_controls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint);
                    job.auth_secret = auth_secret;
                    job.federation_peer_id = "service_node_federation";
                    {
                        AsyncFederationResult result;
                        if (!start_federation_job(job, &federation_requests, &result, federation_dns_base)) {
                            async_results.push_back(result);
                        }
                    }
                    break;
                }
                case eular::ntrs::MessageType::FILTER_PROBE_REQ: {
                    std::string target_ip = msg_str_tag(msg, eular::ntrs::FieldTag::TARGET_IP);
                    uint16_t    target_port = msg_u16_tag(msg, eular::ntrs::FieldTag::TARGET_PORT);
                    std::string token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::SESSION_ID);
                    std::string peer_ip;
                    std::string owner_peer_id;
                    std::string reason;
                    if (target_ip.empty() || target_port == 0 || token.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "target_ip/target_port/token required");
                        break;
                    }
                    node_verbose_log(verbose, "FILTER_PROBE_REQ fd=%d req=%u target=%s:%u token=%s\n", fd,
                                     msg.request_id, target_ip.c_str(), (unsigned)target_port, token.c_str());
                    if (!auth_manager.validateSession(fd, "", session_token, (uint64_t)time(NULL), &reason)) {
                        printf("FILTER_PROBE_REQ auth failed fd=%d req=%u reason=%s\n", fd, msg.request_id,
                               reason.c_str());
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    if (!peer_ipv4_string(fd, &peer_ip) || target_ip != peer_ip) {
                        node_verbose_log(verbose,
                                         "FILTER_PROBE_REQ scope mismatch fd=%d req=%u peer_ip=%s target_ip=%s\n", fd,
                                         msg.request_id, peer_ip.c_str(), target_ip.c_str());
                        send_error(fd, msg.request_id, "TARGET_SCOPE_MISMATCH", "target_ip must match control peer IP");
                        break;
                    }
                    if (!auth_manager.sessionPeerId(fd, &owner_peer_id)) {
                        node_verbose_log(verbose, "FILTER_PROBE_REQ missing session peer fd=%d req=%u\n", fd,
                                         msg.request_id);
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", "session peer missing");
                        break;
                    }

                    bool same_ip_diff_port = false;
                    if (stun_alt_fd >= 0) {
                        same_ip_diff_port =
                            send_udp_filter_probe(stun_alt_fd, target_ip, target_port, token, "same_ip_diff_port");
                    }
                    node_verbose_log(verbose,
                                     "FILTER_PROBE_REQ local send fd=%d req=%u same_ip_diff_port_sent=%s alt_fd=%d\n",
                                     fd, msg.request_id, same_ip_diff_port ? "true" : "false", stun_alt_fd);

                    uint64_t    probe_expire_at = (uint64_t)time(NULL) + 5;
                    std::string probe_auth = eular::ntrs::mintProbeAuthorization(auth_secret, owner_peer_id, target_ip,
                                                                                 target_port, token, probe_expire_at);
                    AsyncFederationJob job;
                    job.type = AsyncFederationJobType::SEND_PROBE;
                    job.fd = fd;
                    job.client_generation = client_generations[fd];
                    job.request_id = msg.request_id;
                    job.controls =
                        build_probe_controls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint);
                    job.auth_secret = auth_secret;
                    job.federation_peer_id = "service_node_federation";
                    job.target_ip = target_ip;
                    job.target_port = target_port;
                    job.probe_token = token;
                    job.owner_peer_id = owner_peer_id;
                    job.probe_expire_at = probe_expire_at;
                    job.probe_auth = probe_auth;
                    job.same_ip_diff_port = same_ip_diff_port;
                    {
                        AsyncFederationResult result;
                        if (!start_federation_job(job, &federation_requests, &result, federation_dns_base)) {
                            node_verbose_log(verbose,
                                             "FILTER_PROBE_REQ federation start failed fd=%d req=%u selected=%s\n",
                                             fd, msg.request_id, result.selected_control.c_str());
                            async_results.push_back(result);
                        } else {
                            node_verbose_log(verbose, "FILTER_PROBE_REQ federation started fd=%d req=%u controls=%zu\n",
                                             fd, msg.request_id, job.controls.size());
                        }
                    }
                    break;
                }
                case eular::ntrs::MessageType::SERVER_INFO_REQ: {
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (!auth_manager.validateSession(fd, "service_node_federation", session_token,
                                                      (uint64_t)time(NULL), &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::SERVER_INFO_RSP, msg.request_id);
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::STUN_ENDPOINT,
                                                       self_stun_endpoint.c_str());
                    send_message(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::SERVER_SEND_PROBE_REQ: {
                    std::string target_ip = msg_str_tag(msg, eular::ntrs::FieldTag::TARGET_IP);
                    uint16_t    target_port = msg_u16_tag(msg, eular::ntrs::FieldTag::TARGET_PORT);
                    std::string token = msg_str_tag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string owner_peer_id = msg_str_tag(msg, eular::ntrs::FieldTag::SRC_PEER_ID);
                    std::string probe_auth = msg_str_tag(msg, eular::ntrs::FieldTag::QUERY);
                    uint32_t    expire_at = msg_u32_tag(msg, eular::ntrs::FieldTag::EXPIRE_AT);
                    std::string session_token = msg_str_tag(msg, eular::ntrs::FieldTag::SESSION_ID);
                    std::string reason;
                    if (!auth_manager.validateSession(fd, "service_node_federation", session_token,
                                                      (uint64_t)time(NULL), &reason)) {
                        node_verbose_log(verbose, "SERVER_SEND_PROBE_REQ auth failed fd=%d req=%u reason=%s\n", fd,
                                         msg.request_id, reason.c_str());
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    if ((uint64_t)expire_at <= (uint64_t)time(NULL) ||
                        !eular::ntrs::validateProbeAuthorization(auth_secret, owner_peer_id, target_ip, target_port,
                                                                 token, expire_at, probe_auth)) {
                        node_verbose_log(
                            verbose,
                            "SERVER_SEND_PROBE_REQ authorization invalid fd=%d req=%u target=%s:%u token=%s\n", fd,
                            msg.request_id, target_ip.c_str(), (unsigned)target_port, token.c_str());
                        send_error(fd, msg.request_id, "PROBE_AUTH_INVALID", "probe authorization invalid");
                        break;
                    }
                    bool ok = send_udp_filter_probe(stun_fd, target_ip, target_port, token, "diff_ip");
                    node_verbose_log(verbose,
                                     "SERVER_SEND_PROBE_REQ fd=%d req=%u target=%s:%u token=%s diff_ip_sent=%s\n",
                                     fd, msg.request_id, target_ip.c_str(), (unsigned)target_port, token.c_str(),
                                     ok ? "true" : "false");

                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::SERVER_SEND_PROBE_RSP, msg.request_id);
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                              (uint8_t)(ok ? eular::ntrs::ResultCode::OK
                                                           : eular::ntrs::ResultCode::FAILED));
                    send_message(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::SERVER_STUN_REQ: {
                    std::string            target_ip = msg_str_tag(msg, eular::ntrs::FieldTag::TARGET_IP);
                    uint16_t               target_port = msg_u16_tag(msg, eular::ntrs::FieldTag::TARGET_PORT);
                    std::string            session_token = msg_str_tag(msg, eular::ntrs::FieldTag::SESSION_ID);
                    const uint8_t*         txid_data = NULL;
                    uint16_t               txid_len = 0;
                    bool                   use_alt_port = msg_bool_tag(msg, eular::ntrs::FieldTag::USE_ALT_PORT);
                    std::string            reason;
                    struct sockaddr_in     target_addr;
                    int                    send_fd = use_alt_port ? stun_alt_fd : stun_fd;
                    uint16_t               response_port = use_alt_port ? stun_alt_bind_port : self_stun_port;

                    if (!auth_manager.validateSession(fd, "service_node_federation", session_token,
                                                      (uint64_t)time(NULL), &reason)) {
                        send_error(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    if (target_ip.empty() || target_port == 0 ||
                        !eular::ntrs::messageGetBytesByTag(&msg, eular::ntrs::FieldTag::STUN_TXID, &txid_data,
                                                           &txid_len) ||
                        txid_len != STUN_TRX_ID_SIZE || send_fd < 0) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "target/txid required");
                        break;
                    }

                    memset(&target_addr, 0, sizeof(target_addr));
                    target_addr.sin_family = AF_INET;
                    target_addr.sin_port = htons(target_port);
                    if (inet_pton(AF_INET, target_ip.c_str(), &target_addr.sin_addr) != 1) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "invalid target_ip");
                        break;
                    }

                    bool ok = send_stun_binding_response(send_fd, &target_addr, txid_data, &target_addr, self_stun_ip,
                                                         response_port, self_stun_ip,
                                                         use_alt_port ? self_stun_port : stun_alt_bind_port);
                    eular::ntrs::Message rsp;
                    eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::SERVER_STUN_RSP, msg.request_id);
                    eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                              (uint8_t)(ok ? eular::ntrs::ResultCode::OK
                                                           : eular::ntrs::ResultCode::FAILED));
                    send_message(fd, rsp);
                    break;
                }
                default:
                    send_error(fd, msg.request_id, "UNSUPPORTED", "message unsupported in M1");
                    break;
                }
                if (std::find(closed.begin(), closed.end(), fd) != closed.end()) {
                    break;
                }
            }
        }

        for (size_t i = 0; i < closed.size(); ++i) {
            int fd = closed[i];
            close(fd);
            clients.erase(fd);
            client_generations.erase(fd);
            client_rx_states.erase(fd);
            erase_peer_for_fd(&peers, fd);
            auth_manager.revokeFd(fd);
            for (std::list<FederationRequest>::iterator fit = federation_requests.begin();
                 fit != federation_requests.end();) {
                if (fit->job.fd == fd) {
                    close_federation_attempt(&*fit);
                    fit = federation_requests.erase(fit);
                } else {
                    ++fit;
                }
            }
        }

        for (std::list<FederationRequest>::iterator it = federation_requests.begin();
             it != federation_requests.end();) {
            int                   fd = it->fd;
            bool                  readable = fd >= 0 && FD_ISSET(fd, &rfds);
            bool                  writable = fd >= 0 && FD_ISSET(fd, &wfds);
            AsyncFederationResult result;
            if (advance_federation_request(&*it, readable, writable, &result, federation_dns_base)) {
                async_results.push_back(result);
                it = federation_requests.erase(it);
            } else {
                ++it;
            }
        }

        for (;;) {
            AsyncFederationResult result;
            bool                  have_result = false;
            if (!async_results.empty()) {
                result = async_results.front();
                async_results.pop_front();
                have_result = true;
            }
            if (!have_result) {
                break;
            }

            if (clients.find(result.fd) == clients.end()) {
                continue;
            }
            if (client_generations.find(result.fd) == client_generations.end() ||
                client_generations[result.fd] != result.client_generation) {
                continue;
            }

            eular::ntrs::Message rsp;
            if (result.type == AsyncFederationJobType::FETCH_STUN) {
                eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::NAT_PROBE_RSP, result.request_id);
                eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::STUN1, self_stun_endpoint.c_str());
                if (result.ok) {
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::SELECTED_CONTROL,
                                                       result.selected_control.c_str());
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::STUN2, result.peer_stun.c_str());
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::FEDERATION, "ok");
                } else {
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::STUN2, "");
                    eular::ntrs::messageAddStringByTag(&rsp, eular::ntrs::FieldTag::FEDERATION, "degraded");
                }
                send_message(result.fd, rsp);
                continue;
            }

            if (result.type == AsyncFederationJobType::SEND_PROBE) {
                printf(
                    "FILTER_PROBE_RSP fd=%d req=%u result=%s same_ip_diff_port_sent=%s diff_ip_sent=%s selected=%s "
                    "error=%s\n",
                    result.fd, result.request_id, (result.same_ip_diff_port || result.diff_ip) ? "ok" : "degraded",
                    result.same_ip_diff_port ? "true" : "false", result.diff_ip ? "true" : "false",
                    result.selected_control.c_str(), result.ok ? "none" : "federation_failed");
                eular::ntrs::messageInit(&rsp, eular::ntrs::MessageType::FILTER_PROBE_RSP, result.request_id);
                eular::ntrs::messageAddBoolByTag(&rsp, eular::ntrs::FieldTag::SAME_IP_DIFF_PORT_SENT,
                                                 result.same_ip_diff_port);
                eular::ntrs::messageAddBoolByTag(&rsp, eular::ntrs::FieldTag::DIFF_IP_SENT, result.diff_ip);
                eular::ntrs::messageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                          (uint8_t)((result.same_ip_diff_port || result.diff_ip)
                                                        ? eular::ntrs::ResultCode::OK
                                                        : eular::ntrs::ResultCode::DEGRADED));
                send_message(result.fd, rsp);
            }
        }
    }

    for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
        close(*it);
    }
    for (std::list<FederationRequest>::iterator it = federation_requests.begin(); it != federation_requests.end();
         ++it) {
        close_federation_attempt(&*it);
    }
    evdns_base_free(federation_dns_base, 1);
    event_base_free(federation_event_base);
    close(listen_fd);
    close(stun_fd);
    if (stun_alt_fd >= 0) {
        close(stun_alt_fd);
    }

    if (use_hub_assignment && mqtt_client) {
        publish_presence(mqtt_client.get(), node_id, boot_id, "offline", "graceful_shutdown", 3);
        mqtt_client->disconnect();
    }

    return 0;
}
