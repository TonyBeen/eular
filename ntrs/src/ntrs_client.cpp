#include <netdb.h>
#include <ntrs_binary_protocol.h>
#include <ntrs_client.h>
#include <ntrs_codec.h>
#include <ntrs_io.h>
#include <socket_address.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/util.h>
#if defined(__linux__)
#include <net/if.h>
#include <sys/ioctl.h>
#endif
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

static uint64_t  g_ntrs_req = 1;
static const int kControlIoTimeoutMs = 2000;
static bool      SetNonblocking(int fd);
static bool      RefreshLocalEndpointForRemote(int sock, const struct sockaddr* remote_addr, socklen_t remote_len,
                                                   std::string* ip, uint16_t* port);
static bool      LocalEndpointIsExplicitBound(const struct sockaddr_storage* local_addr, socklen_t local_len);
static bool      SockaddrEndpoint(const struct sockaddr_storage* addr, socklen_t len, std::string* ip,
                                   uint16_t* port);

static uint64_t CurrentTimeMs()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000ull);
}

static int SocketLastError()
{
    return EVUTIL_SOCKET_ERROR();
}

static bool SocketErrWouldBlock(int err)
{
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == EAGAIN;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

static bool SocketErrInterrupted(int err)
{
#if defined(_WIN32)
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

static bool SocketErrRwRetriable(int err)
{
    return SocketErrWouldBlock(err) || SocketErrInterrupted(err);
}

static bool SocketErrConnectRetriable(int err)
{
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS || err == WSAEINVAL;
#else
    return err == EINTR || err == EINPROGRESS;
#endif
}

struct NatSample {
    std::string               local_ip;
    uint16_t                  local_port;
    std::string               srflx_ip;
    uint16_t                  srflx_port;
    std::string               srflx_ip_2;
    uint16_t                  srflx_port_2;
    bool                      mapping_stable;
    std::string               nat_risk;
    bool                      probe1_ok;
    bool                      probe2_ok;
    int32_t                   probe1_rtt_ms;
    int32_t                   probe2_rtt_ms;
    int32_t                   probe_rounds;
    int32_t                   probe1_success_count;
    int32_t                   probe2_success_count;
    int32_t                   probe1_distinct_mappings;
    int32_t                   probe2_distinct_mappings;
    ntrs_nat_class_t          nat_class;
    ntrs_nat_flags_t          nat_flags;
    ntrs_mapping_behavior_t   mapping_behavior;
    ntrs_filtering_behavior_t filtering_behavior;
    std::string               nat_type;
    bool                      filter_probe_executed;
    bool                      filter_same_ip_diff_port_rx;
    bool                      filter_diff_ip_rx;
};

struct StunResponseInfo {
    std::string mapped_ip;
    uint16_t    mapped_port;
    std::string response_origin_ip;
    uint16_t    response_origin_port;
    std::string other_address_ip;
    uint16_t    other_address_port;
};

static const uint32_t kStunChangeIp = 0x04u;
static const uint32_t kStunChangePort = 0x02u;
static const int      kAsyncFilterMaxAttempts = 2;

enum class AsyncNatPhase {
    PROBE1 = 0,
    PROBE2,
    FILTER_CHANGE_PORT,
    FILTER_CHANGE_IP,
};

static const char* AsyncNatPhaseName(AsyncNatPhase phase)
{
    switch (phase) {
    case AsyncNatPhase::PROBE1:
        return "probe1";
    case AsyncNatPhase::PROBE2:
        return "probe2";
    case AsyncNatPhase::FILTER_CHANGE_PORT:
        return "change_port";
    case AsyncNatPhase::FILTER_CHANGE_IP:
        return "change_ip";
    default:
        return "unknown";
    }
}

static bool IsIpv4Text(const std::string& ip)
{
    struct in_addr addr;
    return !ip.empty() && inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

static bool IsIpv6Text(const std::string& ip)
{
    struct in6_addr addr;
    return !ip.empty() && inet_pton(AF_INET6, ip.c_str(), &addr) == 1;
}

static bool IsPublicIpv4(const std::string& ip)
{
    struct in_addr addr;
    uint32_t       value = 0;

    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        return false;
    }

    value = ntohl(addr.s_addr);
    if ((value & 0xFF000000u) == 0x0A000000u) {
        return false;
    }
    if ((value & 0xFFF00000u) == 0xAC100000u) {
        return false;
    }
    if ((value & 0xFFFF0000u) == 0xC0A80000u) {
        return false;
    }
    if ((value & 0xFFC00000u) == 0x64400000u) {
        return false;
    }
    if ((value & 0xFF000000u) == 0x7F000000u) {
        return false;
    }
    if ((value & 0xFFFF0000u) == 0xA9FE0000u) {
        return false;
    }
    if ((value & 0xF0000000u) == 0xE0000000u) {
        return false;
    }
    if (value == 0u || value == 0xFFFFFFFFu) {
        return false;
    }
    return true;
}

static const char* NatClassName(ntrs_nat_class_t nat_class)
{
    switch (nat_class) {
    case NTRS_NAT_CLASS_OPEN_PUBLIC:
        return "open_public";
    case NTRS_NAT_CLASS_FULL_CONE:
        return "full_cone_nat";
    case NTRS_NAT_CLASS_IP_RESTRICTED:
        return "ip_restricted_nat";
    case NTRS_NAT_CLASS_PORT_RESTRICTED:
        return "port_restricted_nat";
    case NTRS_NAT_CLASS_SYMMETRIC:
        return "symmetric_nat";
    default:
        return "unknown";
    }
}

static void CopyText(char* dst, size_t dst_len, const std::string& src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }

    snprintf(dst, dst_len, "%s", src.c_str());
}

static void CopyText(char* dst, size_t dst_len, const char* src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }

    snprintf(dst, dst_len, "%s", src == NULL ? "" : src);
}

static const char* MsgStrTag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag)
{
    const char* value = eular::ntrs::MessageGetStringByTag(&msg, tag);
    return value == NULL ? "" : value;
}

static uint16_t MsgU16Tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, uint16_t default_value = 0)
{
    uint16_t value = default_value;
    if (eular::ntrs::MessageGetU16ByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static uint32_t MsgU32Tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, uint32_t default_value = 0)
{
    uint32_t value = default_value;
    if (eular::ntrs::MessageGetU32ByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static bool SendAll(int fd, const void* buf, size_t len)
{
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t         left = len;

    while (left > 0) {
        ssize_t nsend = send(fd, ptr, left, 0);
        if (nsend <= 0) {
            return false;
        }

        ptr += nsend;
        left -= (size_t)nsend;
    }

    return true;
}

static bool SendMessage(int fd, const eular::ntrs::Message& msg)
{
    uint8_t buffer[8192];
    size_t  encoded_len = 0;

    if (eular::ntrs::EncodeMessage(msg, buffer, sizeof(buffer), &encoded_len) != 0) {
        return false;
    }

    return SendAll(fd, buffer, encoded_len);
}

static bool RecvMessage(int fd, eular::ntrs::Message* msg)
{
    return eular::ntrs::RecvMessageWithTimeout(fd, kControlIoTimeoutMs, msg);
}

static bool RecvMessageTimeout(int fd, int timeout_ms, eular::ntrs::Message* msg)
{
    fd_set         rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
        return false;
    }

    return RecvMessage(fd, msg);
}

static void EvaluateNatResult(NatSample* nat, bool has_probe2)
{
    bool local_public = false;
    bool same_mapping = false;
    bool unstable_mapping = false;
    bool filter_probe_has_evidence = false;
    bool ipv6_sample = false;

    if (nat == NULL) {
        return;
    }

    nat->nat_class = NTRS_NAT_CLASS_UNKNOWN;
    nat->nat_flags = NTRS_NAT_FLAG_NONE;
    nat->mapping_behavior = NTRS_MAPPING_UNKNOWN;
    nat->filtering_behavior = NTRS_FILTERING_UNKNOWN;

    ipv6_sample = IsIpv6Text(nat->local_ip) || IsIpv6Text(nat->srflx_ip) || IsIpv6Text(nat->srflx_ip_2);
    if (ipv6_sample) {
        if (!nat->probe1_ok) {
            nat->mapping_stable = false;
            nat->nat_risk = "high";
            nat->nat_flags |= NTRS_NAT_FLAG_UDP_BLOCKED | NTRS_NAT_FLAG_PROBE_DEGRADED;
            nat->filtering_behavior = NTRS_FILTERING_BLOCKED;
            nat->nat_type = "ipv6_timeout";
            return;
        }

        nat->mapping_stable = true;
        nat->mapping_behavior = NTRS_MAPPING_UNKNOWN;
        nat->filtering_behavior = NTRS_FILTERING_UNKNOWN;
        nat->nat_class = NTRS_NAT_CLASS_UNKNOWN;
        nat->nat_risk = "medium";
        nat->nat_type = "ipv6_reachable";
        if (nat->filter_probe_executed && !nat->filter_same_ip_diff_port_rx && !nat->filter_diff_ip_rx) {
            nat->filtering_behavior = NTRS_FILTERING_ADDRESS_AND_PORT_DEPENDENT;
            nat->nat_risk = "high";
            nat->nat_type = "ipv6_filtered";
        }
        if (nat->probe1_success_count < nat->probe_rounds ||
            (has_probe2 && nat->probe2_success_count < nat->probe_rounds)) {
            nat->nat_flags |= NTRS_NAT_FLAG_PROBE_DEGRADED;
        }
        return;
    }

    local_public = IsPublicIpv4(nat->local_ip);
    same_mapping = nat->srflx_ip == nat->srflx_ip_2 && nat->srflx_port == nat->srflx_port_2;
    unstable_mapping = nat->probe1_distinct_mappings > 1 || nat->probe2_distinct_mappings > 1;
    filter_probe_has_evidence = nat->filter_same_ip_diff_port_rx || nat->filter_diff_ip_rx;

    if (local_public && nat->local_ip == nat->srflx_ip && nat->local_port == nat->srflx_port) {
        nat->nat_flags |= NTRS_NAT_FLAG_LOCAL_ADDR_PUBLIC;
    }
    if (nat->probe2_ok && nat->srflx_ip != nat->srflx_ip_2 && IsIpv4Text(nat->srflx_ip) &&
        IsIpv4Text(nat->srflx_ip_2)) {
        nat->nat_flags |= NTRS_NAT_FLAG_MULTI_EXTERNAL_IP;
    }

    if (!nat->probe1_ok) {
        nat->mapping_stable = false;
        nat->nat_risk = "high";
        nat->nat_flags |= NTRS_NAT_FLAG_UDP_BLOCKED | NTRS_NAT_FLAG_PROBE_DEGRADED;
        nat->filtering_behavior = NTRS_FILTERING_BLOCKED;
        nat->nat_type = NatClassName(nat->nat_class);
        return;
    }

    if (!has_probe2) {
        nat->mapping_stable = true;
        nat->nat_flags |= NTRS_NAT_FLAG_PROBE_DEGRADED;
        nat->mapping_behavior = NTRS_MAPPING_UNKNOWN;
        nat->filtering_behavior = NTRS_FILTERING_UNKNOWN;
        nat->nat_class = local_public ? NTRS_NAT_CLASS_OPEN_PUBLIC : NTRS_NAT_CLASS_UNKNOWN;
        nat->nat_risk = local_public ? "medium" : "high";
        nat->nat_type = NatClassName(nat->nat_class);
        return;
    }

    if (!nat->probe2_ok) {
        nat->mapping_stable = false;
        nat->nat_flags |= NTRS_NAT_FLAG_MAPPING_UNSTABLE;
        nat->nat_flags |= NTRS_NAT_FLAG_PROBE_DEGRADED;
        nat->mapping_behavior = NTRS_MAPPING_UNKNOWN;
        nat->filtering_behavior = NTRS_FILTERING_UNKNOWN;
        nat->nat_class = NTRS_NAT_CLASS_UNKNOWN;
        nat->nat_risk = "high";
        nat->nat_type = NatClassName(nat->nat_class);
        return;
    }

    nat->mapping_stable = same_mapping;
    if (!nat->mapping_stable || unstable_mapping) {
        nat->nat_flags |= NTRS_NAT_FLAG_MAPPING_UNSTABLE;
    }

    if (unstable_mapping) {
        nat->mapping_behavior = NTRS_MAPPING_UNSTABLE;
    } else if (same_mapping) {
        nat->mapping_behavior = NTRS_MAPPING_ENDPOINT_INDEPENDENT;
    } else {
        nat->mapping_behavior = NTRS_MAPPING_ADDRESS_DEPENDENT;
    }

    if (!same_mapping || unstable_mapping) {
        nat->filtering_behavior = NTRS_FILTERING_UNKNOWN;
    } else if (nat->filter_same_ip_diff_port_rx && nat->filter_diff_ip_rx) {
        nat->filtering_behavior = NTRS_FILTERING_ENDPOINT_INDEPENDENT;
    } else if (nat->filter_same_ip_diff_port_rx && !nat->filter_diff_ip_rx) {
        nat->filtering_behavior = NTRS_FILTERING_ADDRESS_DEPENDENT;
    } else if (nat->filter_probe_executed && !filter_probe_has_evidence) {
        nat->filtering_behavior = NTRS_FILTERING_ADDRESS_AND_PORT_DEPENDENT;
    } else {
        nat->filtering_behavior = NTRS_FILTERING_UNKNOWN;
        if (nat->filter_probe_executed) {
            nat->nat_flags |= NTRS_NAT_FLAG_PROBE_DEGRADED;
        }
    }

    if (local_public && nat->local_ip == nat->srflx_ip && nat->local_port == nat->srflx_port &&
        nat->mapping_behavior == NTRS_MAPPING_ENDPOINT_INDEPENDENT && !unstable_mapping) {
        nat->nat_risk = "low";
        nat->nat_class = NTRS_NAT_CLASS_OPEN_PUBLIC;
    } else if (nat->mapping_behavior == NTRS_MAPPING_UNSTABLE ||
               nat->mapping_behavior == NTRS_MAPPING_ADDRESS_DEPENDENT ||
               nat->mapping_behavior == NTRS_MAPPING_ADDRESS_AND_PORT_DEPENDENT) {
        nat->nat_risk = "high";
        nat->nat_class = NTRS_NAT_CLASS_SYMMETRIC;
    } else if (nat->filtering_behavior == NTRS_FILTERING_ENDPOINT_INDEPENDENT) {
        nat->nat_risk = "low";
        nat->nat_class = NTRS_NAT_CLASS_FULL_CONE;
    } else if (nat->filtering_behavior == NTRS_FILTERING_ADDRESS_DEPENDENT) {
        nat->nat_risk = "medium";
        nat->nat_class = NTRS_NAT_CLASS_IP_RESTRICTED;
    } else if (nat->filtering_behavior == NTRS_FILTERING_ADDRESS_AND_PORT_DEPENDENT) {
        nat->nat_risk = "high";
        nat->nat_class = NTRS_NAT_CLASS_PORT_RESTRICTED;
    } else {
        nat->nat_risk = "high";
        nat->nat_class = NTRS_NAT_CLASS_UNKNOWN;
    }

    if (nat->probe1_success_count < nat->probe_rounds || (has_probe2 && nat->probe2_success_count < nat->probe_rounds)) {
        nat->nat_flags |= NTRS_NAT_FLAG_PROBE_DEGRADED;
    }

    nat->nat_type = NatClassName(nat->nat_class);
}

static void FillNatInfo(ntrs_nat_info_t* out, const NatSample& sample)
{
    if (out == NULL) {
        return;
    }

    CopyText(out->local_ip, sizeof(out->local_ip), sample.local_ip);
    out->local_port = sample.local_port;
    CopyText(out->srflx_ip, sizeof(out->srflx_ip), sample.srflx_ip);
    out->srflx_port = sample.srflx_port;
    CopyText(out->srflx_ip_2, sizeof(out->srflx_ip_2), sample.srflx_ip_2);
    out->srflx_port_2 = sample.srflx_port_2;
    out->mapping_stable = sample.mapping_stable;
    CopyText(out->nat_risk, sizeof(out->nat_risk), sample.nat_risk);
    out->probe1_ok = sample.probe1_ok;
    out->probe2_ok = sample.probe2_ok;
    out->probe1_rtt_ms = sample.probe1_rtt_ms;
    out->probe2_rtt_ms = sample.probe2_rtt_ms;
    out->probe_rounds = sample.probe_rounds;
    out->probe1_success_count = sample.probe1_success_count;
    out->probe2_success_count = sample.probe2_success_count;
    out->probe1_distinct_mappings = sample.probe1_distinct_mappings;
    out->probe2_distinct_mappings = sample.probe2_distinct_mappings;
    out->nat_class = sample.nat_class;
    out->nat_flags = sample.nat_flags;
    out->mapping_behavior = sample.mapping_behavior;
    out->filtering_behavior = sample.filtering_behavior;
    CopyText(out->nat_type, sizeof(out->nat_type), sample.nat_type);
    out->filter_same_ip_diff_port_rx = sample.filter_same_ip_diff_port_rx;
    out->filter_diff_ip_rx = sample.filter_diff_ip_rx;
}

static bool ParseSessionSignalImpl(const eular::ntrs::Message& msg, ntrs_session_signal_t* out)
{
    uint32_t count = 0;
    const char* peer_local_ip = NULL;
    uint16_t    peer_local_port = 0;
    const char* peer_srflx_ip = NULL;
    uint16_t    peer_srflx_port = 0;
    const char* peer_srflx_ip_2 = NULL;
    uint16_t    peer_srflx_port_2 = 0;

    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    CopyText(out->peer_id, sizeof(out->peer_id), eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::PEER_ID));
    CopyText(out->peer_device_id, sizeof(out->peer_device_id),
              eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::PEER_DEVICE_ID));
    out->peer_nat_class =
        MsgU16Tag(msg, eular::ntrs::FieldTag::PEER_NAT_CLASS, NTRS_NAT_CLASS_UNKNOWN);
    out->peer_nat_flags =
        MsgU16Tag(msg, eular::ntrs::FieldTag::PEER_NAT_FLAGS, NTRS_NAT_FLAG_NONE);
    out->peer_mapping_behavior =
        MsgU16Tag(msg, eular::ntrs::FieldTag::PEER_MAPPING_BEHAVIOR, NTRS_MAPPING_UNKNOWN);
    out->peer_filtering_behavior =
        MsgU16Tag(msg, eular::ntrs::FieldTag::PEER_FILTERING_BEHAVIOR, NTRS_FILTERING_UNKNOWN);
    CopyText(out->peer_nat_type, sizeof(out->peer_nat_type),
              eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::PEER_NAT_TYPE));
    CopyText(out->session_id, sizeof(out->session_id),
              eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::SESSION_ID));
    CopyText(out->peer_session_token, sizeof(out->peer_session_token),
              eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::TOKEN));
    eular::ntrs::MessageGetU8ByTag(&msg, eular::ntrs::FieldTag::PUNCH_ORDER, &out->punch_order);
    eular::ntrs::MessageGetU8ByTag(&msg, eular::ntrs::FieldTag::CONNECT_ROLE, &out->connect_role);
    eular::ntrs::MessageGetU32ByTag(&msg, eular::ntrs::FieldTag::WARMUP_ROUNDS, &out->warmup_rounds);
    eular::ntrs::MessageGetU32ByTag(&msg, eular::ntrs::FieldTag::WARMUP_INTERVAL_MS, &out->warmup_interval_ms);
    eular::ntrs::MessageGetU32ByTag(&msg, eular::ntrs::FieldTag::EXPIRE_AT, &out->expire_at);

    peer_local_ip = eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::PEER_LOCAL_IP);
    eular::ntrs::MessageGetU16ByTag(&msg, eular::ntrs::FieldTag::PEER_LOCAL_PORT, &peer_local_port);
    peer_srflx_ip = eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::PEER_SRFLX_IP);
    eular::ntrs::MessageGetU16ByTag(&msg, eular::ntrs::FieldTag::PEER_SRFLX_PORT, &peer_srflx_port);
    peer_srflx_ip_2 = eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::PEER_SRFLX_IP_2);
    eular::ntrs::MessageGetU16ByTag(&msg, eular::ntrs::FieldTag::PEER_SRFLX_PORT_2, &peer_srflx_port_2);

    if (count < NTRS_MAX_CANDIDATES && peer_local_ip[0] != '\0' && peer_local_port > 0) {
        CopyText(out->candidates[count].ip, sizeof(out->candidates[count].ip), peer_local_ip);
        out->candidates[count].port = peer_local_port;
        CopyText(out->candidates[count].type, sizeof(out->candidates[count].type), "host_local");
        ++count;
    }

    if (peer_srflx_ip[0] != '\0' && peer_srflx_port > 0) {
        CopyText(out->candidates[count].ip, sizeof(out->candidates[count].ip), peer_srflx_ip);
        out->candidates[count].port = peer_srflx_port;
        CopyText(out->candidates[count].type, sizeof(out->candidates[count].type), "srflx_primary");
        ++count;
    }

    if (count < NTRS_MAX_CANDIDATES && peer_srflx_ip_2[0] != '\0' && peer_srflx_port_2 > 0) {
        bool duplicate_primary =
            peer_srflx_ip[0] != '\0' && peer_srflx_port > 0 && strcmp(peer_srflx_ip, peer_srflx_ip_2) == 0 &&
            peer_srflx_port == peer_srflx_port_2;
        if (!duplicate_primary) {
            CopyText(out->candidates[count].ip, sizeof(out->candidates[count].ip), peer_srflx_ip_2);
            out->candidates[count].port = peer_srflx_port_2;
            CopyText(out->candidates[count].type, sizeof(out->candidates[count].type), "srflx_secondary");
            ++count;
        }
    }

    out->candidate_count = count;
    return count > 0;
}

struct AsyncRequest;

struct AsyncClientImpl {
    event_base*                       base;
    evdns_base*                       dns_base;
    uint64_t                          next_request_id;
    std::map<uint64_t, AsyncRequest*> requests;
    std::map<int32_t, uint64_t>       active_fds;

    explicit AsyncClientImpl(event_base* event_base) : base(event_base), dns_base(NULL), next_request_id(1) {}
};

struct AsyncRequest {
    AsyncClientImpl*                     client;
    uint64_t                             request_id;
    ntrs_async_request_type_t            type;
    int32_t                              fd;
    bool                                 expect_response;
    eular::ntrs::MessageType             expected_type;
    std::vector<uint8_t>                 output;
    size_t                               output_offset;
    std::vector<uint8_t>                 input;
    event*                               io_event;
    event*                               timeout_event;
    ntrs_async_callback_t                callback;
    void*                                user_data;
    ntrs_async_result_t                  result;
    NatSample                            nat_sample;
    struct sockaddr_storage              probe_addrs[2];
    socklen_t                            probe_addr_lens[2];
    int                                  udp_family;
    struct evdns_getaddrinfo_request*    dns_request;
    int                                  resolving_probe_index;
    std::string                          probe_hosts[2];
    uint16_t                             probe_ports[2];
    std::vector<ntrs_peer_candidate_t>   punch_candidates;
    std::vector<struct sockaddr_storage> punch_addrs;
    std::vector<socklen_t>               punch_addr_lens;
    int                                  punch_round;
    int                                  punch_max_rounds;
    int                                  punch_interval_ms;
    std::string                          connect_host;
    uint16_t                             connect_port;
    int                                  connect_timeout_ms;
    struct evutil_addrinfo*              connect_addrs;
    struct evutil_addrinfo*              connect_next_addr;
    int32_t                              nat_control_fd;
    std::string                          nat_session_token;
    bool                                 nat_enable_filter_probe;
    bool                                 nat_verbose;
    bool                                 nat_explicit_bind;
    bool                                 has_probe2;
    AsyncNatPhase                        nat_phase;
    int                                  probe_index;
    int                                  probe_attempts;
    int                                  probe_max_attempts;
    int                                  probe_phase_max_attempts;
    int                                  probe_success_target;
    int                                  probe_success_count[2];
    long                                 probe_rtt_sum_ms[2];
    std::set<std::string>                probe_mappings[2];
    struct timeval                       probe_sent_at;
    uint32_t                             probe_change_request;
    uint8_t                              probe_token[NTRS_PROBE_TOKEN_WIRE_SIZE];

    AsyncRequest()
        : client(NULL),
          request_id(0),
          type(NTRS_ASYNC_AUTH),
          fd(-1),
          expect_response(false),
          expected_type(eular::ntrs::MessageType::UNKNOWN),
          output_offset(0),
          io_event(NULL),
          timeout_event(NULL),
          callback(NULL),
          user_data(NULL),
          udp_family(AF_UNSPEC),
          dns_request(NULL),
          resolving_probe_index(0),
          punch_round(0),
          punch_max_rounds(0),
          punch_interval_ms(0),
          connect_port(0),
          connect_timeout_ms(0),
          connect_addrs(NULL),
          connect_next_addr(NULL),
          nat_control_fd(-1),
          nat_enable_filter_probe(false),
          nat_verbose(false),
          nat_explicit_bind(false),
          has_probe2(false),
          nat_phase(AsyncNatPhase::PROBE1),
          probe_index(0),
          probe_attempts(0),
          probe_max_attempts(0),
          probe_phase_max_attempts(0),
          probe_success_target(0),
          probe_change_request(0)
    {
        memset(&result, 0, sizeof(result));
        memset(probe_addrs, 0, sizeof(probe_addrs));
        memset(probe_addr_lens, 0, sizeof(probe_addr_lens));
        memset(probe_ports, 0, sizeof(probe_ports));
        memset(probe_success_count, 0, sizeof(probe_success_count));
        memset(probe_rtt_sum_ms, 0, sizeof(probe_rtt_sum_ms));
        memset(&probe_sent_at, 0, sizeof(probe_sent_at));
        memset(probe_token, 0, sizeof(probe_token));
    }
};

/**
 * @brief 将 NAT 探测阶段映射为私有二进制协议阶段。
 *
 * @param phase 当前异步 NAT 探测阶段。
 * @return ntrs_binary_phase_t 对应的私有协议阶段。
 */
static ntrs_binary_phase_t AsyncNatPhaseToBinaryPhase(AsyncNatPhase phase)
{
    switch (phase) {
    case AsyncNatPhase::PROBE1:
        return NTRS_BINARY_PHASE_PROBE1;
    case AsyncNatPhase::PROBE2:
        return NTRS_BINARY_PHASE_PROBE2;
    case AsyncNatPhase::FILTER_CHANGE_PORT:
        return NTRS_BINARY_PHASE_CHANGE_PORT;
    case AsyncNatPhase::FILTER_CHANGE_IP:
        return NTRS_BINARY_PHASE_CHANGE_IP;
    default:
        return NTRS_BINARY_PHASE_UNKNOWN;
    }
}

/**
 * @brief 判断当前阶段是否属于过滤探测阶段。
 *
 * @param phase 当前异步 NAT 探测阶段。
 * @return true 当前阶段为过滤探测。
 * @return false 当前阶段为常规映射探测。
 */
static bool AsyncNatPhaseIsFilterBinary(AsyncNatPhase phase)
{
    return phase == AsyncNatPhase::FILTER_CHANGE_PORT || phase == AsyncNatPhase::FILTER_CHANGE_IP;
}

/**
 * @brief 基于请求上下文生成私有探测 token。
 *
 * 当前迁移阶段继续沿用既有 12 字节 token 生成规则，以便控制面转发、过滤探测
 * 委托以及日志定位仍然能复用既有上下文；区别在于这些字节不再承载旧公开协议
 * 的 transaction-id 语义，只作为 NTRS 私有二进制协议中的探测 token。
 *
 * @param request 请求上下文。
 * @param out_token 输出 token 缓冲区，长度必须至少为 NTRS_PROBE_TOKEN_WIRE_SIZE。
 */
static void BuildAsyncNatProbeToken(const AsyncRequest* request, uint8_t out_token[NTRS_PROBE_TOKEN_WIRE_SIZE])
{
    uint64_t seed = 0;

    memset(out_token, 0, NTRS_PROBE_TOKEN_WIRE_SIZE);
    if (request == NULL) {
        return;
    }

    seed = request->request_id;
    seed ^= ((uint64_t)request->nat_phase << 40);
    seed ^= ((uint64_t)request->probe_index << 32);
    memcpy(out_token, &seed, sizeof(seed));
    out_token[8] = (uint8_t)((request->probe_change_request >> 24) & 0xFFu);
    out_token[9] = (uint8_t)((request->probe_change_request >> 16) & 0xFFu);
    out_token[10] = (uint8_t)((request->probe_change_request >> 8) & 0xFFu);
    out_token[11] = (uint8_t)(request->probe_change_request & 0xFFu);
}

/**
 * @brief 构造私有二进制 NAT 探测请求帧。
 *
 * @param request 请求上下文。
 * @param token 当前探测 token。
 * @param out 输出编码后的请求帧。
 * @return true 构造成功。
 * @return false 构造失败。
 */
static bool BuildAsyncNatProbeRequestFrame(const AsyncRequest* request, const uint8_t* token,
                                                std::vector<uint8_t>* out)
{
    uint8_t                  buffer[256];
    ntrs_binary_frame_t      frame;
    ntrs_binary_frame_type_t frame_type;
    ntrs_binary_phase_t      phase;

    if (request == NULL || token == NULL || out == NULL) {
        return false;
    }

    frame_type = AsyncNatPhaseIsFilterBinary(request->nat_phase) ? NTRS_BINARY_FRAME_FILTER_REQ
                                                                      : NTRS_BINARY_FRAME_PROBE_REQ;
    phase = AsyncNatPhaseToBinaryPhase(request->nat_phase);
    if (!ntrs_binary_frame_init(&frame, buffer, sizeof(buffer)) ||
        !ntrs_binary_frame_set_header(&frame, frame_type, phase, 0, (uint32_t)request->request_id,
                                      (uint32_t)(request->probe_attempts + 1), CurrentTimeMs()) ||
        !ntrs_binary_frame_add_tlv(&frame, NTRS_BINARY_TLV_PROBE_TOKEN, token, NTRS_PROBE_TOKEN_WIRE_SIZE)) {
        return false;
    }

    out->assign(frame.buffer, frame.buffer + frame.length);
    return true;
}

/**
 * @brief 从私有二进制 NAT 探测响应中提取关键信息。
 *
 * @param buffer 输入缓冲区。
 * @param len 输入长度。
 * @param expected_phase 期望阶段。
 * @param expected_token 期望 token。
 * @param out 输出响应信息。
 * @return true 解析成功且 token/阶段匹配。
 * @return false 解析失败或不匹配。
 */
static bool ParseAsyncNatProbeResponseFrame(const uint8_t* buffer, size_t len,
                                                 ntrs_binary_phase_t expected_phase, const uint8_t* expected_token,
                                                 StunResponseInfo* out)
{
    ntrs_binary_frame_view_t frame_view;
    ntrs_binary_tlv_view_t   token_tlv;
    ntrs_binary_tlv_view_t   tlv;
    struct sockaddr_storage  endpoint_addr;
    socklen_t                endpoint_len = 0;

    if (buffer == NULL || expected_token == NULL || out == NULL ||
        !ntrs_binary_frame_parse(buffer, len, &frame_view)) {
        return false;
    }
    if (frame_view.header.phase != (uint8_t)expected_phase) {
        return false;
    }
    if (frame_view.header.frame_type != NTRS_BINARY_FRAME_PROBE_RSP &&
        frame_view.header.frame_type != NTRS_BINARY_FRAME_FILTER_RSP) {
        return false;
    }
    if (!ntrs_binary_frame_find_tlv(&frame_view, NTRS_BINARY_TLV_PROBE_TOKEN, &token_tlv) ||
        token_tlv.value_len != NTRS_PROBE_TOKEN_WIRE_SIZE ||
        memcmp(token_tlv.value, expected_token, NTRS_PROBE_TOKEN_WIRE_SIZE) != 0) {
        return false;
    }

    *out = StunResponseInfo();
    if (!ntrs_binary_frame_find_tlv(&frame_view, NTRS_BINARY_TLV_MAPPED_ADDR, &tlv) ||
        !ntrs_binary_tlv_parse_endpoint(&tlv, &endpoint_addr, &endpoint_len) ||
        !SockaddrEndpoint(&endpoint_addr, endpoint_len, &out->mapped_ip, &out->mapped_port)) {
        return false;
    }
    if (ntrs_binary_frame_find_tlv(&frame_view, NTRS_BINARY_TLV_ORIGIN_ADDR, &tlv) &&
        ntrs_binary_tlv_parse_endpoint(&tlv, &endpoint_addr, &endpoint_len)) {
        SockaddrEndpoint(&endpoint_addr, endpoint_len, &out->response_origin_ip, &out->response_origin_port);
    }
    if (ntrs_binary_frame_find_tlv(&frame_view, NTRS_BINARY_TLV_OTHER_ADDR, &tlv) &&
        ntrs_binary_tlv_parse_endpoint(&tlv, &endpoint_addr, &endpoint_len)) {
        SockaddrEndpoint(&endpoint_addr, endpoint_len, &out->other_address_ip, &out->other_address_port);
    }
    return true;
}

static void InitAsyncResult(ntrs_async_result_t* result, uint64_t request_id, ntrs_async_request_type_t type)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->request_id = request_id;
    result->type = type;
    ntrs_nat_info_init(&result->nat_info);
}

static void SetAsyncError(ntrs_async_result_t* result, const char* message)
{
    if (result == NULL) {
        return;
    }

    result->success = false;
    CopyText(result->error_message, sizeof(result->error_message), message);
}

static bool SetNonblocking(int fd)
{
    return evutil_make_socket_nonblocking(fd) == 0;
}

static bool AsyncResultOkString(const eular::ntrs::Message& msg)
{
    uint8_t result = (uint8_t)eular::ntrs::ResultCode::UNKNOWN;
    if (!eular::ntrs::MessageGetU8ByTag(&msg, eular::ntrs::FieldTag::RESULT, &result)) {
        return true;
    }
    return result == (uint8_t)eular::ntrs::ResultCode::OK || result == (uint8_t)eular::ntrs::ResultCode::DEGRADED;
}

static bool FillAsyncResultFromMessage(AsyncRequest* request, const eular::ntrs::Message& msg)
{
    if (msg.type == eular::ntrs::MessageType::ERROR_RSP) {
        const char* code = MsgStrTag(msg, eular::ntrs::FieldTag::CODE);
        const char* message = MsgStrTag(msg, eular::ntrs::FieldTag::MESSAGE);
        char        error_buf[NTRS_MAX_TEXT_LEN];

        if (code[0] != '\0' && message[0] != '\0') {
            snprintf(error_buf, sizeof(error_buf), "%s: %s", code, message);
            SetAsyncError(&request->result, error_buf);
        } else if (message[0] != '\0') {
            SetAsyncError(&request->result, message);
        } else if (code[0] != '\0') {
            SetAsyncError(&request->result, code);
        } else {
            SetAsyncError(&request->result, "request failed");
        }
        return false;
    }

    switch (request->type) {
    case NTRS_ASYNC_AUTH:
        if (msg.type != eular::ntrs::MessageType::AUTH_RSP) {
            SetAsyncError(&request->result, "unexpected auth response");
            return false;
        }
        CopyText(request->result.session_token, sizeof(request->result.session_token),
                  MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN));
        request->result.lease_default_sec =
            MsgU32Tag(msg, eular::ntrs::FieldTag::LEASE_DEFAULT_SEC);
        request->result.success = request->result.session_token[0] != '\0';
        if (!request->result.success) {
            SetAsyncError(&request->result, "auth failed");
        }
        return request->result.success;
    case NTRS_ASYNC_REQUEST_PROBE_ENDPOINTS:
        if (msg.type != eular::ntrs::MessageType::NAT_PROBE_RSP) {
            SetAsyncError(&request->result, "unexpected probe endpoint response");
            return false;
        }
        {
            const char* probe1_value = MsgStrTag(msg, eular::ntrs::FieldTag::PROBE1);
            const char* probe2_value = MsgStrTag(msg, eular::ntrs::FieldTag::PROBE2);
            CopyText(request->result.probe1, sizeof(request->result.probe1), probe1_value);
            CopyText(request->result.probe2, sizeof(request->result.probe2), probe2_value);
        }
        request->result.success = request->result.probe1[0] != '\0';
        if (!request->result.success) {
            SetAsyncError(&request->result, "request probe endpoints failed");
        }
        return request->result.success;
    case NTRS_ASYNC_REGISTER_PEER:
        if (msg.type != eular::ntrs::MessageType::REGISTER_RSP) {
            SetAsyncError(&request->result, "unexpected register response");
            return false;
        }
        request->result.success = AsyncResultOkString(msg);
        if (!request->result.success) {
            SetAsyncError(&request->result, "register peer failed");
        }
        return request->result.success;
    case NTRS_ASYNC_CREATE_SESSION:
        if (msg.type != eular::ntrs::MessageType::SESSION_CREATE_RSP) {
            SetAsyncError(&request->result, "unexpected create session response");
            return false;
        }
        request->result.success = ParseSessionSignalImpl(msg, &request->result.session_signal);
        if (!request->result.success) {
            SetAsyncError(&request->result, "create session failed");
        }
        return request->result.success;
    case NTRS_ASYNC_WAIT_FOR_SIGNAL:
        if (msg.type != eular::ntrs::MessageType::SESSION_NOTIFY &&
            msg.type != eular::ntrs::MessageType::SESSION_CREATE_RSP) {
            SetAsyncError(&request->result, "unexpected signal response");
            return false;
        }
        request->result.success = ParseSessionSignalImpl(msg, &request->result.session_signal);
        if (!request->result.success) {
            SetAsyncError(&request->result, "wait for signal failed");
        }
        return request->result.success;
    case NTRS_ASYNC_UNREGISTER_PEER:
        request->result.success = true;
        return true;
    case NTRS_ASYNC_HEARTBEAT:
        if (msg.type != eular::ntrs::MessageType::HEARTBEAT_RSP) {
            SetAsyncError(&request->result, "unexpected heartbeat response");
            return false;
        }
        request->result.success = AsyncResultOkString(msg);
        if (!request->result.success) {
            SetAsyncError(&request->result, "heartbeat failed");
        }
        return request->result.success;
    default:
        SetAsyncError(&request->result, "unsupported async request");
        return false;
    }
}

static void AsyncRequestFinish(AsyncRequest* request, bool cancelled, const char* error_message)
{
    ntrs_async_callback_t callback = NULL;
    void*                 user_data = NULL;
    ntrs_async_result_t   result;

    if (request == NULL) {
        return;
    }

    if (error_message != NULL && error_message[0] != '\0') {
        SetAsyncError(&request->result, error_message);
    }
    request->result.cancelled = cancelled;

    if (request->io_event != NULL) {
        event_free(request->io_event);
        request->io_event = NULL;
    }
    if (request->timeout_event != NULL) {
        event_free(request->timeout_event);
        request->timeout_event = NULL;
    }
    if (request->dns_request != NULL) {
        evdns_getaddrinfo_cancel(request->dns_request);
        request->dns_request = NULL;
    }
    if (request->connect_addrs != NULL) {
        evutil_freeaddrinfo(request->connect_addrs);
        request->connect_addrs = NULL;
        request->connect_next_addr = NULL;
    }

    request->client->active_fds.erase(request->fd);
    request->client->requests.erase(request->request_id);

    callback = request->callback;
    user_data = request->user_data;
    result = request->result;
    delete request;

    if (callback != NULL) {
        callback(&result, user_data);
    }
}

static void AsyncTimeoutCb(evutil_socket_t, short, void* arg)
{
    AsyncRequestFinish(static_cast<AsyncRequest*>(arg), false, "request timeout");
}

static bool AsyncProcessInput(AsyncRequest* request)
{
    for (;;) {
        uint32_t             frame_size = 0;
        eular::ntrs::Message msg;

        if (request->input.size() < eular::ntrs::FRAME_HDR_SIZE) {
            return false;
        }
        if (!eular::ntrs::FrameSizeFromHeader(request->input.data(), request->input.size(), &frame_size)) {
            AsyncRequestFinish(request, false, "invalid response frame");
            return true;
        }
        if (frame_size < eular::ntrs::FRAME_HDR_SIZE || frame_size > 8192u) {
            AsyncRequestFinish(request, false, "response frame too large");
            return true;
        }
        if (request->input.size() < frame_size) {
            return false;
        }
        if (!eular::ntrs::DecodeMessage(request->input.data(), frame_size, &msg)) {
            AsyncRequestFinish(request, false, "decode response failed");
            return true;
        }
        request->input.erase(request->input.begin(), request->input.begin() + frame_size);
        if (msg.request_id != (uint32_t)request->request_id && request->type != NTRS_ASYNC_WAIT_FOR_SIGNAL) {
            continue;
        }
        FillAsyncResultFromMessage(request, msg);
        AsyncRequestFinish(request, false, NULL);
        return true;
    }
}

static void AsyncIoCb(evutil_socket_t fd, short events, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);

    if ((events & EV_WRITE) != 0 && request->output_offset < request->output.size()) {
        while (request->output_offset < request->output.size()) {
            ssize_t nsend = send((int)fd, request->output.data() + request->output_offset,
                                 request->output.size() - request->output_offset, 0);
            if (nsend < 0 && SocketErrRwRetriable(SocketLastError())) {
                break;
            }
            if (nsend <= 0) {
                AsyncRequestFinish(request, false, "send request failed");
                return;
            }
            request->output_offset += (size_t)nsend;
        }
        if (request->output_offset == request->output.size() && !request->expect_response) {
            request->result.success = true;
            AsyncRequestFinish(request, false, NULL);
            return;
        }
        if (request->output_offset == request->output.size()) {
            event_del(request->io_event);
            event_assign(request->io_event, request->client->base, fd, EV_READ | EV_PERSIST, AsyncIoCb, request);
            event_add(request->io_event, NULL);
        }
    }

    if ((events & EV_READ) != 0) {
        for (;;) {
            uint8_t buffer[2048];
            ssize_t nread = recv((int)fd, buffer, sizeof(buffer), 0);
            if (nread < 0 && SocketErrInterrupted(SocketLastError())) {
                continue;
            }
            if (nread < 0 && SocketErrWouldBlock(SocketLastError())) {
                break;
            }
            if (nread <= 0) {
                AsyncRequestFinish(request, false, "connection closed");
                return;
            }
            request->input.insert(request->input.end(), buffer, buffer + nread);
            if (AsyncProcessInput(request)) {
                return;
            }
        }
    }
}

static bool SockaddrEndpoint(const struct sockaddr_storage* addr, socklen_t len, std::string* ip, uint16_t* port)
{
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];

    if (addr == NULL || ip == NULL || port == NULL ||
        getnameinfo((const struct sockaddr*)addr, len, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }

    *ip = host;
    *port = (uint16_t)atoi(service);
    return !ip->empty() && *port != 0;
}

static bool SockaddrSameEndpoint(const struct sockaddr_storage* lhs, socklen_t lhs_len,
                                   const struct sockaddr_storage* rhs, socklen_t rhs_len)
{
    std::string lhs_ip;
    std::string rhs_ip;
    uint16_t    lhs_port = 0;
    uint16_t    rhs_port = 0;

    if (lhs == NULL || rhs == NULL || lhs->ss_family != rhs->ss_family || lhs_len == 0 || rhs_len == 0) {
        return false;
    }

    return SockaddrEndpoint(lhs, lhs_len, &lhs_ip, &lhs_port) && SockaddrEndpoint(rhs, rhs_len, &rhs_ip, &rhs_port) &&
           lhs_ip == rhs_ip && lhs_port == rhs_port;
}

static bool AsyncFilterResponseValid(const AsyncRequest* request, const struct sockaddr_storage* src_addr,
                                        socklen_t src_len, const StunResponseInfo& response)
{
    std::string primary_ip;
    std::string secondary_ip;
    std::string source_ip;
    uint16_t    primary_port = 0;
    uint16_t    secondary_port = 0;
    uint16_t    source_port = 0;

    if (request == NULL || src_addr == NULL || src_len == 0 ||
        !SockaddrEndpoint(src_addr, src_len, &source_ip, &source_port) ||
        !SockaddrEndpoint(&request->probe_addrs[0], request->probe_addr_lens[0], &primary_ip, &primary_port)) {
        return false;
    }

    if (request->nat_phase == AsyncNatPhase::FILTER_CHANGE_PORT) {
        if (source_ip != response.response_origin_ip || source_port != response.response_origin_port ||
            response.response_origin_ip != primary_ip || response.response_origin_port == primary_port) {
            return false;
        }
        if (!response.other_address_ip.empty() &&
            (response.other_address_ip != primary_ip || response.other_address_port != primary_port)) {
            return false;
        }
        return true;
    }

    if (request->nat_phase != AsyncNatPhase::FILTER_CHANGE_IP || !request->has_probe2 ||
        !SockaddrEndpoint(&request->probe_addrs[1], request->probe_addr_lens[1], &secondary_ip, &secondary_port)) {
        return false;
    }

    if (source_ip != response.response_origin_ip || source_port != response.response_origin_port ||
        response.response_origin_ip != secondary_ip || response.response_origin_port != secondary_port) {
        return false;
    }
    return true;
}

static bool RefreshLocalEndpointForRemote(int sock, const struct sockaddr* remote_addr, socklen_t remote_len,
                                              std::string* ip, uint16_t* port)
{
    struct sockaddr_storage local_addr;
    socklen_t               local_len = sizeof(local_addr);

    if (remote_addr == NULL || ip == NULL || port == NULL) {
        return false;
    }

    if (connect(sock, remote_addr, remote_len) != 0) {
        return false;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(sock, (struct sockaddr*)&local_addr, &local_len) != 0 ||
        !SockaddrEndpoint(&local_addr, local_len, ip, port)) {
        struct sockaddr_storage unspec_addr;

        memset(&unspec_addr, 0, sizeof(unspec_addr));
        unspec_addr.ss_family = AF_UNSPEC;
        connect(sock, (const struct sockaddr*)&unspec_addr, sizeof(unspec_addr));
        return false;
    }

    {
        struct sockaddr_storage unspec_addr;

        memset(&unspec_addr, 0, sizeof(unspec_addr));
        unspec_addr.ss_family = AF_UNSPEC;
        connect(sock, (const struct sockaddr*)&unspec_addr, sizeof(unspec_addr));
    }

    return true;
}

static bool LocalEndpointIsExplicitBound(const struct sockaddr_storage* local_addr, socklen_t local_len)
{
    std::string ip;
    uint16_t    port = 0;

    if (local_addr == NULL || local_len == 0) {
        return false;
    }
    if (!SockaddrEndpoint(local_addr, local_len, &ip, &port)) {
        return false;
    }
    if (ip.empty() || ip == "0.0.0.0" || ip == "::") {
        return false;
    }
    return true;
}

static void InitAsyncNatSample(AsyncRequest* request)
{
    struct sockaddr_storage local_addr;
    socklen_t               local_len = sizeof(local_addr);

    request->nat_sample = NatSample();
    request->nat_sample.local_ip = "0.0.0.0";
    request->nat_sample.srflx_ip = "0.0.0.0";
    request->nat_sample.srflx_ip_2 = "0.0.0.0";
    request->nat_sample.nat_risk = "high";
    request->nat_sample.nat_class = NTRS_NAT_CLASS_UNKNOWN;
    request->nat_sample.nat_flags = NTRS_NAT_FLAG_NONE;
    request->nat_sample.mapping_behavior = NTRS_MAPPING_UNKNOWN;
    request->nat_sample.filtering_behavior = NTRS_FILTERING_UNKNOWN;
    request->nat_sample.nat_type = "unknown";
    request->nat_sample.probe1_rtt_ms = -1;
    request->nat_sample.probe2_rtt_ms = -1;
    request->nat_sample.probe_rounds = request->probe_success_target;

    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(request->fd, (struct sockaddr*)&local_addr, &local_len) == 0) {
        std::string ip;
        uint16_t    port = 0;
        if (SockaddrEndpoint(&local_addr, local_len, &ip, &port)) {
            request->nat_sample.local_ip = ip;
            request->nat_sample.local_port = port;
        }
    }
}

static bool AsyncNatPhaseIsFilter(const AsyncRequest* request)
{
    return request != NULL &&
           (request->nat_phase == AsyncNatPhase::FILTER_CHANGE_PORT ||
            request->nat_phase == AsyncNatPhase::FILTER_CHANGE_IP);
}

static void NatProbeLog(const AsyncRequest* request, const char* fmt, ...)
{
    va_list args;

    if (request == NULL || !request->nat_verbose || fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static std::string TxidToHex(const uint8_t* txid, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       out;

    if (txid == NULL || len == 0) {
        return "";
    }
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(txid[i] >> 4) & 0x0F]);
        out.push_back(kHex[txid[i] & 0x0F]);
    }
    return out;
}

static bool SendAsyncProbeRequest(AsyncRequest* request);
static void FinishAsyncNatDetection(AsyncRequest* request);

static bool AdvanceAsyncNatPhase(AsyncRequest* request)
{
    bool mapping_changed = false;
    bool mapping_unstable = false;

    if (request == NULL) {
        return false;
    }

    mapping_changed = request->nat_sample.srflx_ip != request->nat_sample.srflx_ip_2 ||
                      request->nat_sample.srflx_port != request->nat_sample.srflx_port_2;
    mapping_unstable =
        request->nat_sample.probe1_distinct_mappings > 1 || request->nat_sample.probe2_distinct_mappings > 1;

    if (request->nat_phase == AsyncNatPhase::PROBE1) {
        if (request->has_probe2 && request->nat_enable_filter_probe) {
            NatProbeLog(request, "nat probe advance phase=%s -> %s\n", AsyncNatPhaseName(request->nat_phase),
                          AsyncNatPhaseName(AsyncNatPhase::FILTER_CHANGE_PORT));
            request->nat_phase = AsyncNatPhase::FILTER_CHANGE_PORT;
            request->probe_index = 0;
            request->probe_attempts = 0;
            request->probe_max_attempts = kAsyncFilterMaxAttempts;
            request->probe_change_request = kStunChangePort;
            request->nat_sample.filter_probe_executed = true;
            return SendAsyncProbeRequest(request);
        }
        if (request->has_probe2) {
            NatProbeLog(request, "nat probe advance phase=%s -> %s\n", AsyncNatPhaseName(request->nat_phase),
                          AsyncNatPhaseName(AsyncNatPhase::PROBE2));
            request->nat_phase = AsyncNatPhase::PROBE2;
            request->probe_index = 1;
            request->probe_attempts = 0;
            request->probe_change_request = 0;
            return SendAsyncProbeRequest(request);
        }
        NatProbeLog(request, "nat probe finish after %s\n", AsyncNatPhaseName(request->nat_phase));
        FinishAsyncNatDetection(request);
        return true;
    }

    if (request->nat_phase == AsyncNatPhase::PROBE2) {
        NatProbeLog(request, "nat probe finish after %s mapping_changed=%s mapping_unstable=%s filter_probe=%s\n",
                      AsyncNatPhaseName(request->nat_phase), mapping_changed ? "true" : "false",
                      mapping_unstable ? "true" : "false", request->nat_enable_filter_probe ? "true" : "false");
        FinishAsyncNatDetection(request);
        return true;
    }

    if (request->nat_phase == AsyncNatPhase::FILTER_CHANGE_PORT) {
        NatProbeLog(request, "nat probe advance phase=%s -> %s\n", AsyncNatPhaseName(request->nat_phase),
                      AsyncNatPhaseName(AsyncNatPhase::FILTER_CHANGE_IP));
        request->nat_phase = AsyncNatPhase::FILTER_CHANGE_IP;
        request->probe_index = 0;
        request->probe_attempts = 0;
        request->probe_max_attempts = kAsyncFilterMaxAttempts;
        request->probe_change_request = kStunChangeIp;
        return SendAsyncProbeRequest(request);
    }

    if (request->nat_phase == AsyncNatPhase::FILTER_CHANGE_IP) {
        if (request->has_probe2) {
            NatProbeLog(request, "nat probe advance phase=%s -> %s\n", AsyncNatPhaseName(request->nat_phase),
                          AsyncNatPhaseName(AsyncNatPhase::PROBE2));
            request->nat_phase = AsyncNatPhase::PROBE2;
            request->probe_index = 1;
            request->probe_attempts = 0;
            request->probe_max_attempts = request->probe_phase_max_attempts;
            request->probe_change_request = 0;
            return SendAsyncProbeRequest(request);
        }
        NatProbeLog(request, "nat probe finish after %s\n", AsyncNatPhaseName(request->nat_phase));
        FinishAsyncNatDetection(request);
        return true;
    }

    NatProbeLog(request, "nat probe finish after %s\n", AsyncNatPhaseName(request->nat_phase));
    FinishAsyncNatDetection(request);
    return true;
}

static void FinishAsyncNatDetection(AsyncRequest* request)
{
    bool has_probe2 = request->has_probe2 && request->probe_success_count[1] > 0;

    if (request->probe_success_count[0] > 0) {
        request->nat_sample.probe1_ok = true;
        request->nat_sample.probe1_success_count = request->probe_success_count[0];
        request->nat_sample.probe1_distinct_mappings = (int32_t)request->probe_mappings[0].size();
        request->nat_sample.probe1_rtt_ms = (int32_t)(request->probe_rtt_sum_ms[0] / request->probe_success_count[0]);
    }
    if (request->probe_success_count[1] > 0) {
        request->nat_sample.probe2_ok = true;
        request->nat_sample.probe2_success_count = request->probe_success_count[1];
        request->nat_sample.probe2_distinct_mappings = (int32_t)request->probe_mappings[1].size();
        request->nat_sample.probe2_rtt_ms = (int32_t)(request->probe_rtt_sum_ms[1] / request->probe_success_count[1]);
    }

    NatProbeLog(
        request,
        "nat probe finish local=%s:%u srflx1=%s:%u srflx2=%s:%u mapping=%u filter=%u flags=0x%04x class=%u "
        "rounds=%d p1=%d p2=%d p1_map=%d p2_map=%d\n",
        request->nat_sample.local_ip.c_str(), (unsigned)request->nat_sample.local_port,
        request->nat_sample.srflx_ip.c_str(), (unsigned)request->nat_sample.srflx_port,
        request->nat_sample.srflx_ip_2.c_str(), (unsigned)request->nat_sample.srflx_port_2,
        (unsigned)request->nat_sample.mapping_behavior, (unsigned)request->nat_sample.filtering_behavior,
        (unsigned)request->nat_sample.nat_flags, (unsigned)request->nat_sample.nat_class,
        request->nat_sample.probe_rounds, request->nat_sample.probe1_success_count,
        request->nat_sample.probe2_success_count, request->nat_sample.probe1_distinct_mappings,
        request->nat_sample.probe2_distinct_mappings);

    EvaluateNatResult(&request->nat_sample, has_probe2);
    FillNatInfo(&request->result.nat_info, request->nat_sample);
    request->result.success = request->nat_sample.probe1_ok;
    if (!request->result.success) {
        SetAsyncError(&request->result, "detect nat failed");
    }
    AsyncRequestFinish(request, false, NULL);
}

static void AsyncNatTimeoutCb(evutil_socket_t, short, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);

    if (request == NULL) {
        return;
    }

    request->probe_attempts++;
    if (request->probe_attempts < request->probe_max_attempts) {
        if (!SendAsyncProbeRequest(request)) {
            AsyncRequestFinish(request, false, "send probe request failed");
        }
        return;
    }

    if (!AdvanceAsyncNatPhase(request)) {
        AsyncRequestFinish(request, false, "advance nat phase failed");
    }
}

static bool ArmAsyncProbeTimer(AsyncRequest* request)
{
    struct timeval tv;

    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    return evtimer_add(request->timeout_event, &tv) == 0;
}

static bool SendAsyncProbeRequest(AsyncRequest* request)
{
    std::vector<uint8_t>        message;
    uint8_t                     transaction_id[NTRS_PROBE_TOKEN_WIRE_SIZE];
    uint32_t                    change_request = request->probe_change_request;
    int                         target_index = request->probe_index;

    BuildAsyncNatProbeToken(request, transaction_id);
    memcpy(request->probe_token, transaction_id, sizeof(request->probe_token));

    NatProbeLog(request, "nat probe send phase=%s target=%s:%u change_request=0x%08x attempt=%d token=%s\n",
                  AsyncNatPhaseName(request->nat_phase), request->probe_hosts[target_index].c_str(),
                  (unsigned)request->probe_ports[target_index], (unsigned)change_request, request->probe_attempts,
                  TxidToHex(transaction_id, sizeof(transaction_id)).c_str());

    if (!BuildAsyncNatProbeRequestFrame(request, transaction_id, &message)) {
        return false;
    }

    if (sendto(request->fd, message.data(), message.size(), 0, (struct sockaddr*)&request->probe_addrs[target_index],
               request->probe_addr_lens[target_index]) < 0) {
        if (SocketErrRwRetriable(SocketLastError())) {
            return ArmAsyncProbeTimer(request);
        }
        return false;
    }

    if (request->nat_sample.local_port == 0 || request->nat_sample.local_ip == "0.0.0.0") {
        std::string ip;
        uint16_t    port = 0;

        if (RefreshLocalEndpointForRemote(request->fd, (const struct sockaddr*)&request->probe_addrs[target_index],
                                              request->probe_addr_lens[target_index], &ip, &port)) {
            request->nat_sample.local_ip = ip;
            request->nat_sample.local_port = port;
        }
    }

    gettimeofday(&request->probe_sent_at, NULL);
    return ArmAsyncProbeTimer(request);
}

static void AsyncNatIoCb(evutil_socket_t fd, short events, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);

    if ((events & EV_READ) == 0) {
        return;
    }

    for (;;) {
        uint8_t                 buffer[1500];
        struct sockaddr_storage src_addr;
        socklen_t               src_len = sizeof(src_addr);
        StunResponseInfo        response;
        ssize_t nread = recvfrom((int)fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&src_addr, &src_len);
        if (nread < 0 && SocketErrInterrupted(SocketLastError())) {
            continue;
        }
        if (nread < 0 && SocketErrWouldBlock(SocketLastError())) {
            return;
        }
        if (nread <= 0) {
            return;
        }

        if (!ParseAsyncNatProbeResponseFrame(buffer, (size_t)nread,
                                                  AsyncNatPhaseToBinaryPhase(request->nat_phase),
                                                  request->probe_token, &response)) {
            NatProbeLog(request, "nat probe drop phase=%s reason=binary_frame_invalid len=%zd\n",
                          AsyncNatPhaseName(request->nat_phase), nread);
            continue;
        }
        if (AsyncNatPhaseIsFilter(request)) {
            if (!AsyncFilterResponseValid(request, &src_addr, src_len, response)) {
                std::string src_ip;
                uint16_t    src_port = 0;
                SockaddrEndpoint(&src_addr, src_len, &src_ip, &src_port);
                NatProbeLog(
                    request,
                    "nat probe drop phase=%s src=%s:%u reason=filter_source_invalid resp_origin=%s:%u other=%s:%u\n",
                    AsyncNatPhaseName(request->nat_phase), src_ip.c_str(), (unsigned)src_port,
                    response.response_origin_ip.c_str(), (unsigned)response.response_origin_port,
                    response.other_address_ip.c_str(), (unsigned)response.other_address_port);
                continue;
            }
        } else if (!SockaddrSameEndpoint(&src_addr, src_len, &request->probe_addrs[request->probe_index],
                                           request->probe_addr_lens[request->probe_index])) {
            std::string src_ip;
            uint16_t    src_port = 0;
            std::string exp_ip;
            uint16_t    exp_port = 0;
            SockaddrEndpoint(&src_addr, src_len, &src_ip, &src_port);
            SockaddrEndpoint(&request->probe_addrs[request->probe_index], request->probe_addr_lens[request->probe_index],
                              &exp_ip, &exp_port);
            NatProbeLog(request, "nat probe drop phase=%s src=%s:%u expected=%s:%u reason=unexpected_source\n",
                          AsyncNatPhaseName(request->nat_phase), src_ip.c_str(), (unsigned)src_port,
                          exp_ip.c_str(), (unsigned)exp_port);
            continue;
        }

        struct timeval now;
        long           rtt_ms = 0;
        gettimeofday(&now, NULL);
        rtt_ms = (long)(now.tv_sec - request->probe_sent_at.tv_sec) * 1000L +
                 (long)(now.tv_usec - request->probe_sent_at.tv_usec) / 1000L;
        if (rtt_ms < 0) {
            rtt_ms = 0;
        }

        evtimer_del(request->timeout_event);
        if (request->nat_phase == AsyncNatPhase::FILTER_CHANGE_PORT) {
            request->nat_sample.filter_probe_executed = true;
            request->nat_sample.filter_same_ip_diff_port_rx = true;
            NatProbeLog(request, "nat probe receive phase=%s matched=change_port\n",
                          AsyncNatPhaseName(request->nat_phase));
            if (!AdvanceAsyncNatPhase(request)) {
                AsyncRequestFinish(request, false, "advance nat phase failed");
            }
            return;
        }
        if (request->nat_phase == AsyncNatPhase::FILTER_CHANGE_IP) {
            request->nat_sample.filter_probe_executed = true;
            request->nat_sample.filter_diff_ip_rx = true;
            NatProbeLog(request, "nat probe receive phase=%s matched=change_ip\n",
                          AsyncNatPhaseName(request->nat_phase));
            if (!AdvanceAsyncNatPhase(request)) {
                AsyncRequestFinish(request, false, "advance nat phase failed");
            }
            return;
        }

        request->probe_success_count[request->probe_index]++;
        request->probe_rtt_sum_ms[request->probe_index] += rtt_ms;
        request->probe_mappings[request->probe_index].insert(response.mapped_ip + ":" +
                                                           std::to_string((unsigned long long)response.mapped_port));

        if (request->probe_index == 0) {
            request->nat_sample.srflx_ip = response.mapped_ip;
            request->nat_sample.srflx_port = response.mapped_port;
        } else {
            request->nat_sample.srflx_ip_2 = response.mapped_ip;
            request->nat_sample.srflx_port_2 = response.mapped_port;
        }
        NatProbeLog(request, "nat probe receive phase=%s mapped=%s:%u origin=%s:%u other=%s:%u rtt=%ldms\n",
                      AsyncNatPhaseName(request->nat_phase), response.mapped_ip.c_str(),
                      (unsigned)response.mapped_port, response.response_origin_ip.c_str(),
                      (unsigned)response.response_origin_port, response.other_address_ip.c_str(),
                      (unsigned)response.other_address_port, rtt_ms);

        if (request->probe_success_count[request->probe_index] < request->probe_success_target &&
            request->probe_attempts + 1 < request->probe_max_attempts) {
            request->probe_attempts++;
            if (!SendAsyncProbeRequest(request)) {
                AsyncRequestFinish(request, false, "send probe request failed");
            }
            return;
        }

        if (!AdvanceAsyncNatPhase(request)) {
            AsyncRequestFinish(request, false, "advance nat phase failed");
        }
        return;
    }
}

static bool StartAsyncNatIo(AsyncRequest* request)
{
    if (request == NULL) {
        return false;
    }

    InitAsyncNatSample(request);
    NatProbeLog(request, "nat probe start local=%s:%u probe1=%s:%u probe2=%s:%u filter_probe=%s rounds=%d retries=%d\n",
                  request->nat_sample.local_ip.c_str(), (unsigned)request->nat_sample.local_port,
                  request->probe_hosts[0].c_str(), (unsigned)request->probe_ports[0],
                  request->has_probe2 ? request->probe_hosts[1].c_str() : "-",
                  request->has_probe2 ? (unsigned)request->probe_ports[1] : 0u,
                  request->nat_enable_filter_probe ? "true" : "false", request->probe_success_target,
                  request->probe_max_attempts);
    request->io_event = event_new(request->client->base, request->fd, EV_READ | EV_PERSIST, AsyncNatIoCb, request);
    request->timeout_event = evtimer_new(request->client->base, AsyncNatTimeoutCb, request);
    if (request->io_event == NULL || request->timeout_event == NULL) {
        AsyncRequestFinish(request, false, "create async nat event failed");
        return false;
    }
    if (event_add(request->io_event, NULL) != 0 || !SendAsyncProbeRequest(request)) {
        AsyncRequestFinish(request, false, "start async nat request failed");
        return false;
    }
    return true;
}

static void AsyncNatDnsCb(int result, struct evutil_addrinfo* res, void* arg);

static bool ResolveAsyncNatEndpoint(AsyncRequest* request, int index)
{
    char                   port_text[16];
    struct evutil_addrinfo hints;

    if (request == NULL || request->client == NULL || request->client->dns_base == NULL ||
        request->probe_hosts[index].empty() || request->probe_ports[index] == 0) {
        return false;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family =
        request->udp_family == AF_INET || request->udp_family == AF_INET6 ? request->udp_family : AF_UNSPEC;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)request->probe_ports[index]);
    request->resolving_probe_index = index;
    request->dns_request = evdns_getaddrinfo(request->client->dns_base, request->probe_hosts[index].c_str(), port_text,
                                             &hints, AsyncNatDnsCb, request);
    if (request->dns_request == NULL && request->probe_addr_lens[index] == 0) {
        return false;
    }
    return true;
}

static bool ContinueAsyncNatResolution(AsyncRequest* request)
{
    if (!ResolveAsyncNatEndpoint(request, 0)) {
        return false;
    }
    return true;
}

static void AsyncNatDnsCb(int result, struct evutil_addrinfo* res, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);
    int           index = 0;

    if (request == NULL) {
        if (res != NULL) {
            evutil_freeaddrinfo(res);
        }
        return;
    }

    request->dns_request = NULL;
    index = request->resolving_probe_index;
    if (result != 0 || res == NULL) {
        if (res != NULL) {
            evutil_freeaddrinfo(res);
        }
        NatProbeLog(request, "nat probe resolve failed index=%d host=%s port=%u\n", index,
                      request->probe_hosts[index].c_str(), (unsigned)request->probe_ports[index]);
        AsyncRequestFinish(request, false, "resolve probe endpoint failed");
        return;
    }

    memcpy(&request->probe_addrs[index], res->ai_addr, res->ai_addrlen);
    request->probe_addr_lens[index] = (socklen_t)res->ai_addrlen;
    {
        std::string resolved_ip;
        uint16_t    resolved_port = 0;
        SockaddrEndpoint(&request->probe_addrs[index], request->probe_addr_lens[index], &resolved_ip,
                          &resolved_port);
        NatProbeLog(request, "nat probe resolve ok index=%d host=%s port=%u resolved=%s:%u\n", index,
                      request->probe_hosts[index].c_str(), (unsigned)request->probe_ports[index],
                      resolved_ip.c_str(), (unsigned)resolved_port);
    }
    evutil_freeaddrinfo(res);

    if (index == 0 && request->has_probe2) {
        if (!ResolveAsyncNatEndpoint(request, 1)) {
            AsyncRequestFinish(request, false, "resolve probe endpoint failed");
        }
        return;
    }

    StartAsyncNatIo(request);
}

static bool SubmitAsyncNatRequest(AsyncClientImpl* client, int32_t udp_sock, const char* probe1_host,
                                     uint16_t probe1_port, const char* probe2_host, uint16_t probe2_port,
                                     int32_t control_fd, const char* session_token,
                                     const ntrs_detect_nat_options_t* options, ntrs_async_callback_t callback,
                                     void* user_data, uint64_t* request_id)
{
    AsyncRequest*             request = NULL;
    ntrs_detect_nat_options_t effective_options;
    struct sockaddr_storage   local_addr;
    socklen_t                 local_len = sizeof(local_addr);

    if (client == NULL || client->base == NULL || client->dns_base == NULL || callback == NULL || udp_sock < 0 ||
        probe1_host == NULL || probe1_host[0] == '\0' || probe1_port == 0) {
        return false;
    }
    if (client->active_fds.find(udp_sock) != client->active_fds.end()) {
        return false;
    }
    request = new AsyncRequest();
    request->client = client;
    request->request_id = client->next_request_id++;
    request->type = NTRS_ASYNC_DETECT_NAT;
    request->fd = udp_sock;
    request->callback = callback;
    request->user_data = user_data;
    request->nat_control_fd = control_fd;
    request->nat_session_token = session_token == NULL ? "" : session_token;
    InitAsyncResult(&request->result, request->request_id, request->type);
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(udp_sock, (struct sockaddr*)&local_addr, &local_len) == 0) {
        request->udp_family = local_addr.ss_family;
        request->nat_explicit_bind = LocalEndpointIsExplicitBound(&local_addr, local_len);
    }

    ntrs_detect_nat_options_init(&effective_options);
    if (options != NULL) {
        effective_options = *options;
    }
    if (effective_options.probe_rounds <= 0) {
        effective_options.probe_rounds = 3;
    }
    if (effective_options.retries_per_round <= 0) {
        effective_options.retries_per_round = 3;
    }
    request->probe_success_target = effective_options.probe_rounds;
    request->probe_max_attempts = effective_options.probe_rounds * effective_options.retries_per_round;
    request->probe_phase_max_attempts = request->probe_max_attempts;
    request->nat_enable_filter_probe = effective_options.enable_filter_probe;
    request->nat_verbose = effective_options.verbose;
    request->has_probe2 = probe2_host != NULL && probe2_host[0] != '\0' && probe2_port > 0;
    request->nat_phase = AsyncNatPhase::PROBE1;
    request->probe_change_request = 0;
    request->probe_hosts[0] = probe1_host;
    request->probe_ports[0] = probe1_port;
    if (request->has_probe2) {
        request->probe_hosts[1] = probe2_host;
        request->probe_ports[1] = probe2_port;
    }

    client->requests[request->request_id] = request;
    client->active_fds[udp_sock] = request->request_id;
    if (request_id != NULL) {
        *request_id = request->request_id;
    }
    if (!ContinueAsyncNatResolution(request)) {
        if (client->requests.find(request->request_id) != client->requests.end()) {
            AsyncRequestFinish(request, false, "resolve probe endpoint failed");
            return false;
        }
        return true;
    }
    return true;
}

static bool SubmitAsyncMessageRequest(AsyncClientImpl* client, int32_t fd, ntrs_async_request_type_t type,
                                         const eular::ntrs::Message* message, bool expect_response, int timeout_ms,
                                         ntrs_async_callback_t callback, void* user_data, uint64_t* request_id)
{
    uint8_t        buffer[8192];
    size_t         encoded_len = 0;
    AsyncRequest*  request = NULL;
    struct timeval tv;

    if (client == NULL || client->base == NULL || callback == NULL || fd < 0) {
        return false;
    }
    if (client->active_fds.find(fd) != client->active_fds.end()) {
        return false;
    }
    if (message != NULL && eular::ntrs::EncodeMessage(*message, buffer, sizeof(buffer), &encoded_len) != 0) {
        return false;
    }
    if (!SetNonblocking(fd)) {
        return false;
    }

    request = new AsyncRequest();
    request->client = client;
    request->request_id = client->next_request_id++;
    request->type = type;
    request->fd = fd;
    request->expect_response = expect_response;
    request->output.assign(buffer, buffer + encoded_len);
    request->callback = callback;
    request->user_data = user_data;
    InitAsyncResult(&request->result, request->request_id, request->type);

    short io_events = EV_READ | EV_PERSIST;
    if (!request->output.empty()) {
        io_events |= EV_WRITE;
    }
    request->io_event = event_new(client->base, fd, io_events, AsyncIoCb, request);
    request->timeout_event = evtimer_new(client->base, AsyncTimeoutCb, request);
    if (request->io_event == NULL || request->timeout_event == NULL) {
        AsyncRequestFinish(request, false, "create async event failed");
        return false;
    }

    client->requests[request->request_id] = request;
    client->active_fds[fd] = request->request_id;
    if (request_id != NULL) {
        *request_id = request->request_id;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (timeout_ms <= 0) {
        tv.tv_sec = kControlIoTimeoutMs / 1000;
        tv.tv_usec = (kControlIoTimeoutMs % 1000) * 1000;
    }
    if (event_add(request->io_event, NULL) != 0 || evtimer_add(request->timeout_event, &tv) != 0) {
        AsyncRequestFinish(request, false, "start async event failed");
        return false;
    }
    return true;
}

static bool StartAsyncConnectAttempt(AsyncRequest* request);

static void AsyncConnectIoCb(evutil_socket_t, short events, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);
    int           err = 0;
    socklen_t     err_len = sizeof(err);

    if (request == NULL || (events & EV_WRITE) == 0) {
        return;
    }

    if (getsockopt(request->fd, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0) {
        request->result.success = true;
        request->result.control_fd = request->fd;
        request->fd = -1;
        AsyncRequestFinish(request, false, NULL);
        return;
    }

    if (request->fd >= 0) {
        EVUTIL_CLOSESOCKET(request->fd);
        request->fd = -1;
    }
    if (!StartAsyncConnectAttempt(request)) {
        AsyncRequestFinish(request, false, "connect control failed");
    }
}

static void AsyncConnectTimeoutCb(evutil_socket_t, short, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);
    if (request == NULL) {
        return;
    }
    if (request->fd >= 0) {
        EVUTIL_CLOSESOCKET(request->fd);
        request->fd = -1;
    }
    if (!StartAsyncConnectAttempt(request)) {
        AsyncRequestFinish(request, false, "connect control timeout");
    }
}

static bool ArmAsyncConnectTimeout(AsyncRequest* request)
{
    struct timeval tv;
    tv.tv_sec = request->connect_timeout_ms / 1000;
    tv.tv_usec = (request->connect_timeout_ms % 1000) * 1000;
    if (request->connect_timeout_ms <= 0) {
        tv.tv_sec = kControlIoTimeoutMs / 1000;
        tv.tv_usec = (kControlIoTimeoutMs % 1000) * 1000;
    }
    return evtimer_add(request->timeout_event, &tv) == 0;
}

static bool StartAsyncConnectAttempt(AsyncRequest* request)
{
    while (request != NULL && request->connect_next_addr != NULL) {
        struct evutil_addrinfo* addr = request->connect_next_addr;
        request->connect_next_addr = request->connect_next_addr->ai_next;

        request->fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (request->fd < 0) {
            continue;
        }
        if (!SetNonblocking(request->fd)) {
            EVUTIL_CLOSESOCKET(request->fd);
            request->fd = -1;
            continue;
        }

        int rc = connect(request->fd, addr->ai_addr, addr->ai_addrlen);
        if (rc == 0) {
            request->result.success = true;
            request->result.control_fd = request->fd;
            request->fd = -1;
            AsyncRequestFinish(request, false, NULL);
            return true;
        }
        if (!SocketErrConnectRetriable(SocketLastError())) {
            EVUTIL_CLOSESOCKET(request->fd);
            request->fd = -1;
            continue;
        }

        if (request->io_event != NULL) {
            event_free(request->io_event);
        }
        if (request->timeout_event != NULL) {
            event_free(request->timeout_event);
        }
        request->io_event = event_new(request->client->base, request->fd, EV_WRITE, AsyncConnectIoCb, request);
        request->timeout_event = evtimer_new(request->client->base, AsyncConnectTimeoutCb, request);
        if (request->io_event == NULL || request->timeout_event == NULL) {
            return false;
        }
        if (event_add(request->io_event, NULL) != 0 || !ArmAsyncConnectTimeout(request)) {
            return false;
        }
        return true;
    }

    return false;
}

static void AsyncConnectDnsCb(int result, struct evutil_addrinfo* res, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);
    if (request == NULL) {
        if (res != NULL) {
            evutil_freeaddrinfo(res);
        }
        return;
    }

    request->dns_request = NULL;
    if (result != 0 || res == NULL) {
        AsyncRequestFinish(request, false, "resolve control endpoint failed");
        return;
    }

    request->connect_addrs = res;
    request->connect_next_addr = res;
    if (!StartAsyncConnectAttempt(request)) {
        AsyncRequestFinish(request, false, "connect control failed");
    }
}

static bool SubmitAsyncConnectRequest(AsyncClientImpl* client, const char* host, uint16_t port, int32_t timeout_ms,
                                         ntrs_async_callback_t callback, void* user_data, uint64_t* request_id)
{
    AsyncRequest*          request = NULL;
    char                   port_text[16];
    struct evutil_addrinfo hints;

    if (client == NULL || client->base == NULL || client->dns_base == NULL || host == NULL || host[0] == '\0' ||
        port == 0 || callback == NULL) {
        return false;
    }

    request = new AsyncRequest();
    request->client = client;
    request->request_id = client->next_request_id++;
    request->type = NTRS_ASYNC_CONNECT_CONTROL;
    request->fd = -1;
    request->callback = callback;
    request->user_data = user_data;
    request->connect_host = host;
    request->connect_port = port;
    request->connect_timeout_ms = timeout_ms <= 0 ? kControlIoTimeoutMs : timeout_ms;
    InitAsyncResult(&request->result, request->request_id, request->type);

    client->requests[request->request_id] = request;
    if (request_id != NULL) {
        *request_id = request->request_id;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    request->dns_request = evdns_getaddrinfo(client->dns_base, host, port_text, &hints, AsyncConnectDnsCb, request);
    if (request->dns_request == NULL && request->connect_addrs == NULL &&
        client->requests.find(request->request_id) != client->requests.end()) {
        AsyncRequestFinish(request, false, "resolve control endpoint failed");
        return false;
    }
    return true;
}

static bool CandidateToSockaddr(const ntrs_peer_candidate_t* candidate, struct sockaddr_storage* out,
                                  socklen_t* out_len)
{
    struct sockaddr_in*  addr4 = NULL;
    struct sockaddr_in6* addr6 = NULL;

    if (candidate == NULL || candidate->ip[0] == '\0' || candidate->port == 0 || out == NULL || out_len == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    addr4 = (struct sockaddr_in*)out;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(candidate->port);
    if (inet_pton(AF_INET, candidate->ip, &addr4->sin_addr) == 1) {
        *out_len = sizeof(*addr4);
        return true;
    }

    memset(out, 0, sizeof(*out));
    addr6 = (struct sockaddr_in6*)out;
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(candidate->port);
    if (inet_pton(AF_INET6, candidate->ip, &addr6->sin6_addr) == 1) {
        *out_len = sizeof(*addr6);
        return true;
    }

    return false;
}

static void AsyncPunchFinish(AsyncRequest* request, bool success, const ntrs_peer_candidate_t* selected)
{
    request->result.success = success;
    if (success && selected != NULL) {
        request->result.selected_candidate = *selected;
    } else {
        SetAsyncError(&request->result, "udp hole punch failed");
    }
    AsyncRequestFinish(request, false, NULL);
}

struct PunchFrameMeta {
    ntrs_binary_frame_type_t frame_type;
    uint32_t                 request_id;
    uint32_t                 sequence;
    uint8_t                  token[8];
    uint8_t                  token_len;
    char                     candidate_type[NTRS_MAX_TEXT_LEN];
};

/**
 * @brief 构造本轮打洞使用的固定 token。
 *
 * 当前阶段先使用请求编号派生一个稳定的 8 字节 token，避免继续依赖文本前缀。
 * 后续接入真正的 `probe_token` 后，可以在不破坏二进制帧结构的前提下直接替换。
 */
static void FillPunchToken(uint64_t request_id, uint8_t token[8])
{
    uint32_t high = (uint32_t)((request_id >> 32) & 0xFFFFFFFFu);
    uint32_t low = (uint32_t)(request_id & 0xFFFFFFFFu);

    token[0] = (uint8_t)((high >> 24) & 0xFFu);
    token[1] = (uint8_t)((high >> 16) & 0xFFu);
    token[2] = (uint8_t)((high >> 8) & 0xFFu);
    token[3] = (uint8_t)(high & 0xFFu);
    token[4] = (uint8_t)((low >> 24) & 0xFFu);
    token[5] = (uint8_t)((low >> 16) & 0xFFu);
    token[6] = (uint8_t)((low >> 8) & 0xFFu);
    token[7] = (uint8_t)(low & 0xFFu);
}

/**
 * @brief 构造二进制打洞帧。
 */
static bool BuildPunchFrame(ntrs_binary_frame_type_t frame_type, uint64_t request_id, const char* candidate_type,
                              uint32_t round, uint8_t* out, size_t out_cap, size_t* out_len)
{
    ntrs_binary_frame_t frame;
    uint8_t             token[8];
    uint64_t            timestamp_ms = CurrentTimeMs();
    const char*         type_text =
        candidate_type == NULL || candidate_type[0] == '\0' ? "candidate" : candidate_type;

    if (out == NULL || out_len == NULL || !ntrs_binary_frame_init(&frame, out, out_cap)) {
        return false;
    }

    FillPunchToken(request_id, token);
    if (!ntrs_binary_frame_set_header(&frame, frame_type, NTRS_BINARY_PHASE_PUNCH, 0u, (uint32_t)request_id, round,
                                      timestamp_ms)) {
        return false;
    }
    if (!ntrs_binary_frame_add_tlv(&frame, NTRS_BINARY_TLV_PROBE_TOKEN, token, sizeof(token))) {
        return false;
    }
    if (!ntrs_binary_frame_add_tlv(&frame, NTRS_BINARY_TLV_CANDIDATE_TYPE, type_text,
                                   (uint16_t)strlen(type_text))) {
        return false;
    }

    *out_len = frame.length;
    return true;
}

/**
 * @brief 解析二进制打洞帧的最小元数据。
 */
static bool ParsePunchFrame(const uint8_t* payload, size_t payload_len, PunchFrameMeta* out)
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

/**
 * @brief 由请求帧元数据构造 ACK。
 */
static bool BuildPunchAckFrame(const PunchFrameMeta* req_meta, uint8_t* out, size_t out_cap, size_t* out_len)
{
    ntrs_binary_frame_t frame;

    if (req_meta == NULL || out == NULL || out_len == NULL || !ntrs_binary_frame_init(&frame, out, out_cap)) {
        return false;
    }
    if (!ntrs_binary_frame_set_header(&frame, NTRS_BINARY_FRAME_PUNCH_ACK, NTRS_BINARY_PHASE_PUNCH, 0u,
                                      req_meta->request_id, req_meta->sequence, CurrentTimeMs())) {
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

static void AsyncPunchSendRound(AsyncRequest* request)
{
    for (size_t i = 0; i < request->punch_candidates.size(); ++i) {
        uint8_t payload[256];
        size_t  payload_len = 0;
        if (!BuildPunchFrame(NTRS_BINARY_FRAME_PUNCH_REQ, request->request_id, request->punch_candidates[i].type,
                               (uint32_t)(request->punch_round + 1), payload, sizeof(payload), &payload_len)) {
            continue;
        }
        sendto(request->fd, payload, payload_len, 0, (struct sockaddr*)&request->punch_addrs[i],
               request->punch_addr_lens[i]);
    }
}

static void AsyncPunchTimeoutCb(evutil_socket_t, short, void* arg)
{
    AsyncRequest*  request = static_cast<AsyncRequest*>(arg);
    struct timeval tv;

    if (request == NULL) {
        return;
    }
    request->punch_round++;
    if (request->punch_round >= request->punch_max_rounds) {
        AsyncPunchFinish(request, false, NULL);
        return;
    }

    AsyncPunchSendRound(request);
    tv.tv_sec = request->punch_interval_ms / 1000;
    tv.tv_usec = (request->punch_interval_ms % 1000) * 1000;
    evtimer_add(request->timeout_event, &tv);
}

static void AsyncPunchIoCb(evutil_socket_t fd, short events, void* arg)
{
    AsyncRequest* request = static_cast<AsyncRequest*>(arg);

    if ((events & EV_READ) == 0) {
        return;
    }

    for (;;) {
        uint8_t                 buffer[512];
        struct sockaddr_storage src;
        socklen_t               src_len = sizeof(src);
        ssize_t                 nread = recvfrom((int)fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&src, &src_len);
        PunchFrameMeta          meta;
        if (nread < 0 && SocketErrInterrupted(SocketLastError())) {
            continue;
        }
        if (nread < 0 && SocketErrWouldBlock(SocketLastError())) {
            return;
        }
        if (nread <= 0) {
            return;
        }

        std::string src_ip;
        uint16_t    src_port = 0;
        SockaddrEndpoint(&src, src_len, &src_ip, &src_port);

        for (size_t i = 0; i < request->punch_candidates.size(); ++i) {
            if (SockaddrSameEndpoint(&src, src_len, &request->punch_addrs[i], request->punch_addr_lens[i])) {
                if (ParsePunchFrame(buffer, (size_t)nread, &meta) && meta.frame_type == NTRS_BINARY_FRAME_PUNCH_REQ) {
                    uint8_t ack_payload[256];
                    size_t  ack_len = 0;
                    if (BuildPunchAckFrame(&meta, ack_payload, sizeof(ack_payload), &ack_len)) {
                        sendto((int)fd, ack_payload, ack_len, 0, (struct sockaddr*)&src, src_len);
                    }
                } else if (!ParsePunchFrame(buffer, (size_t)nread, &meta) ||
                           meta.frame_type != NTRS_BINARY_FRAME_PUNCH_ACK) {
                    continue;
                }
                AsyncPunchFinish(request, true, &request->punch_candidates[i]);
                return;
            }
        }

        if (ParsePunchFrame(buffer, (size_t)nread, &meta) && meta.frame_type == NTRS_BINARY_FRAME_PUNCH_REQ) {
            for (size_t i = 0; i < request->punch_candidates.size(); ++i) {
                if (strcmp(request->punch_candidates[i].type, "host_local") == 0 &&
                    request->punch_candidates[i].port == src_port) {
                    uint8_t ack_payload[256];
                    size_t  ack_len = 0;
                    if (BuildPunchAckFrame(&meta, ack_payload, sizeof(ack_payload), &ack_len)) {
                        sendto((int)fd, ack_payload, ack_len, 0, (struct sockaddr*)&src, src_len);
                    }
                    AsyncPunchFinish(request, true, &request->punch_candidates[i]);
                    return;
                }
            }
        }
    }
}

static bool SubmitAsyncPunchRequest(AsyncClientImpl* client, int32_t udp_sock,
                                       const ntrs_peer_candidate_t* candidates, uint32_t candidate_count,
                                       int32_t send_rounds, int32_t interval_ms, ntrs_async_callback_t callback,
                                       void* user_data, uint64_t* request_id)
{
    AsyncRequest*  request = NULL;
    struct timeval tv;

    if (client == NULL || client->base == NULL || callback == NULL || udp_sock < 0 || candidates == NULL ||
        candidate_count == 0) {
        return false;
    }
    if (client->active_fds.find(udp_sock) != client->active_fds.end()) {
        return false;
    }
    request = new AsyncRequest();
    request->client = client;
    request->request_id = client->next_request_id++;
    request->type = NTRS_ASYNC_UDP_HOLE_PUNCH;
    request->fd = udp_sock;
    request->callback = callback;
    request->user_data = user_data;
    request->punch_max_rounds = send_rounds <= 0 ? 8 : send_rounds;
    request->punch_interval_ms = interval_ms <= 0 ? 200 : interval_ms;
    InitAsyncResult(&request->result, request->request_id, request->type);

    for (uint32_t i = 0; i < candidate_count; ++i) {
        struct sockaddr_storage addr;
        socklen_t               len = 0;
        if (!CandidateToSockaddr(&candidates[i], &addr, &len)) {
            continue;
        }
        request->punch_candidates.push_back(candidates[i]);
        request->punch_addrs.push_back(addr);
        request->punch_addr_lens.push_back(len);
    }
    if (request->punch_candidates.empty()) {
        delete request;
        return false;
    }

    request->io_event = event_new(client->base, udp_sock, EV_READ | EV_PERSIST, AsyncPunchIoCb, request);
    request->timeout_event = evtimer_new(client->base, AsyncPunchTimeoutCb, request);
    if (request->io_event == NULL || request->timeout_event == NULL) {
        AsyncRequestFinish(request, false, "create async punch event failed");
        return false;
    }
    client->requests[request->request_id] = request;
    client->active_fds[udp_sock] = request->request_id;
    if (request_id != NULL) {
        *request_id = request->request_id;
    }
    if (event_add(request->io_event, NULL) != 0) {
        AsyncRequestFinish(request, false, "start async punch event failed");
        return false;
    }
    AsyncPunchSendRound(request);
    tv.tv_sec = request->punch_interval_ms / 1000;
    tv.tv_usec = (request->punch_interval_ms % 1000) * 1000;
    if (evtimer_add(request->timeout_event, &tv) != 0) {
        AsyncRequestFinish(request, false, "start async punch timer failed");
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

void ntrs_nat_info_init(ntrs_nat_info_t* info)
{
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(*info));
    CopyText(info->local_ip, sizeof(info->local_ip), "0.0.0.0");
    CopyText(info->srflx_ip, sizeof(info->srflx_ip), "0.0.0.0");
    CopyText(info->srflx_ip_2, sizeof(info->srflx_ip_2), "0.0.0.0");
    info->nat_class = NTRS_NAT_CLASS_UNKNOWN;
    info->nat_flags = NTRS_NAT_FLAG_NONE;
    info->mapping_behavior = NTRS_MAPPING_UNKNOWN;
    info->filtering_behavior = NTRS_FILTERING_UNKNOWN;
    CopyText(info->nat_risk, sizeof(info->nat_risk), "high");
    CopyText(info->nat_type, sizeof(info->nat_type), "unknown");
    info->probe1_rtt_ms = -1;
    info->probe2_rtt_ms = -1;
    info->probe_rounds = 3;
}

void ntrs_detect_nat_options_init(ntrs_detect_nat_options_t* options)
{
    if (options == NULL) {
        return;
    }

    options->probe_rounds = 3;
    options->retries_per_round = 3;
    options->enable_filter_probe = true;
    options->verbose = false;
}

int32_t ntrs_connect_control(const char* ntrs_ip, uint16_t ntrs_port)
{
    int fd = -1;
    if (!eular::ntrs::ConnectTcpHostPort(ntrs_ip, ntrs_port, kControlIoTimeoutMs, &fd)) {
        return -1;
    }
    return fd;
}

bool ntrs_request_probe_endpoints(int32_t control_fd, const char* session_token, char* probe1, size_t probe1_len,
                                  char* probe2, size_t probe2_len)
{
    static const int kProbeEndpointRetryCount = 10;
    static const int kProbeEndpointRetryDelayUs = 200 * 1000;
    eular::ntrs::Message req;
    eular::ntrs::Message rsp;
    const char*          probe1_value = NULL;
    const char*          probe2_value = NULL;
    int                  attempt = 0;

    if (control_fd < 0 || probe1 == NULL || probe1_len == 0 || probe2 == NULL || probe2_len == 0) {
        return false;
    }

    probe1[0] = '\0';
    probe2[0] = '\0';

    for (attempt = 0; attempt < kProbeEndpointRetryCount; ++attempt) {
        eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::NAT_PROBE_REQ, (uint32_t)g_ntrs_req++);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::VERSION, "m1");
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN,
                                           session_token == NULL ? "" : session_token);

        if (!SendMessage(control_fd, req) || !RecvMessage(control_fd, &rsp) ||
            rsp.type != eular::ntrs::MessageType::NAT_PROBE_RSP) {
            return false;
        }

        probe1_value = MsgStrTag(rsp, eular::ntrs::FieldTag::PROBE1);
        probe2_value = MsgStrTag(rsp, eular::ntrs::FieldTag::PROBE2);
        CopyText(probe1, probe1_len, probe1_value);
        CopyText(probe2, probe2_len, probe2_value);
        if (probe1[0] == '\0') {
            return false;
        }
        if (probe2[0] != '\0') {
            return true;
        }
        if (attempt + 1 < kProbeEndpointRetryCount) {
            usleep(kProbeEndpointRetryDelayUs);
        }
    }

    return probe1[0] != '\0';
}

bool ntrs_auth(int32_t control_fd, const char* peer_id, const char* bootstrap_token, char* session_token,
               size_t session_token_len, uint32_t* lease_default_sec)
{
    eular::ntrs::Message req;
    eular::ntrs::Message rsp;

    if (control_fd < 0 || peer_id == NULL || peer_id[0] == '\0' || bootstrap_token == NULL || session_token == NULL ||
        session_token_len == 0) {
        return false;
    }

    session_token[0] = '\0';
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::AUTH_REQ, (uint32_t)g_ntrs_req++);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, bootstrap_token);
    if (!SendMessage(control_fd, req) || !RecvMessage(control_fd, &rsp)) {
        return false;
    }

    if (rsp.type != eular::ntrs::MessageType::AUTH_RSP) {
        return false;
    }

    CopyText(session_token, session_token_len, MsgStrTag(rsp, eular::ntrs::FieldTag::TOKEN));
    if (lease_default_sec != NULL) {
        *lease_default_sec = 0;
        eular::ntrs::MessageGetU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_DEFAULT_SEC, lease_default_sec);
    }
    return session_token[0] != '\0';
}

bool ntrs_register_peer(int32_t control_fd, const char* peer_id, const char* device_id, const char* session_token,
                        const ntrs_nat_info_t* nat)
{
    eular::ntrs::Message req;
    eular::ntrs::Message rsp;
    if (control_fd < 0 || peer_id == NULL || device_id == NULL || nat == NULL) {
        return false;
    }

    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::REGISTER_REQ, (uint32_t)g_ntrs_req++);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::DEVICE_ID, device_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::LOCAL_IP, nat->local_ip);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::LOCAL_PORT, nat->local_port);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRFLX_IP, nat->srflx_ip);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::SRFLX_PORT, nat->srflx_port);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRFLX_IP_2, nat->srflx_ip_2);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::SRFLX_PORT_2, nat->srflx_port_2);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::MAPPING_STABLE, nat->mapping_stable);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::NAT_RISK, nat->nat_risk);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::PROBE1_OK, nat->probe1_ok);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::PROBE2_OK, nat->probe2_ok);
    eular::ntrs::MessageAddI32ByTag(&req, eular::ntrs::FieldTag::PROBE1_RTT_MS, nat->probe1_rtt_ms);
    eular::ntrs::MessageAddI32ByTag(&req, eular::ntrs::FieldTag::PROBE2_RTT_MS, nat->probe2_rtt_ms);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE_ROUNDS, (uint32_t)nat->probe_rounds);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE1_SUCCESS_COUNT,
                                    (uint32_t)nat->probe1_success_count);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE2_SUCCESS_COUNT,
                                    (uint32_t)nat->probe2_success_count);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE1_DISTINCT_MAPPINGS,
                                    (uint32_t)nat->probe1_distinct_mappings);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE2_DISTINCT_MAPPINGS,
                                    (uint32_t)nat->probe2_distinct_mappings);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::NAT_CLASS, nat->nat_class);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::NAT_FLAGS, nat->nat_flags);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::MAPPING_BEHAVIOR, nat->mapping_behavior);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::FILTERING_BEHAVIOR, nat->filtering_behavior);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::FILTER_SAME_IP_DIFF_PORT_RX,
                                     nat->filter_same_ip_diff_port_rx);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::FILTER_DIFF_IP_RX, nat->filter_diff_ip_rx);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::NAT_TYPE, nat->nat_type);

    if (!SendMessage(control_fd, req) || !RecvMessage(control_fd, &rsp)) {
        return false;
    }

    return rsp.type == eular::ntrs::MessageType::REGISTER_RSP;
}

bool ntrs_unregister_peer(int32_t control_fd, const char* peer_id, const char* session_token, const char* reason)
{
    eular::ntrs::Message req;
    eular::ntrs::ReasonCode reason_code = eular::ntrs::ReasonCode::NONE;

    if (control_fd < 0 || peer_id == NULL || peer_id[0] == '\0') {
        return false;
    }

    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::UNREGISTER_REQ, (uint32_t)g_ntrs_req++);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    if (reason != NULL && strcmp(reason, "client_exit") == 0) {
        reason_code = eular::ntrs::ReasonCode::CLIENT_EXIT;
    }
    eular::ntrs::MessageAddU8ByTag(&req, eular::ntrs::FieldTag::REASON, (uint8_t)reason_code);
    return SendMessage(control_fd, req);
}

bool ntrs_create_session(int32_t control_fd, const char* src_peer_id, const char* src_device_id,
                         const char* dst_peer_id, const char* dst_device_id, const char* session_token,
                         ntrs_session_signal_t* out)
{
    eular::ntrs::Message req;
    eular::ntrs::Message rsp;

    if (control_fd < 0 || src_peer_id == NULL || src_device_id == NULL || dst_peer_id == NULL || dst_device_id == NULL ||
        out == NULL) {
        return false;
    }

    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::SESSION_CREATE_REQ, (uint32_t)g_ntrs_req++);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRC_PEER_ID, src_peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRC_DEVICE_ID, src_device_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::DST_PEER_ID, dst_peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::DST_DEVICE_ID, dst_device_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);

    if (!SendMessage(control_fd, req) || !RecvMessage(control_fd, &rsp) ||
        rsp.type != eular::ntrs::MessageType::SESSION_CREATE_RSP) {
        return false;
    }

    return ParseSessionSignalImpl(rsp, out);
}

bool ntrs_wait_for_signal(int32_t control_fd, int32_t timeout_ms, ntrs_session_signal_t* out)
{
    eular::ntrs::Message msg;

    if (control_fd < 0 || timeout_ms <= 0 || out == NULL) {
        return false;
    }

    if (!RecvMessageTimeout(control_fd, timeout_ms, &msg)) {
        return false;
    }

    if (msg.type != eular::ntrs::MessageType::SESSION_NOTIFY &&
        msg.type != eular::ntrs::MessageType::SESSION_CREATE_RSP) {
        return false;
    }

    return ParseSessionSignalImpl(msg, out);
}

bool ntrs_try_udp_hole_punch(int32_t udp_sock, const ntrs_peer_candidate_t* candidates, uint32_t candidate_count,
                             int32_t send_rounds, int32_t select_wait_ms, ntrs_peer_candidate_t* selected)
{
    uint32_t i = 0;

    if (udp_sock < 0 || candidates == NULL || candidate_count == 0 || selected == NULL) {
        return false;
    }

    if (send_rounds <= 0) {
        send_rounds = 8;
    }
    if (select_wait_ms <= 0) {
        select_wait_ms = 200;
    }

    for (i = 0; i < candidate_count; ++i) {
        int                     round = 0;
        struct sockaddr_storage dst;
        socklen_t               dst_len = 0;

        if (!CandidateToSockaddr(&candidates[i], &dst, &dst_len)) {
            continue;
        }

        for (round = 0; round < send_rounds; ++round) {
            uint8_t        payload[256];
            size_t         payload_len = 0;
            fd_set         rfds;
            struct timeval tv;

            if (!BuildPunchFrame(NTRS_BINARY_FRAME_PUNCH_REQ, (uint64_t)(i + 1), candidates[i].type,
                                   (uint32_t)(round + 1), payload, sizeof(payload), &payload_len)) {
                continue;
            }
            sendto(udp_sock, payload, payload_len, 0, reinterpret_cast<struct sockaddr*>(&dst), dst_len);

            FD_ZERO(&rfds);
            FD_SET(udp_sock, &rfds);
            tv.tv_sec = select_wait_ms / 1000;
            tv.tv_usec = (select_wait_ms % 1000) * 1000;
            if (select(udp_sock + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(udp_sock, &rfds)) {
                uint8_t                 buffer[512];
                struct sockaddr_storage src;
                socklen_t               src_len = sizeof(src);
                ssize_t nread = recvfrom(udp_sock, buffer, sizeof(buffer) - 1, 0,
                                         reinterpret_cast<struct sockaddr*>(&src), &src_len);
                if (nread > 0) {
                    PunchFrameMeta meta;
                    if (ParsePunchFrame(buffer, (size_t)nread, &meta) &&
                        meta.frame_type == NTRS_BINARY_FRAME_PUNCH_REQ) {
                        uint8_t ack_payload[256];
                        size_t  ack_len = 0;
                        if (BuildPunchAckFrame(&meta, ack_payload, sizeof(ack_payload), &ack_len)) {
                            sendto(udp_sock, ack_payload, ack_len, 0, reinterpret_cast<struct sockaddr*>(&src),
                                   src_len);
                        }
                    }
                    if (!ParsePunchFrame(buffer, (size_t)nread, &meta) ||
                        (meta.frame_type != NTRS_BINARY_FRAME_PUNCH_REQ &&
                         meta.frame_type != NTRS_BINARY_FRAME_PUNCH_ACK)) {
                        continue;
                    }
                    memset(selected, 0, sizeof(*selected));
                    memcpy(selected, &candidates[i], sizeof(*selected));
                    return true;
                }
            }

            usleep(120000);
        }
    }

    return false;
}

ntrs_async_client_t* ntrs_async_client_create(struct event_base* base)
{
    AsyncClientImpl* impl = NULL;
    if (base == NULL) {
        return NULL;
    }
    impl = new AsyncClientImpl(base);
    impl->dns_base = evdns_base_new(base, 1);
    if (impl->dns_base == NULL) {
        delete impl;
        return NULL;
    }
    evdns_base_set_option(impl->dns_base, "timeout", "2");
    evdns_base_set_option(impl->dns_base, "attempts", "2");
    return reinterpret_cast<ntrs_async_client_t*>(impl);
}

void ntrs_async_client_destroy(ntrs_async_client_t* client)
{
    AsyncClientImpl* impl = reinterpret_cast<AsyncClientImpl*>(client);
    if (client == NULL) {
        return;
    }
    while (!impl->requests.empty()) {
        AsyncRequestFinish(impl->requests.begin()->second, true, "client destroyed");
    }
    if (impl->dns_base != NULL) {
        evdns_base_free(impl->dns_base, 1);
        impl->dns_base = NULL;
    }
    delete impl;
}

bool ntrs_async_client_cancel(ntrs_async_client_t* client, uint64_t request_id)
{
    AsyncClientImpl* impl = reinterpret_cast<AsyncClientImpl*>(client);
    if (client == NULL || request_id == 0) {
        return false;
    }
    std::map<uint64_t, AsyncRequest*>::iterator it = impl->requests.find(request_id);
    if (it == impl->requests.end()) {
        return false;
    }
    AsyncRequestFinish(it->second, true, "request cancelled");
    return true;
}

bool ntrs_async_auth(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd, const char* peer_id,
                     const char* bootstrap_token, ntrs_async_callback_t callback, void* user_data)
{
    AsyncClientImpl*     impl = reinterpret_cast<AsyncClientImpl*>(client);
    eular::ntrs::Message req;
    if (impl == NULL || peer_id == NULL || peer_id[0] == '\0' || bootstrap_token == NULL) {
        return false;
    }
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::AUTH_REQ, (uint32_t)impl->next_request_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, bootstrap_token);
    return SubmitAsyncMessageRequest(impl, control_fd, NTRS_ASYNC_AUTH, &req, true, kControlIoTimeoutMs, callback,
                                        user_data, request_id);
}

bool ntrs_async_connect_control(ntrs_async_client_t* client, uint64_t* request_id, const char* host, uint16_t port,
                                int32_t timeout_ms, ntrs_async_callback_t callback, void* user_data)
{
    return SubmitAsyncConnectRequest(reinterpret_cast<AsyncClientImpl*>(client), host, port, timeout_ms, callback,
                                        user_data, request_id);
}

bool ntrs_async_request_probe_endpoints(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                        const char* session_token, ntrs_async_callback_t callback, void* user_data)
{
    AsyncClientImpl*     impl = reinterpret_cast<AsyncClientImpl*>(client);
    eular::ntrs::Message req;
    if (impl == NULL) {
        return false;
    }
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::NAT_PROBE_REQ, (uint32_t)impl->next_request_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::VERSION, "m1");
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    return SubmitAsyncMessageRequest(impl, control_fd, NTRS_ASYNC_REQUEST_PROBE_ENDPOINTS, &req, true,
                                        kControlIoTimeoutMs, callback, user_data, request_id);
}

bool ntrs_async_detect_nat(ntrs_async_client_t* client, uint64_t* request_id, int32_t udp_sock, const char* probe1_host,
                           uint16_t probe1_port, const char* probe2_host, uint16_t probe2_port, int32_t control_fd,
                           const char* session_token, const ntrs_detect_nat_options_t* options,
                           ntrs_async_callback_t callback, void* user_data)
{
    return SubmitAsyncNatRequest(reinterpret_cast<AsyncClientImpl*>(client), udp_sock, probe1_host, probe1_port,
                                    probe2_host, probe2_port, control_fd, session_token, options, callback, user_data,
                                    request_id);
}

bool ntrs_async_register_peer(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                              const char* peer_id, const char* device_id, const char* session_token,
                              const ntrs_nat_info_t* nat, ntrs_async_callback_t callback, void* user_data)
{
    AsyncClientImpl*       impl = reinterpret_cast<AsyncClientImpl*>(client);
    eular::ntrs::Message   req;
    ntrs_nat_info_t        default_nat;
    const ntrs_nat_info_t* effective_nat = nat;

    if (impl == NULL || peer_id == NULL || device_id == NULL) {
        return false;
    }
    if (effective_nat == NULL) {
        ntrs_nat_info_init(&default_nat);
        effective_nat = &default_nat;
    }

    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::REGISTER_REQ, (uint32_t)impl->next_request_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::DEVICE_ID, device_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::LOCAL_IP, effective_nat->local_ip);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::LOCAL_PORT, effective_nat->local_port);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRFLX_IP, effective_nat->srflx_ip);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::SRFLX_PORT, effective_nat->srflx_port);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRFLX_IP_2, effective_nat->srflx_ip_2);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::SRFLX_PORT_2, effective_nat->srflx_port_2);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::MAPPING_STABLE, effective_nat->mapping_stable);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::NAT_RISK, effective_nat->nat_risk);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::PROBE1_OK, effective_nat->probe1_ok);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::PROBE2_OK, effective_nat->probe2_ok);
    eular::ntrs::MessageAddI32ByTag(&req, eular::ntrs::FieldTag::PROBE1_RTT_MS, effective_nat->probe1_rtt_ms);
    eular::ntrs::MessageAddI32ByTag(&req, eular::ntrs::FieldTag::PROBE2_RTT_MS, effective_nat->probe2_rtt_ms);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE_ROUNDS,
                                    (uint32_t)effective_nat->probe_rounds);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE1_SUCCESS_COUNT,
                                    (uint32_t)effective_nat->probe1_success_count);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE2_SUCCESS_COUNT,
                                    (uint32_t)effective_nat->probe2_success_count);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE1_DISTINCT_MAPPINGS,
                                    (uint32_t)effective_nat->probe1_distinct_mappings);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::PROBE2_DISTINCT_MAPPINGS,
                                    (uint32_t)effective_nat->probe2_distinct_mappings);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::NAT_CLASS, effective_nat->nat_class);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::NAT_FLAGS, effective_nat->nat_flags);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::MAPPING_BEHAVIOR,
                                    effective_nat->mapping_behavior);
    eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::FILTERING_BEHAVIOR,
                                    effective_nat->filtering_behavior);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::FILTER_SAME_IP_DIFF_PORT_RX,
                                     effective_nat->filter_same_ip_diff_port_rx);
    eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::FILTER_DIFF_IP_RX,
                                     effective_nat->filter_diff_ip_rx);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::NAT_TYPE, effective_nat->nat_type);
    return SubmitAsyncMessageRequest(impl, control_fd, NTRS_ASYNC_REGISTER_PEER, &req, true, kControlIoTimeoutMs,
                                        callback, user_data, request_id);
}

bool ntrs_async_unregister_peer(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                const char* peer_id, const char* session_token, const char* reason,
                                ntrs_async_callback_t callback, void* user_data)
{
    AsyncClientImpl*     impl = reinterpret_cast<AsyncClientImpl*>(client);
    eular::ntrs::Message req;
    eular::ntrs::ReasonCode reason_code = eular::ntrs::ReasonCode::NONE;
    if (impl == NULL || peer_id == NULL || peer_id[0] == '\0') {
        return false;
    }
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::UNREGISTER_REQ, (uint32_t)impl->next_request_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    if (reason != NULL && strcmp(reason, "client_exit") == 0) {
        reason_code = eular::ntrs::ReasonCode::CLIENT_EXIT;
    }
    eular::ntrs::MessageAddU8ByTag(&req, eular::ntrs::FieldTag::REASON, (uint8_t)reason_code);
    return SubmitAsyncMessageRequest(impl, control_fd, NTRS_ASYNC_UNREGISTER_PEER, &req, false, kControlIoTimeoutMs,
                                        callback, user_data, request_id);
}

bool ntrs_async_heartbeat(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd, const char* peer_id,
                          const char* session_token, uint32_t lease_seq, ntrs_async_callback_t callback,
                          void* user_data)
{
    AsyncClientImpl*     impl = reinterpret_cast<AsyncClientImpl*>(client);
    eular::ntrs::Message req;
    if (impl == NULL || peer_id == NULL || peer_id[0] == '\0') {
        return false;
    }
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::HEARTBEAT_REQ, (uint32_t)impl->next_request_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::LEASE_SEQ, lease_seq);
    return SubmitAsyncMessageRequest(impl, control_fd, NTRS_ASYNC_HEARTBEAT, &req, true, kControlIoTimeoutMs,
                                        callback, user_data, request_id);
}

bool ntrs_async_create_session(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                               const char* src_peer_id, const char* src_device_id, const char* dst_peer_id,
                               const char* dst_device_id, const char* session_token, ntrs_async_callback_t callback,
                               void* user_data)
{
    AsyncClientImpl*     impl = reinterpret_cast<AsyncClientImpl*>(client);
    eular::ntrs::Message req;
    if (impl == NULL || src_peer_id == NULL || src_device_id == NULL || dst_peer_id == NULL ||
        dst_device_id == NULL) {
        return false;
    }
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::SESSION_CREATE_REQ, (uint32_t)impl->next_request_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRC_PEER_ID, src_peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRC_DEVICE_ID, src_device_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::DST_PEER_ID, dst_peer_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::DST_DEVICE_ID, dst_device_id);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, session_token == NULL ? "" : session_token);
    return SubmitAsyncMessageRequest(impl, control_fd, NTRS_ASYNC_CREATE_SESSION, &req, true, kControlIoTimeoutMs,
                                        callback, user_data, request_id);
}

bool ntrs_async_wait_for_signal(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                int32_t timeout_ms, ntrs_async_callback_t callback, void* user_data)
{
    return SubmitAsyncMessageRequest(reinterpret_cast<AsyncClientImpl*>(client), control_fd,
                                        NTRS_ASYNC_WAIT_FOR_SIGNAL, NULL, true, timeout_ms, callback, user_data,
                                        request_id);
}

bool ntrs_async_try_udp_hole_punch(ntrs_async_client_t* client, uint64_t* request_id, int32_t udp_sock,
                                   const ntrs_peer_candidate_t* candidates, uint32_t candidate_count,
                                   int32_t send_rounds, int32_t interval_ms, ntrs_async_callback_t callback,
                                   void* user_data)
{
    return SubmitAsyncPunchRequest(reinterpret_cast<AsyncClientImpl*>(client), udp_sock, candidates, candidate_count,
                                      send_rounds, interval_ms, callback, user_data, request_id);
}

}  // extern "C"
