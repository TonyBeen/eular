#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <ntrs_auth.h>
#include <ntrs_binary_protocol.h>
#include <ntrs_client.h>
#include <ntrs_codec.h>
#include <ntrs_io.h>
#include <socket_address.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <ctime>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/socket.h>

static void NodeVerboseLog(bool verbose, const char* fmt, ...)
{
    va_list args;

    if (!verbose || fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/**
 * @brief 将控制节点列表格式化为便于日志观察的字符串。
 *
 * @param controls 控制节点端点列表。
 * @return std::string 逗号分隔后的端点文本；若为空则返回 "-"。
 */
static std::string JoinControlEndpoints(const std::vector<std::string>& controls)
{
    std::string text;

    if (controls.empty()) {
        return "-";
    }
    for (size_t i = 0; i < controls.size(); ++i) {
        if (i > 0) {
            text.append(",");
        }
        text.append(controls[i]);
    }
    return text;
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

/**
 * @brief 生成 `peer_id + device_id` 组合键。
 *
 * 节点内部允许同一 `peer_id` 下并存多个设备，因此注册表必须使用组合键而不是
 * 单独的 `peer_id`。
 *
 * @param peer_id 逻辑身份。
 * @param device_id 具体设备。
 * @return std::string 组合键。
 */
static std::string MakePeerKey(const std::string& peer_id, const std::string& device_id)
{
    return peer_id + "\n" + device_id;
}

/**
 * @brief 生成便于日志展示的 `peer/device` 文本。
 *
 * @param peer_id 逻辑身份。
 * @param device_id 具体设备。
 * @return std::string 展示文本。
 */
static std::string FormatPeerDevice(const std::string& peer_id, const std::string& device_id)
{
    if (device_id.empty()) {
        return peer_id + "/-";
    }
    return peer_id + "/" + device_id;
}

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

static uint32_t NatStrictnessScore(ntrs_nat_class_t nat_class)
{
    switch (nat_class) {
    case NTRS_NAT_CLASS_OPEN_PUBLIC:
        return 1;
    case NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL:
        return 2;
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

static SessionStrategyPlan BuildSessionStrategy(const PeerSession& src, const PeerSession& dst)
{
    SessionStrategyPlan plan;
    uint32_t            src_score = NatStrictnessScore(src.nat_class);
    uint32_t            dst_score = NatStrictnessScore(dst.nat_class);

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
    FETCH_PROBE_ENDPOINT,
    SEND_PROBE,
    SEND_PROBE_DELEGATE,
};

struct AsyncFederationJob {
    AsyncFederationJobType   type;
    bool                     verbose;
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
    /**
     * @brief 联邦探测任务携带的私有 probe token。
     *
     * 节点内部统一按私有探测 token 处理，不承载任何公开标准协议 transaction-id
     * 语义。
     */
    std::vector<uint8_t>     probe_token_bytes;
    bool                     use_alt_port;
};

struct AsyncFederationResult {
    AsyncFederationJobType type;
    int                    fd;
    uint64_t               client_generation;
    uint32_t               request_id;
    bool                   ok;
    std::string            selected_control;
    std::string            peer_probe_endpoint;
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

static void OnProcessSignal(int) { g_stop = 1; }

static bool ParseEndpoint(const std::string& input, std::string* host, uint16_t* port)
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

static uint16_t EndpointPort(const std::string& endpoint)
{
    std::string host;
    uint16_t    port = 0;
    if (!ParseEndpoint(endpoint, &host, &port)) {
        return 0;
    }
    return port;
}

static std::string FormatEndpoint(const std::string& host, uint16_t port)
{
    if (host.find(':') != std::string::npos && (host.empty() || host[0] != '[')) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

static bool IsIpv6Literal(const std::string& host)
{
    struct in6_addr addr6;
    return inet_pton(AF_INET6, host.c_str(), &addr6) == 1;
}

static bool IsUsableInterfaceIpv6(const struct in6_addr& addr)
{
    static const struct in6_addr kAny = IN6ADDR_ANY_INIT;
    static const struct in6_addr kLoopback = IN6ADDR_LOOPBACK_INIT;
    const uint8_t*               bytes = reinterpret_cast<const uint8_t*>(&addr);

    if (memcmp(&addr, &kAny, sizeof(addr)) == 0 || memcmp(&addr, &kLoopback, sizeof(addr)) == 0) {
        return false;
    }
    return !(bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80);
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

/**
 * @brief 打印更清晰的命令行帮助信息。
 *
 * @param program 当前程序名。
 */
static void PrintUsage(const char* program)
{
    printf(
        "Usage:\n"
        "  %s [OPTIONS]\n"
        "\n"
        "Required:\n"
        "  --hub TEXT                 NTRS hub endpoint, format: host:port\n"
        "  --node-id TEXT             Node identifier\n"
        "  --public-host TEXT         Public IPv4/IPv6/domain advertised to peers\n"
        "\n"
        "Options:\n"
        "  --control-port UINT        Control listen port [default: 19000]\n"
        "  --probe-port UINT          Primary probe UDP port [default: 33478]\n"
        "  --probe-alt-port UINT      Alternate probe UDP port [default: 33479]\n"
        "  --bind-ip TEXT             Bind probe UDP sockets to a local IP address\n"
        "  --bind-device TEXT         Bind probe UDP sockets to a network interface\n"
        "  -4, --ipv4                 Use IPv4 UDP probing [default]\n"
        "  -6, --ipv6                 Use IPv6 UDP probing\n"
        "  --region TEXT              Region label [default: default]\n"
        "  --auth-secret TEXT         Control and hub auth secret [default: ntrs-dev-secret]\n"
        "  --verbose, -v              Enable verbose logs\n"
        "  --help, -h                 Show this help message\n"
        "\n"
        "Long option forms:\n"
        "  --name value               Space separated form\n"
        "  --name=value               Equals form\n"
        "\n"
        "Examples:\n"
        "  %s --hub bd.eular.top:18083 --node-id node-a --public-host 120.48.107.15 --verbose\n"
        "  %s --hub=bd.eular.top:18083 --node-id=node-a --public-host=120.48.107.15\n"
        "  %s -6 --hub bd.eular.top:18083 --node-id node-v6 --public-host 2001:db8::10\n"
        "\n"
        "Legacy positional mode:\n"
        "  %s <control_port> <self_probe_host:port> [peer_node_control_host:port]\n"
        "     [hub_host] [hub_port] [node_id] [region] [auth_secret]\n",
        program, program, program, program, program);
}

/**
 * @brief 判断长选项是否匹配指定名字，并提取其值。
 *
 * @param arg 当前参数文本。
 * @param name 期望的长选项名字，例如 "--hub"。
 * @param next_value 下一个 argv 值；若不存在则传入 NULL。
 * @param consumed_next 输出是否消费了下一个参数。
 * @param out_value 输出参数值。
 * @return true 参数名字匹配。
 * @return false 参数名字不匹配。
 */
static bool MatchLongOption(const std::string& arg, const char* name, const char* next_value, bool* consumed_next,
                              std::string* out_value)
{
    const size_t name_len = strlen(name);

    if (consumed_next == NULL || out_value == NULL) {
        return false;
    }
    *consumed_next = false;
    out_value->clear();
    if (arg == name) {
        if (next_value != NULL) {
            *out_value = next_value;
            *consumed_next = true;
        }
        return true;
    }
    if (arg.size() > name_len && arg.compare(0, name_len, name) == 0 && arg[name_len] == '=') {
        *out_value = arg.substr(name_len + 1);
        return true;
    }
    return false;
}

/**
 * @brief 打印缺失参数值错误。
 *
 * @param option 选项名字。
 */
static void PrintMissingOptionValue(const char* option)
{
    printf("missing value for %s\n", option);
}

static std::string NowIso8601()
{
    time_t    t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
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

static bool MsgBoolTag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, bool default_value = false)
{
    bool value = default_value;
    if (eular::ntrs::MessageGetBoolByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static int32_t MsgI32Tag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag, int32_t default_value = 0)
{
    int32_t value = default_value;
    if (eular::ntrs::MessageGetI32ByTag(&msg, tag, &value)) {
        return value;
    }
    return default_value;
}

static bool ResolveIpv4Literal(const std::string& host, std::string* ip_out)
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

static bool ResolveProbeHostLiteral(const std::string& host, int address_family, std::string* ip_out)
{
    struct addrinfo  hints;
    struct addrinfo* result = NULL;

    if (ip_out == NULL || host.empty()) {
        return false;
    }

    if (address_family == AF_INET) {
        return ResolveIpv4Literal(host, ip_out);
    }

    struct in6_addr addr6;
    if (address_family == AF_INET6 && inet_pton(AF_INET6, host.c_str(), &addr6) == 1) {
        *ip_out = host;
        return true;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = address_family;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), NULL, &hints, &result) != 0 || result == NULL) {
        return false;
    }

    char host_buffer[NI_MAXHOST] = {0};
    bool ok = getnameinfo(result->ai_addr, result->ai_addrlen, host_buffer, sizeof(host_buffer), NULL, 0,
                          NI_NUMERICHOST) == 0;
    if (ok) {
        *ip_out = host_buffer;
    }
    freeaddrinfo(result);
    return ok && !ip_out->empty();
}

static bool ResolveProbeOtherAddress(const std::vector<std::string>& probe_controls, uint16_t probe_port,
                                        int address_family, std::string* other_ip, uint16_t* other_port)
{
    size_t i = 0;

    if (other_ip == NULL || other_port == NULL) {
        return false;
    }

    for (i = 0; i < probe_controls.size(); ++i) {
        std::string host;
        uint16_t    port = 0;
        std::string resolved_ip;

        if (!ParseEndpoint(probe_controls[i], &host, &port)) {
            continue;
        }
        if (!ResolveProbeHostLiteral(host, address_family, &resolved_ip)) {
            resolved_ip = host;
        }
        if (!resolved_ip.empty()) {
            *other_ip = resolved_ip;
            *other_port = probe_port;
            return true;
        }
    }

    return false;
}

static eular::ntrs::Message BuildNodePresenceMessage(const std::string& node_id,
                                                     const std::string& boot_id,
                                                     eular::ntrs::NodeStatusCode status_code,
                                                     eular::ntrs::ReasonCode reason_code,
                                                     uint64_t request_id)
{
    eular::ntrs::Message msg;

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::NODE_PRESENCE, (uint32_t)request_id);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::STATUS, (uint8_t)status_code);
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::REASON, (uint8_t)reason_code);
    const std::string ts = NowIso8601();
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    return msg;
}

static void AppendUnique(std::vector<std::string>* out, const std::string& value)
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

/**
 * @brief 在注册表中按 `peer_id + device_id` 精确查找设备。
 *
 * @param peers 在线设备表。
 * @param peer_id 逻辑身份。
 * @param device_id 设备标识。
 * @return std::map<std::string, PeerSession>::iterator 查找到的设备。
 */
static std::map<std::string, PeerSession>::iterator FindPeerExact(std::map<std::string, PeerSession>* peers,
                                                                    const std::string& peer_id,
                                                                    const std::string& device_id)
{
    if (peers == NULL) {
        return std::map<std::string, PeerSession>::iterator();
    }
    return peers->find(MakePeerKey(peer_id, device_id));
}

/**
 * @brief 在注册表中根据 `peer_id` 和连接 fd 查找本端设备。
 *
 * 该辅助函数用于 `HEARTBEAT_REQ/UNREGISTER_REQ` 等历史消息，因为它们当前不携带
 * `device_id`，只能结合控制连接和 `peer_id` 反查唯一设备。
 *
 * @param peers 在线设备表。
 * @param peer_id 逻辑身份。
 * @param fd 当前控制连接。
 * @return std::map<std::string, PeerSession>::iterator 查找到的设备。
 */
static std::map<std::string, PeerSession>::iterator FindPeerByFdAndPeerId(
    std::map<std::string, PeerSession>* peers, const std::string& peer_id, int fd)
{
    if (peers == NULL) {
        return std::map<std::string, PeerSession>::iterator();
    }
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end(); ++it) {
        if (it->second.peer_id == peer_id && it->second.fd == fd) {
            return it;
        }
    }
    return peers->end();
}

/**
 * @brief 仅按 `peer_id` 查找唯一在线设备。
 *
 * 如果同一 `peer_id` 下存在多个设备，则返回 `end()` 并将 `ambiguous` 置为 true。
 *
 * @param peers 在线设备表。
 * @param peer_id 逻辑身份。
 * @param ambiguous 输出是否存在歧义。
 * @return std::map<std::string, PeerSession>::iterator 唯一设备或 `end()`。
 */
static std::map<std::string, PeerSession>::iterator FindUniquePeerByPeerId(
    std::map<std::string, PeerSession>* peers, const std::string& peer_id, bool* ambiguous)
{
    std::map<std::string, PeerSession>::iterator found;
    bool                                        has_found = false;

    if (ambiguous != NULL) {
        *ambiguous = false;
    }
    if (peers == NULL) {
        return std::map<std::string, PeerSession>::iterator();
    }

    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end(); ++it) {
        if (it->second.peer_id != peer_id) {
            continue;
        }
        if (!has_found) {
            found = it;
            has_found = true;
            continue;
        }
        if (ambiguous != NULL) {
            *ambiguous = true;
        }
        return peers->end();
    }

    return has_found ? found : peers->end();
}

static std::vector<std::string> BuildProbeControls(const std::string& p1, const std::string& p2,
                                                     const std::string& b1, const std::string& fallback)
{
    std::vector<std::string> out;
    AppendUnique(&out, p1);
    AppendUnique(&out, p2);
    AppendUnique(&out, b1);
    AppendUnique(&out, fallback);
    return out;
}

static void ErasePeerForFd(std::map<std::string, PeerSession>* peers, int fd)
{
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end();) {
        if (it->second.fd == fd) {
            printf("peer offline: %s\n", FormatPeerDevice(it->second.peer_id, it->second.device_id).c_str());
            std::map<std::string, PeerSession>::iterator to_erase = it++;
            peers->erase(to_erase);
        } else {
            ++it;
        }
    }
}

static void SweepExpiredPeers(std::map<std::string, PeerSession>* peers)
{
    time_t now = time(NULL);
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end();) {
        if (it->second.expire_at <= now) {
            printf("peer lease expired: %s\n", FormatPeerDevice(it->second.peer_id, it->second.device_id).c_str());
            std::map<std::string, PeerSession>::iterator to_erase = it++;
            peers->erase(to_erase);
        } else {
            ++it;
        }
    }
}

static bool SendAll(int fd, const void* buf, size_t len)
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

static bool SetNonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool SendMessage(int fd, const eular::ntrs::Message& msg)
{
    uint8_t buf[8192];
    size_t  len = 0;
    if (eular::ntrs::EncodeMessage(msg, buf, sizeof(buf), &len) != 0) {
        return false;
    }
    return SendAll(fd, buf, len);
}

struct HubConnection {
    int                  fd;
    bool                 connecting;
    bool                 auth_sent;
    bool                 authed;
    bool                 registered;
    std::string          session_token;
    ControlClientRxState rx_state;
    uint32_t             next_request_id;
    time_t               next_reconnect_ts;
    time_t               last_heartbeat_ts;

    HubConnection()
        : fd(-1),
          connecting(false),
          auth_sent(false),
          authed(false),
          registered(false),
          next_request_id(1),
          next_reconnect_ts(0),
          last_heartbeat_ts(0)
    {
    }
};

static bool DrainControlMessages(int fd, ControlClientRxState* state, std::deque<eular::ntrs::Message>* messages,
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
        uint32_t frame_size = 0;

        if (!eular::ntrs::FrameSizeFromHeader(state->buffer.data(), eular::ntrs::FRAME_HDR_SIZE, &frame_size) ||
            frame_size < eular::ntrs::FRAME_HDR_SIZE || frame_size > 8192u) {
            *peer_closed = true;
            return false;
        }
        if (state->buffer.size() < frame_size) {
            break;
        }
        messages->emplace_back();
        if (!eular::ntrs::DecodeMessage(state->buffer.data(), frame_size, &messages->back())) {
            messages->pop_back();
            *peer_closed = true;
            return false;
        }
        state->buffer.erase(state->buffer.begin(), state->buffer.begin() + frame_size);
    }

    return true;
}

static void CloseHubConnection(HubConnection* hub)
{
    if (hub == NULL) {
        return;
    }
    if (hub->fd >= 0) {
        close(hub->fd);
    }
    hub->fd = -1;
    hub->connecting = false;
    hub->auth_sent = false;
    hub->authed = false;
    hub->registered = false;
    hub->session_token.clear();
    hub->rx_state.buffer.clear();
    hub->next_reconnect_ts = time(NULL) + 3;
    hub->last_heartbeat_ts = 0;
}

static bool ConnectHub(HubConnection* hub, const std::string& hub_host, uint16_t hub_port, bool verbose)
{
    struct addrinfo  hints;
    struct addrinfo* result = NULL;

    if (hub == NULL || hub_host.empty() || hub_port == 0 || hub->fd >= 0) {
        return false;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)hub_port);
    if (getaddrinfo(hub_host.c_str(), port_text, &hints, &result) != 0 || result == NULL) {
        hub->next_reconnect_ts = time(NULL) + 3;
        return false;
    }

    for (struct addrinfo* it = result; it != NULL; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        SetNonblocking(fd);
        int rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS) {
            hub->fd = fd;
            hub->connecting = (rc != 0);
            hub->auth_sent = false;
            hub->authed = false;
            hub->registered = false;
            hub->session_token.clear();
            hub->rx_state.buffer.clear();
            hub->last_heartbeat_ts = 0;
            NodeVerboseLog(verbose, "hub connecting endpoint=%s fd=%d\n",
                           FormatEndpoint(hub_host, hub_port).c_str(), fd);
            freeaddrinfo(result);
            return true;
        }
        close(fd);
    }

    freeaddrinfo(result);
    hub->next_reconnect_ts = time(NULL) + 3;
    return false;
}

static bool SendHubAuth(HubConnection* hub, const std::string& node_id, const std::string& auth_secret)
{
    eular::ntrs::Message msg;

    if (hub == NULL || hub->fd < 0 || node_id.empty()) {
        return false;
    }
    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::AUTH_REQ, hub->next_request_id++);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::PEER_ID, node_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TOKEN, auth_secret.c_str());
    return SendMessage(hub->fd, msg);
}

static eular::ntrs::Message BuildNodeRegisterMessage(const std::string& node_id,
                                                     const std::string& boot_id,
                                                     const std::string& region,
                                                     const std::string& self_probe_endpoint,
                                                     const std::string& self_control_endpoint,
                                                     uint32_t request_id)
{
    eular::ntrs::Message msg;
    const std::string    ts = NowIso8601();

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::NODE_REGISTER, request_id);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::REGION, region.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::PROBE_ENDPOINT, self_probe_endpoint.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::CONTROL_ENDPOINT, self_control_endpoint.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NAT_TYPE, "service_node");
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 5);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    return msg;
}

static eular::ntrs::Message BuildNodeHeartbeatMessage(const std::string& node_id,
                                                      const std::string& boot_id,
                                                      uint32_t assignment_version,
                                                      uint32_t load,
                                                      uint32_t request_id)
{
    eular::ntrs::Message msg;
    const std::string    ts = NowIso8601();

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::NODE_HEARTBEAT, request_id);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::STATUS,
                                   (uint8_t)eular::ntrs::NodeStatusCode::ONLINE);
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::LOAD, load);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NAT_TYPE, "service_node");
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 5);
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::ASSIGNMENT_VERSION, assignment_version);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    return msg;
}

/**
 * @brief 创建内建私有 NAT 探测端口。
 *
 * @param self_probe_endpoint 节点对外公布的主探测端点。
 * @return int 创建成功返回 UDP socket，失败返回 -1。
 */
static int CreateProbeSocket(const std::string& self_probe_endpoint, int address_family, const std::string& bind_ip,
                             const std::string& bind_device)
{
    uint16_t port = EndpointPort(self_probe_endpoint);
    std::string resolved_ip;

    if (port == 0) {
        return -1;
    }
    if (!ResolveBindIp(bind_ip, bind_device, address_family, &resolved_ip)) {
        return -1;
    }

    int fd = socket(address_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (!ApplyBindDeviceIfSupported(fd, bind_device)) {
        close(fd);
        return -1;
    }

    if (address_family == AF_INET6) {
        int v6_only = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        if (!resolved_ip.empty()) {
            if (inet_pton(AF_INET6, resolved_ip.c_str(), &addr6.sin6_addr) != 1) {
                close(fd);
                return -1;
            }
        } else {
            addr6.sin6_addr = in6addr_any;
        }
        if (bind(fd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        if (!resolved_ip.empty()) {
            if (inet_pton(AF_INET, resolved_ip.c_str(), &addr4.sin_addr) != 1) {
                close(fd);
                return -1;
            }
        } else {
            addr4.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        if (bind(fd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
            close(fd);
            return -1;
        }
    }

    return fd;
}

/**
 * @brief 创建备用探测端口。
 *
 * @param port 备用探测端口号。
 * @return int 创建成功返回 UDP socket，失败返回 -1。
 */
static int CreateProbeSocketWithPort(uint16_t port, int address_family, const std::string& bind_ip,
                                     const std::string& bind_device)
{
    std::string resolved_ip;

    if (port == 0) {
        return -1;
    }
    if (!ResolveBindIp(bind_ip, bind_device, address_family, &resolved_ip)) {
        return -1;
    }

    int fd = socket(address_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (!ApplyBindDeviceIfSupported(fd, bind_device)) {
        close(fd);
        return -1;
    }

    if (address_family == AF_INET6) {
        int v6_only = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        if (!resolved_ip.empty()) {
            if (inet_pton(AF_INET6, resolved_ip.c_str(), &addr6.sin6_addr) != 1) {
                close(fd);
                return -1;
            }
        } else {
            addr6.sin6_addr = in6addr_any;
        }
        if (bind(fd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        if (!resolved_ip.empty()) {
            if (inet_pton(AF_INET, resolved_ip.c_str(), &addr4.sin_addr) != 1) {
                close(fd);
                return -1;
            }
        } else {
            addr4.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        if (bind(fd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
            close(fd);
            return -1;
        }
    }

    return fd;
}

static bool MakeSockaddrStorage(const std::string& ip, uint16_t port, struct sockaddr_storage* out, socklen_t* out_len)
{
    if (out == NULL || out_len == NULL || ip.empty() || port == 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    struct sockaddr_in* addr4 = reinterpret_cast<struct sockaddr_in*>(out);
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr4->sin_addr) == 1) {
        *out_len = static_cast<socklen_t>(sizeof(*addr4));
        return true;
    }

    memset(out, 0, sizeof(*out));
    struct sockaddr_in6* addr6 = reinterpret_cast<struct sockaddr_in6*>(out);
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(port);
    if (inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr) == 1) {
        *out_len = static_cast<socklen_t>(sizeof(*addr6));
        return true;
    }

    memset(out, 0, sizeof(*out));
    *out_len = 0;
    return false;
}

static bool SockaddrHostPort(const struct sockaddr* addr, socklen_t addr_len, std::string* host, uint16_t* port)
{
    char host_buffer[NI_MAXHOST] = {0};
    char port_buffer[NI_MAXSERV] = {0};

    if (addr == NULL || host == NULL || port == NULL ||
        getnameinfo(addr, addr_len, host_buffer, sizeof(host_buffer), port_buffer, sizeof(port_buffer),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }

    *host = host_buffer;
    *port = static_cast<uint16_t>(atoi(port_buffer));
    return !host->empty() && *port != 0;
}

static std::string EndpointText(const struct sockaddr* addr, socklen_t addr_len)
{
    std::string host;
    uint16_t    port = 0;

    if (!SockaddrHostPort(addr, addr_len, &host, &port)) {
        return "-";
    }
    return FormatEndpoint(host, port);
}

static bool SendUdpFilterProbe(int udp_fd, const std::string& target_ip, uint16_t target_port,
                                  const std::string& token, const std::string& tag)
{
    static const int kFilterProbeBurstCount = 3;
    if (udp_fd < 0 || target_ip.empty() || target_port == 0 || token.empty()) {
        return false;
    }

    struct sockaddr_storage dst;
    socklen_t               dst_len = 0;
    if (!MakeSockaddrStorage(target_ip, target_port, &dst, &dst_len)) {
        return false;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "NTRS_FILTER_PROBE|%s|%s", token.c_str(), tag.c_str());
    for (int i = 0; i < kFilterProbeBurstCount; ++i) {
        ssize_t n = sendto(udp_fd, payload, strlen(payload), 0, reinterpret_cast<struct sockaddr*>(&dst), dst_len);
        if (n <= 0) {
            return false;
        }
    }
    return true;
}

static bool PeerIpString(int fd, std::string* ip_out)
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

static void InitFederationResult(FederationRequest* request)
{
    request->result.type = request->job.type;
    request->result.fd = request->job.fd;
    request->result.client_generation = request->job.client_generation;
    request->result.request_id = request->job.request_id;
    request->result.ok = false;
    request->result.same_ip_diff_port = request->job.same_ip_diff_port;
    request->result.diff_ip = false;
}

/**
 * @brief 解析私有二进制 NAT 探测请求帧。
 *
 * @param buffer 输入缓冲区。
 * @param len 输入长度。
 * @param frame_type 输出帧类型。
 * @param phase 输出阶段。
 * @param token 输出探测 token。
 * @param token_len 输出 token 长度。
 * @return true 为合法的私有探测请求。
 * @return false 不是合法的私有探测请求。
 */
static bool ParseBinaryProbeRequest(const uint8_t* buffer, size_t len, ntrs_binary_frame_type_t* frame_type,
                                       ntrs_binary_phase_t* phase, uint8_t* token, uint16_t* token_len)
{
    ntrs_binary_frame_view_t frame_view;
    ntrs_binary_tlv_view_t   token_tlv;

    if (buffer == NULL || frame_type == NULL || phase == NULL || token == NULL || token_len == NULL ||
        !ntrs_binary_frame_parse(buffer, len, &frame_view)) {
        return false;
    }
    if (frame_view.header.frame_type != NTRS_BINARY_FRAME_PROBE_REQ &&
        frame_view.header.frame_type != NTRS_BINARY_FRAME_FILTER_REQ) {
        return false;
    }
    if (!ntrs_binary_frame_find_tlv(&frame_view, NTRS_BINARY_TLV_PROBE_TOKEN, &token_tlv) ||
        token_tlv.value_len == 0 || token_tlv.value_len > NTRS_PROBE_TOKEN_WIRE_SIZE) {
        return false;
    }

    *frame_type = static_cast<ntrs_binary_frame_type_t>(frame_view.header.frame_type);
    *phase = static_cast<ntrs_binary_phase_t>(frame_view.header.phase);
    memcpy(token, token_tlv.value, token_tlv.value_len);
    *token_len = token_tlv.value_len;
    return true;
}

/**
 * @brief 构造私有二进制 NAT 探测响应帧。
 *
 * @param frame_type 请求帧类型。
 * @param phase 当前探测阶段。
 * @param token 探测 token。
 * @param token_len 探测 token 长度。
 * @param mapped_addr 对端观察到的映射地址。
 * @param response_origin_ip 当前响应出口地址。
 * @param response_origin_port 当前响应出口端口。
 * @param other_address_ip 另一探测端点地址。
 * @param other_address_port 另一探测端点端口。
 * @param out 输出编码结果。
 * @return true 构造成功。
 * @return false 构造失败。
 */
static bool BuildBinaryProbeResponse(ntrs_binary_frame_type_t frame_type, ntrs_binary_phase_t phase,
                                        const uint8_t* token, uint16_t token_len,
                                        const struct sockaddr* mapped_addr, socklen_t mapped_addr_len,
                                        const std::string& response_origin_ip, uint16_t response_origin_port,
                                        const std::string& other_address_ip, uint16_t other_address_port,
                                        std::vector<uint8_t>* out)
{
    uint8_t                  buffer[256];
    ntrs_binary_frame_t      frame;
    ntrs_binary_frame_type_t response_type;
    struct sockaddr_storage  origin_addr;
    socklen_t                origin_addr_len = 0;
    struct sockaddr_storage  other_addr;
    socklen_t                other_addr_len = 0;
    bool                     has_other = false;

    if (token == NULL || token_len == 0 || mapped_addr == NULL || out == NULL || response_origin_ip.empty()) {
        return false;
    }

    response_type = frame_type == NTRS_BINARY_FRAME_FILTER_REQ ? NTRS_BINARY_FRAME_FILTER_RSP
                                                               : NTRS_BINARY_FRAME_PROBE_RSP;
    if (!ntrs_binary_frame_init(&frame, buffer, sizeof(buffer)) ||
        !ntrs_binary_frame_set_header(&frame, response_type, phase, 0, 0, 0, (uint64_t)time(NULL) * 1000ull) ||
        !ntrs_binary_frame_add_tlv(&frame, NTRS_BINARY_TLV_PROBE_TOKEN, token, token_len) ||
        !ntrs_binary_frame_add_endpoint_tlv(&frame, NTRS_BINARY_TLV_MAPPED_ADDR,
                                            reinterpret_cast<const struct sockaddr*>(mapped_addr),
                                            mapped_addr_len) ||
        !MakeSockaddrStorage(response_origin_ip, response_origin_port, &origin_addr, &origin_addr_len) ||
        !ntrs_binary_frame_add_endpoint_tlv(&frame, NTRS_BINARY_TLV_ORIGIN_ADDR,
                                            reinterpret_cast<const struct sockaddr*>(&origin_addr),
                                            origin_addr_len)) {
        return false;
    }

    has_other = !other_address_ip.empty() && other_address_port != 0 &&
                MakeSockaddrStorage(other_address_ip, other_address_port, &other_addr, &other_addr_len);
    if (has_other &&
        !ntrs_binary_frame_add_endpoint_tlv(&frame, NTRS_BINARY_TLV_OTHER_ADDR,
                                            reinterpret_cast<const struct sockaddr*>(&other_addr),
                                            other_addr_len)) {
        return false;
    }

    out->assign(frame.buffer, frame.buffer + frame.length);
    return true;
}

/**
 * @brief 发送私有二进制 NAT 探测响应。
 *
 * @param udp_fd 响应套接字。
 * @param target_addr 目标地址。
 * @param frame_type 请求帧类型。
 * @param phase 探测阶段。
 * @param token 探测 token。
 * @param token_len 探测 token 长度。
 * @param mapped_addr 对端观察到的映射地址。
 * @param response_origin_ip 当前响应出口地址。
 * @param response_origin_port 当前响应出口端口。
 * @param other_address_ip 另一探测端点地址。
 * @param other_address_port 另一探测端点端口。
 * @return true 发送成功。
 * @return false 发送失败。
 */
static bool SendBinaryProbeResponse(int udp_fd, const struct sockaddr* target_addr, socklen_t target_addr_len,
                                       ntrs_binary_frame_type_t frame_type, ntrs_binary_phase_t phase,
                                       const uint8_t* token, uint16_t token_len, const struct sockaddr* mapped_addr,
                                       socklen_t mapped_addr_len,
                                       const std::string& response_origin_ip, uint16_t response_origin_port,
                                       const std::string& other_address_ip, uint16_t other_address_port)
{
    std::vector<uint8_t> rsp;

    if (udp_fd < 0 || target_addr == NULL || mapped_addr == NULL) {
        return false;
    }
    if (!BuildBinaryProbeResponse(frame_type, phase, token, token_len, mapped_addr, mapped_addr_len, response_origin_ip,
                                     response_origin_port, other_address_ip, other_address_port, &rsp)) {
        return false;
    }

    return sendto(udp_fd, rsp.data(), rsp.size(), 0, target_addr, target_addr_len) > 0;
}

static bool EncodeTxMessage(const eular::ntrs::Message& msg, std::vector<uint8_t>* out)
{
    uint8_t buf[8192];
    size_t  len = 0;
    if (out == NULL || eular::ntrs::EncodeMessage(msg, buf, sizeof(buf), &len) != 0) {
        return false;
    }
    out->assign(buf, buf + len);
    return true;
}

static void FederationDnsCb(int result, struct evutil_addrinfo* res, void* arg)
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

static bool StartFederationAttempt(FederationRequest* request, struct evdns_base* dns_base)
{
    /*
     * TODO: 当前 node-node federation 每个协助任务都会临时建立 TCP 连接并重新认证，
     * 适合测试和中低频探测。线上 PCDN 高频 NAT 探测应改为按 hub assignment 维护
     * 到 primary/backup node 的长连接或连接池，并复用已认证会话发送 SERVER_INFO_REQ、
     * SERVER_SEND_PROBE_REQ、SERVER_PROBE_DELEGATE_REQ，避免握手、认证、TIME_WAIT 和
     * 跨地域 RTT 成为探测性能瓶颈。
     */
    while (request != NULL && request->control_index < request->job.controls.size()) {
        if (request->resolved_addrs == NULL) {
            std::string            host;
            uint16_t               port = 0;
            char                   port_text[16];
            struct evutil_addrinfo hints;

            if (!ParseEndpoint(request->job.controls[request->control_index], &host, &port)) {
                request->control_index++;
                continue;
            }

            memset(&hints, 0, sizeof(hints));
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_family = AF_UNSPEC;
            snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
            NodeVerboseLog(request->job.verbose,
                             "federation resolve start req=%u type=%d control=%s\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str());
            request->state = FederationState::RESOLVING;
            request->deadline = time(NULL) + 3;
            request->dns_request =
                evdns_getaddrinfo(dns_base, host.c_str(), port_text, &hints, FederationDnsCb, request);
            if (request->state == FederationState::RESOLVING && request->dns_request == NULL &&
                request->resolved_addrs == NULL) {
                NodeVerboseLog(request->job.verbose,
                                 "federation resolve submit failed req=%u type=%d control=%s\n",
                                 request->job.request_id,
                                 (int)request->job.type,
                                 request->job.controls[request->control_index].c_str());
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
            NodeVerboseLog(request->job.verbose,
                             "federation socket failed req=%u type=%d control=%s errno=%d\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str(),
                             errno);
            continue;
        }
        SetNonblocking(request->fd);

        int rc = connect(request->fd, addr->ai_addr, addr->ai_addrlen);
        if (rc == 0) {
            NodeVerboseLog(request->job.verbose,
                             "federation connect immediate req=%u type=%d control=%s\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str());
            request->state = FederationState::SENDING_AUTH;
        } else if (errno == EINPROGRESS) {
            NodeVerboseLog(request->job.verbose,
                             "federation connect pending req=%u type=%d control=%s\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str());
            request->state = FederationState::CONNECTING;
        } else {
            NodeVerboseLog(request->job.verbose,
                             "federation connect failed req=%u type=%d control=%s errno=%d\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str(),
                             errno);
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

static bool PrepareFederationAuth(FederationRequest* request)
{
    eular::ntrs::Message req;
    eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::AUTH_REQ, 1);
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID, request->job.federation_peer_id.c_str());
    eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, request->job.auth_secret.c_str());
    NodeVerboseLog(request->job.verbose,
                     "federation auth send req=%u type=%d control=%s peer=%s\n",
                     request->job.request_id,
                     (int)request->job.type,
                     request->job.controls[request->control_index].c_str(),
                     request->job.federation_peer_id.c_str());
    request->tx_offset = 0;
    return EncodeTxMessage(req, &request->tx_buffer);
}

static bool PrepareFederationRequest(FederationRequest* request)
{
    eular::ntrs::Message req;
    if (request->job.type == AsyncFederationJobType::FETCH_PROBE_ENDPOINT) {
        eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::SERVER_INFO_REQ, 2);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::QUERY, "probe_endpoint");
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, request->session_token.c_str());
        NodeVerboseLog(request->job.verbose,
                         "federation request send req=%u type=%d control=%s message=SERVER_INFO_REQ\n",
                         request->job.request_id,
                         (int)request->job.type,
                         request->job.controls[request->control_index].c_str());
    } else if (request->job.type == AsyncFederationJobType::SEND_PROBE) {
        eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::SERVER_SEND_PROBE_REQ, 2);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TARGET_IP, request->job.target_ip.c_str());
        eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::TARGET_PORT, request->job.target_port);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TOKEN, request->job.probe_token.c_str());
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::PEER_ID,
                                           request->job.federation_peer_id.c_str());
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SRC_PEER_ID,
                                           request->job.owner_peer_id.c_str());
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::QUERY, request->job.probe_auth.c_str());
        eular::ntrs::MessageAddU32ByTag(&req, eular::ntrs::FieldTag::EXPIRE_AT,
                                        (uint32_t)request->job.probe_expire_at);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SESSION_ID, request->session_token.c_str());
        NodeVerboseLog(request->job.verbose,
                         "federation request send req=%u type=%d control=%s message=SERVER_SEND_PROBE_REQ\n",
                         request->job.request_id,
                         (int)request->job.type,
                         request->job.controls[request->control_index].c_str());
    } else {
        eular::ntrs::MessageInit(&req, eular::ntrs::MessageType::SERVER_PROBE_DELEGATE_REQ, 2);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::TARGET_IP, request->job.target_ip.c_str());
        eular::ntrs::MessageAddU16ByTag(&req, eular::ntrs::FieldTag::TARGET_PORT, request->job.target_port);
        eular::ntrs::MessageAddBytesByTag(&req, eular::ntrs::FieldTag::PROBE_TOKEN,
                                          request->job.probe_token_bytes.data(),
                                          (uint16_t)request->job.probe_token_bytes.size());
        eular::ntrs::MessageAddBoolByTag(&req, eular::ntrs::FieldTag::USE_ALT_PORT, request->job.use_alt_port);
        eular::ntrs::MessageAddStringByTag(&req, eular::ntrs::FieldTag::SESSION_ID, request->session_token.c_str());
        NodeVerboseLog(request->job.verbose,
                         "federation request send req=%u type=%d control=%s message=SERVER_PROBE_DELEGATE_REQ\n",
                         request->job.request_id,
                         (int)request->job.type,
                         request->job.controls[request->control_index].c_str());
    }
    request->tx_offset = 0;
    return EncodeTxMessage(req, &request->tx_buffer);
}

static bool FlushFederationTx(FederationRequest* request)
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

static void CloseFederationSocket(FederationRequest* request)
{
    if (request->fd >= 0) {
        close(request->fd);
        request->fd = -1;
    }
}

static void CloseFederationAttempt(FederationRequest* request)
{
    CloseFederationSocket(request);
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

static bool FederationResultFromResponse(FederationRequest* request, const eular::ntrs::Message& rsp)
{
    if (request->job.type == AsyncFederationJobType::FETCH_PROBE_ENDPOINT) {
        if (rsp.type != eular::ntrs::MessageType::SERVER_INFO_RSP) {
            NodeVerboseLog(request->job.verbose,
                             "federation response mismatch req=%u type=%d control=%s expected=SERVER_INFO_RSP got=%u\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str(),
                             (unsigned)rsp.type);
            return false;
        }
        std::string endpoint = MsgStrTag(rsp, eular::ntrs::FieldTag::PROBE_ENDPOINT);
        if (endpoint.empty()) {
            NodeVerboseLog(request->job.verbose,
                             "federation response empty probe endpoint req=%u type=%d control=%s\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str());
            return false;
        }
        NodeVerboseLog(request->job.verbose,
                         "federation response ok req=%u type=%d control=%s probe=%s\n",
                         request->job.request_id,
                         (int)request->job.type,
                         request->job.controls[request->control_index].c_str(),
                         endpoint.c_str());
        request->result.ok = true;
        request->result.selected_control = request->job.controls[request->control_index];
        request->result.peer_probe_endpoint = endpoint;
        return true;
    }

    if (request->job.type == AsyncFederationJobType::SEND_PROBE_DELEGATE) {
        uint8_t result_code = (uint8_t)eular::ntrs::ResultCode::UNKNOWN;
        eular::ntrs::MessageGetU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT, &result_code);
        if (rsp.type != eular::ntrs::MessageType::SERVER_PROBE_DELEGATE_RSP ||
            result_code != (uint8_t)eular::ntrs::ResultCode::OK) {
            NodeVerboseLog(request->job.verbose,
                             "federation response mismatch req=%u type=%d control=%s expected=SERVER_PROBE_DELEGATE_RSP got=%u result=%u\n",
                             request->job.request_id,
                             (int)request->job.type,
                             request->job.controls[request->control_index].c_str(),
                             (unsigned)rsp.type,
                             (unsigned)result_code);
            return false;
        }
        request->result.ok = true;
        request->result.selected_control = request->job.controls[request->control_index];
        return true;
    }

    uint8_t result_code = (uint8_t)eular::ntrs::ResultCode::UNKNOWN;
    eular::ntrs::MessageGetU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT, &result_code);
    if (rsp.type != eular::ntrs::MessageType::SERVER_SEND_PROBE_RSP ||
        result_code != (uint8_t)eular::ntrs::ResultCode::OK) {
        NodeVerboseLog(request->job.verbose,
                         "federation response mismatch req=%u type=%d control=%s expected=SERVER_SEND_PROBE_RSP got=%u result=%u\n",
                         request->job.request_id,
                         (int)request->job.type,
                         request->job.controls[request->control_index].c_str(),
                         (unsigned)rsp.type,
                         (unsigned)result_code);
        return false;
    }
    request->result.ok = true;
    request->result.diff_ip = true;
    request->result.selected_control = request->job.controls[request->control_index];
    return true;
}

static void FailFederationAttempt(FederationRequest* request, struct evdns_base* dns_base)
{
    NodeVerboseLog(request->job.verbose,
                     "federation attempt failed req=%u type=%d control=%s state=%d next_addr=%s errno=%d\n",
                     request->job.request_id,
                     (int)request->job.type,
                     request->control_index < request->job.controls.size()
                         ? request->job.controls[request->control_index].c_str()
                         : "-",
                     (int)request->state,
                     request->next_addr == NULL ? "null" : "set",
                     errno);
    CloseFederationSocket(request);
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
    StartFederationAttempt(request, dns_base);
}

static bool AdvanceFederationRequest(FederationRequest* request, bool readable, bool writable,
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
        FailFederationAttempt(request, dns_base);
    }
    if (request->state == FederationState::CONNECTING && request->fd < 0) {
        StartFederationAttempt(request, dns_base);
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
                    NodeVerboseLog(request->job.verbose,
                                     "federation connect completion failed req=%u type=%d control=%s so_error=%d errno=%d\n",
                                     request->job.request_id,
                                     (int)request->job.type,
                                     request->job.controls[request->control_index].c_str(),
                                     err,
                                     errno);
                    FailFederationAttempt(request, dns_base);
                } else {
                    NodeVerboseLog(request->job.verbose,
                                     "federation connect ok req=%u type=%d control=%s\n",
                                     request->job.request_id,
                                     (int)request->job.type,
                                     request->job.controls[request->control_index].c_str());
                    request->state = FederationState::SENDING_AUTH;
                }
            }

    if (request->state == FederationState::SENDING_AUTH) {
        if (request->tx_buffer.empty() && !PrepareFederationAuth(request)) {
            FailFederationAttempt(request, dns_base);
        } else if (writable && !FlushFederationTx(request)) {
            FailFederationAttempt(request, dns_base);
        } else if (request->tx_offset == request->tx_buffer.size()) {
            request->state = FederationState::READING_AUTH;
            request->tx_buffer.clear();
            request->tx_offset = 0;
        }
    }

    if ((request->state == FederationState::READING_AUTH || request->state == FederationState::READING_RESPONSE) &&
        readable) {
        std::deque<eular::ntrs::Message> messages;
        bool                              peer_closed = false;
        if (!DrainControlMessages(request->fd, &request->rx_state, &messages, &peer_closed) || peer_closed) {
            FailFederationAttempt(request, dns_base);
        } else {
            for (size_t i = 0; i < messages.size(); ++i) {
                if (request->state == FederationState::READING_AUTH) {
                    if (messages[i].type != eular::ntrs::MessageType::AUTH_RSP ||
                        MsgStrTag(messages[i], eular::ntrs::FieldTag::TOKEN)[0] == '\0') {
                        NodeVerboseLog(request->job.verbose,
                                         "federation auth rsp invalid req=%u type=%d control=%s message_type=%u token_empty=%s\n",
                                         request->job.request_id,
                                         (int)request->job.type,
                                         request->job.controls[request->control_index].c_str(),
                                         (unsigned)messages[i].type,
                                         MsgStrTag(messages[i], eular::ntrs::FieldTag::TOKEN)[0] == '\0' ? "true"
                                                                                                           : "false");
                        FailFederationAttempt(request, dns_base);
                        break;
                    }
                    request->session_token = MsgStrTag(messages[i], eular::ntrs::FieldTag::TOKEN);
                    NodeVerboseLog(request->job.verbose,
                                     "federation auth rsp ok req=%u type=%d control=%s session_token_len=%zu\n",
                                     request->job.request_id,
                                     (int)request->job.type,
                                     request->job.controls[request->control_index].c_str(),
                                     request->session_token.size());
                    if (!PrepareFederationRequest(request)) {
                        FailFederationAttempt(request, dns_base);
                        break;
                    }
                    request->state = FederationState::SENDING_REQUEST;
                } else if (FederationResultFromResponse(request, messages[i])) {
                    CloseFederationAttempt(request);
                    *completed = request->result;
                    return true;
                } else {
                    FailFederationAttempt(request, dns_base);
                    break;
                }
            }
        }
    }

    if (request->state == FederationState::SENDING_REQUEST) {
        if (writable && !FlushFederationTx(request)) {
            FailFederationAttempt(request, dns_base);
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

static bool StartFederationJob(const AsyncFederationJob& job, std::list<FederationRequest>* requests,
                                 AsyncFederationResult* immediate_result, struct evdns_base* dns_base)
{
    requests->push_back(FederationRequest());
    FederationRequest* request = &requests->back();
    request->job = job;
    InitFederationResult(request);
    if (!StartFederationAttempt(request, dns_base)) {
        if (immediate_result != NULL) {
            *immediate_result = request->result;
        }
        requests->pop_back();
        return false;
    }
    return true;
}

/**
 * @brief 处理节点内建探测端口收到的数据报。
 *
 * 仅解析 NTRS 私有二进制探测帧；非法或未知数据报直接静默丢弃。
 *
 * @param probe_fd 当前收到数据的探测 socket。
 * @param is_alt_socket 当前 socket 是否为备用端口。
 * @param probe_primary_fd 主探测 socket。
 * @param probe_alt_fd 备用探测 socket。
 * @param self_probe_ip 当前节点主探测出口地址。
 * @param self_probe_port 当前节点主探测端口。
 * @param probe_alt_bind_port 当前节点备用探测端口。
 * @param probe_controls 联邦协同节点控制面地址列表。
 * @param auth_secret 联邦鉴权密钥。
 * @param federation_requests 联邦异步请求队列。
 * @param async_results 联邦异步结果队列。
 * @param federation_dns_base 联邦 DNS 解析上下文。
 * @param verbose 是否输出详细日志。
 */
static void HandleProbePacket(int probe_fd, bool is_alt_socket, int probe_primary_fd, int probe_alt_fd,
                                const std::string& self_probe_ip, uint16_t self_probe_port,
                                uint16_t probe_alt_bind_port, int address_family,
                                const std::vector<std::string>& probe_controls, const std::string& auth_secret,
                                std::list<FederationRequest>* federation_requests,
                                std::deque<AsyncFederationResult>* async_results,
                                struct evdns_base* federation_dns_base, bool verbose)
{
    uint8_t            buf[2048];
    ntrs_binary_frame_type_t binary_frame_type = NTRS_BINARY_FRAME_UNKNOWN;
    ntrs_binary_phase_t      binary_phase = NTRS_BINARY_PHASE_UNKNOWN;
    uint8_t                  binary_token[NTRS_PROBE_TOKEN_WIRE_SIZE];
    uint16_t                 binary_token_len = 0;
    struct sockaddr_storage peer_addr;
    socklen_t               peer_len = sizeof(peer_addr);

    (void)is_alt_socket;
    (void)probe_primary_fd;

    ssize_t n = recvfrom(probe_fd, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_len);
    if (n <= 0) {
        return;
    }

    if (ParseBinaryProbeRequest(buf, (size_t)n, &binary_frame_type, &binary_phase, binary_token,
                                   &binary_token_len)) {
        if (binary_phase == NTRS_BINARY_PHASE_PROBE1 || binary_phase == NTRS_BINARY_PHASE_PROBE2) {
            std::string other_ip;
            uint16_t    other_port = 0;

            if (!ResolveProbeOtherAddress(probe_controls, self_probe_port, address_family, &other_ip, &other_port)) {
                other_ip = self_probe_ip;
                other_port = probe_alt_bind_port;
            }
            NodeVerboseLog(verbose,
                             "probe rsp fd=%d phase=%u src=%s mapped=%s origin=%s other=%s\n",
                             probe_fd,
                             (unsigned)binary_phase,
                             EndpointText(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len).c_str(),
                             EndpointText(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len).c_str(),
                             FormatEndpoint(self_probe_ip, self_probe_port).c_str(),
                             FormatEndpoint(other_ip, other_port).c_str());
            if (!SendBinaryProbeResponse(probe_fd, reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len,
                                            binary_frame_type, binary_phase, binary_token, binary_token_len,
                                            reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len,
                                            self_probe_ip, self_probe_port, other_ip, other_port)) {
                printf("binary probe rsp send failed errno=%d(%s)\n", errno, strerror(errno));
            }
            return;
        }
        if (binary_phase == NTRS_BINARY_PHASE_CHANGE_PORT) {
            if (probe_alt_fd < 0) {
                return;
            }
            NodeVerboseLog(verbose,
                             "probe rsp fd=%d phase=%u src=%s mapped=%s origin=%s other=%s via_alt_fd=%d\n",
                             probe_alt_fd,
                             (unsigned)binary_phase,
                             EndpointText(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len).c_str(),
                             EndpointText(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len).c_str(),
                             FormatEndpoint(self_probe_ip, probe_alt_bind_port).c_str(),
                             FormatEndpoint(self_probe_ip, self_probe_port).c_str(),
                             probe_alt_fd);
            if (!SendBinaryProbeResponse(probe_alt_fd, reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len,
                                            binary_frame_type, binary_phase, binary_token, binary_token_len,
                                            reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len,
                                            self_probe_ip, probe_alt_bind_port, self_probe_ip, self_probe_port)) {
                printf("binary change-port rsp send failed errno=%d(%s)\n", errno, strerror(errno));
            }
            return;
        }
        if (binary_phase == NTRS_BINARY_PHASE_CHANGE_IP) {
            std::string peer_host;
            uint16_t    peer_port = 0;
            if (!SockaddrHostPort(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len, &peer_host,
                                  &peer_port)) {
                return;
            }
            AsyncFederationJob job;
            job.type = AsyncFederationJobType::SEND_PROBE_DELEGATE;
            job.fd = -1;
            job.client_generation = 0;
            job.request_id = 0;
            job.controls = probe_controls;
            job.auth_secret = auth_secret;
            job.federation_peer_id = "service_node_federation";
            job.target_ip = peer_host;
            job.target_port = peer_port;
            job.same_ip_diff_port = false;
            job.use_alt_port = false;
            job.probe_token_bytes.assign(binary_token, binary_token + binary_token_len);
            NodeVerboseLog(verbose,
                             "probe delegate fd=%d phase=%u src=%s target=%s controls=%zu\n",
                             probe_fd,
                             (unsigned)binary_phase,
                             EndpointText(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len).c_str(),
                             FormatEndpoint(job.target_ip, job.target_port).c_str(),
                             job.controls.size());

            AsyncFederationResult result;
            if (!probe_controls.empty() && federation_requests != NULL && federation_dns_base != NULL &&
                !StartFederationJob(job, federation_requests, &result, federation_dns_base)) {
                if (async_results != NULL) {
                    async_results->push_back(result);
                }
            }
            return;
        }
    }

    NodeVerboseLog(verbose, "probe packet dropped fd=%d src=%s reason=invalid_or_unsupported_binary\n", probe_fd,
                     EndpointText(reinterpret_cast<const struct sockaddr*>(&peer_addr), peer_len).c_str());
}

static void SendError(int fd, uint64_t req, const char* code, const char* message)
{
    eular::ntrs::Message rsp;
    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::ERROR_RSP, (uint32_t)req);
    eular::ntrs::MessageAddString(&rsp, "code", code);
    eular::ntrs::MessageAddString(&rsp, "message", message);
    SendMessage(fd, rsp);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    int         port = 19000;
    uint16_t    probe_bind_port = 33478;
    uint16_t    probe_alt_bind_port = 33479;
    std::string public_host;
    std::string self_probe_endpoint;
    std::string peer_node_control_endpoint;
    std::string hub_host;
    int         hub_port = 18083;
    std::string node_id;
    std::string region = "default";
    std::string auth_secret = "ntrs-dev-secret";
    std::string bind_ip;
    std::string bind_device;
    int         address_family = AF_INET;
    bool        verbose = false;

    if (argc > 1 && argv[1][0] == '-') {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            const char* next_value = (i + 1 < argc) ? argv[i + 1] : NULL;
            std::string value;
            bool        consumed_next = false;
            if (arg == "--help" || arg == "-h") {
                PrintUsage(argv[0]);
                return 0;
            } else if (MatchLongOption(arg, "--hub", next_value, &consumed_next, &value)) {
                std::string parsed_host;
                uint16_t    parsed_port = 0;
                if (value.empty()) {
                    PrintMissingOptionValue("--hub");
                    return 1;
                }
                if (!ParseEndpoint(value.c_str(), &parsed_host, &parsed_port)) {
                    printf("invalid --hub endpoint: %s\n", value.c_str());
                    return 1;
                }
                hub_host = parsed_host;
                hub_port = parsed_port;
            } else if (MatchLongOption(arg, "--node-id", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--node-id");
                    return 1;
                }
                node_id = value;
            } else if (MatchLongOption(arg, "--public-host", next_value, &consumed_next, &value) ||
                       MatchLongOption(arg, "--advertise-host", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--public-host");
                    return 1;
                }
                public_host = value;
            } else if (MatchLongOption(arg, "--control-port", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--control-port");
                    return 1;
                }
                port = atoi(value.c_str());
            } else if (MatchLongOption(arg, "--probe-port", next_value, &consumed_next, &value)) {
                int parsed = value.empty() ? 0 : atoi(value.c_str());
                if (value.empty()) {
                    PrintMissingOptionValue("--probe-port");
                    return 1;
                }
                if (parsed <= 0 || parsed > 65535) {
                    printf("invalid --probe-port: %s\n", value.c_str());
                    return 1;
                }
                probe_bind_port = (uint16_t)parsed;
            } else if (MatchLongOption(arg, "--probe-alt-port", next_value, &consumed_next, &value)) {
                int parsed = value.empty() ? 0 : atoi(value.c_str());
                if (value.empty()) {
                    PrintMissingOptionValue("--probe-alt-port");
                    return 1;
                }
                if (parsed <= 0 || parsed > 65535) {
                    printf("invalid --probe-alt-port: %s\n", value.c_str());
                    return 1;
                }
                probe_alt_bind_port = (uint16_t)parsed;
            } else if (MatchLongOption(arg, "--bind-ip", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--bind-ip");
                    return 1;
                }
                bind_ip = value;
            } else if (MatchLongOption(arg, "--bind-device", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--bind-device");
                    return 1;
                }
                bind_device = value;
            } else if (MatchLongOption(arg, "--region", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--region");
                    return 1;
                }
                region = value;
            } else if (MatchLongOption(arg, "--auth-secret", next_value, &consumed_next, &value)) {
                if (value.empty()) {
                    PrintMissingOptionValue("--auth-secret");
                    return 1;
                }
                auth_secret = value;
            } else if (arg == "--ipv4" || arg == "-4") {
                address_family = AF_INET;
            } else if (arg == "--ipv6" || arg == "-6") {
                address_family = AF_INET6;
            } else if (arg == "--verbose" || arg == "-v") {
                verbose = true;
            } else {
                printf("unknown argument: %s\n\n", arg.c_str());
                PrintUsage(argv[0]);
                return 1;
            }
            if (consumed_next) {
                ++i;
            }
        }

        if (hub_host.empty() || node_id.empty() || public_host.empty()) {
            PrintUsage(argv[0]);
            return 1;
        }
        if (port <= 0 || port > 65535) {
            printf("invalid --control-port: %d\n", port);
            return 1;
        }
        self_probe_endpoint = FormatEndpoint(public_host, probe_bind_port);
    } else {
        if (argc > 1) {
            port = atoi(argv[1]);
        }
        if (argc > 2) {
            self_probe_endpoint = argv[2];
        }
        if (argc > 3) {
            peer_node_control_endpoint = argv[3];
        }
        hub_host = (argc > 4) ? argv[4] : "";
        hub_port = (argc > 5) ? atoi(argv[5]) : 18083;
        node_id = (argc > 6) ? argv[6] : "";
        region = (argc > 7) ? argv[7] : "default";
        auth_secret = (argc > 8) ? argv[8] : "ntrs-dev-secret";
    }

    if (self_probe_endpoint.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    signal(SIGINT, OnProcessSignal);
    signal(SIGTERM, OnProcessSignal);
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif

    eular::ntrs::ControlAuthManager AuthManager(auth_secret, kLeaseSec);

    bool        use_hub_assignment = (!hub_host.empty() && !node_id.empty());
    std::string boot_id =
        std::to_string((unsigned long long)time(NULL)) + "-" + std::to_string((unsigned long long)getpid());
    std::string assignment_p1;
    std::string assignment_p2;
    std::string assignment_b1;
    uint32_t    assignment_version = 0;
    HubConnection hub_conn;

    std::string self_probe_host;
    uint16_t    self_probe_port = 0;
    std::string self_probe_ip;
    std::string self_control_endpoint;
    if (!ParseEndpoint(self_probe_endpoint, &self_probe_host, &self_probe_port)) {
        self_probe_host = "127.0.0.1";
    }
    if (IsIpv6Literal(self_probe_host)) {
        address_family = AF_INET6;
    }
    if (!ResolveProbeHostLiteral(self_probe_host, address_family, &self_probe_ip)) {
        self_probe_ip = self_probe_host;
    }
    std::string resolved_bind_ip;
    if (!ResolveBindIp(bind_ip, bind_device, address_family, &resolved_bind_ip)) {
        printf("invalid --bind-ip/--bind-device for selected address family\n");
        return 1;
    }
    self_control_endpoint = FormatEndpoint(self_probe_host, (uint16_t)port);

    if (verbose && !bind_device.empty()) {
        printf("binding probe sockets to device: %s\n", bind_device.c_str());
    }
    if (verbose && !bind_ip.empty()) {
        printf("binding probe sockets to local IP: %s\n", bind_ip.c_str());
    }

    int probe_fd = CreateProbeSocket(self_probe_endpoint, address_family, bind_ip, bind_device);
    if (probe_fd < 0) {
        printf("failed to start built-in probe service on %s\n", self_probe_endpoint.c_str());
        return 1;
    }

    uint16_t probe_port = EndpointPort(self_probe_endpoint);
    int      probe_alt_fd = -1;
    if (probe_alt_bind_port == probe_port && probe_port > 0 && probe_port < 65535) {
        probe_alt_bind_port = (uint16_t)(probe_port + 1);
    }
    if (probe_alt_bind_port > 0) {
        probe_alt_fd = CreateProbeSocketWithPort(probe_alt_bind_port, address_family, bind_ip, bind_device);
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

    printf("NTRS node listening on :%d self_probe=%s alt_probe_port=%d peer_node_control=%s\n", port,
           self_probe_endpoint.c_str(), probe_alt_fd >= 0 ? (int)probe_alt_bind_port : -1,
           peer_node_control_endpoint.empty() ? "-" : peer_node_control_endpoint.c_str());

    std::set<int>                       clients;
    std::map<int, uint64_t>             client_generations;
    std::map<int, ControlClientRxState> client_rx_states;
    std::map<std::string, PeerSession>  peers;
    std::deque<AsyncFederationResult>   async_results;
    std::list<FederationRequest>        federation_requests;
    uint64_t                            next_client_generation = 1;
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
        time_t loop_now = time(NULL);
        if (use_hub_assignment && hub_conn.fd < 0 && loop_now >= hub_conn.next_reconnect_ts) {
            ConnectHub(&hub_conn, hub_host, (uint16_t)hub_port, verbose);
        }

        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(listen_fd, &rfds);
        FD_SET(probe_fd, &rfds);
        int maxfd = listen_fd;
        if (probe_fd > maxfd) {
            maxfd = probe_fd;
        }
        if (probe_alt_fd >= 0) {
            FD_SET(probe_alt_fd, &rfds);
            if (probe_alt_fd > maxfd) {
                maxfd = probe_alt_fd;
            }
        }
        if (use_hub_assignment && hub_conn.fd >= 0) {
            FD_SET(hub_conn.fd, &rfds);
            if (hub_conn.connecting) {
                FD_SET(hub_conn.fd, &wfds);
            }
            if (hub_conn.fd > maxfd) {
                maxfd = hub_conn.fd;
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

        SweepExpiredPeers(&peers);
        AuthManager.sweepExpired((uint64_t)time(NULL));
        event_base_loop(federation_event_base, EVLOOP_NONBLOCK | EVLOOP_NO_EXIT_ON_EMPTY);

        if (use_hub_assignment && hub_conn.fd >= 0) {
            bool hub_closed = false;

            if (hub_conn.connecting && FD_ISSET(hub_conn.fd, &wfds)) {
                int       err = 0;
                socklen_t err_len = sizeof(err);
                if (getsockopt(hub_conn.fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
                    printf("hub connect failed endpoint=%s errno=%d\n",
                           FormatEndpoint(hub_host, (uint16_t)hub_port).c_str(), err);
                    CloseHubConnection(&hub_conn);
                    hub_closed = true;
                } else {
                    hub_conn.connecting = false;
                    if (!SendHubAuth(&hub_conn, node_id, auth_secret)) {
                        CloseHubConnection(&hub_conn);
                        hub_closed = true;
                    } else {
                        hub_conn.auth_sent = true;
                        NodeVerboseLog(verbose, "hub auth sent node=%s\n", node_id.c_str());
                    }
                }
            }
            if (!hub_closed && !hub_conn.connecting && !hub_conn.authed && !hub_conn.auth_sent) {
                if (!SendHubAuth(&hub_conn, node_id, auth_secret)) {
                    CloseHubConnection(&hub_conn);
                    hub_closed = true;
                } else {
                    hub_conn.auth_sent = true;
                    NodeVerboseLog(verbose, "hub auth sent node=%s\n", node_id.c_str());
                }
            }

            if (!hub_closed && FD_ISSET(hub_conn.fd, &rfds)) {
                std::deque<eular::ntrs::Message> hub_messages;
                bool                              peer_closed = false;
                if (!DrainControlMessages(hub_conn.fd, &hub_conn.rx_state, &hub_messages, &peer_closed)) {
                    peer_closed = true;
                }
                if (peer_closed) {
                    printf("hub disconnected endpoint=%s\n", FormatEndpoint(hub_host, (uint16_t)hub_port).c_str());
                    CloseHubConnection(&hub_conn);
                    hub_closed = true;
                }

                for (size_t i = 0; !hub_closed && i < hub_messages.size(); ++i) {
                    eular::ntrs::Message& msg = hub_messages[i];
                    if (msg.type == eular::ntrs::MessageType::AUTH_RSP) {
                        uint8_t result_code = (uint8_t)eular::ntrs::ResultCode::UNKNOWN;
                        eular::ntrs::MessageGetU8ByTag(&msg, eular::ntrs::FieldTag::RESULT, &result_code);
                        if (result_code != (uint8_t)eular::ntrs::ResultCode::OK ||
                            MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN)[0] == '\0') {
                            printf("hub auth rejected node=%s\n", node_id.c_str());
                            CloseHubConnection(&hub_conn);
                            hub_closed = true;
                            break;
                        }
                        hub_conn.authed = true;
                        hub_conn.session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                        eular::ntrs::Message reg = BuildNodeRegisterMessage(
                            node_id, boot_id, region, self_probe_endpoint, self_control_endpoint,
                            hub_conn.next_request_id++);
                        eular::ntrs::Message online = BuildNodePresenceMessage(
                            node_id, boot_id, eular::ntrs::NodeStatusCode::ONLINE,
                            eular::ntrs::ReasonCode::STARTUP, hub_conn.next_request_id++);
                        if (!SendMessage(hub_conn.fd, reg) || !SendMessage(hub_conn.fd, online)) {
                            CloseHubConnection(&hub_conn);
                            hub_closed = true;
                            break;
                        }
                        hub_conn.registered = true;
                        hub_conn.last_heartbeat_ts = 0;
                        NodeVerboseLog(verbose, "hub register sent node=%s control=%s probe=%s\n",
                                       node_id.c_str(), self_control_endpoint.c_str(), self_probe_endpoint.c_str());
                    } else if (msg.type == eular::ntrs::MessageType::HUB_CLUSTER_EVENT) {
                        uint8_t event_code = (uint8_t)eular::ntrs::EventCode::UNKNOWN;
                        eular::ntrs::MessageGetU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, &event_code);
                        if (event_code == (uint8_t)eular::ntrs::EventCode::ASSIGNMENT) {
                            std::string event_node = MsgStrTag(msg, eular::ntrs::FieldTag::NODE_ID);
                            if (event_node.empty() || event_node == node_id) {
                                assignment_p1 = MsgStrTag(msg, eular::ntrs::FieldTag::PRIMARY1_CONTROL);
                                assignment_p2 = MsgStrTag(msg, eular::ntrs::FieldTag::PRIMARY2_CONTROL);
                                assignment_b1 = MsgStrTag(msg, eular::ntrs::FieldTag::BACKUP1_CONTROL);
                                assignment_version = MsgU32Tag(msg, eular::ntrs::FieldTag::ASSIGNMENT_VERSION);
                                if (assignment_version == 0) {
                                    assignment_version = msg.request_id;
                                }
                                NodeVerboseLog(verbose, "assignment updated version=%u p1=%s p2=%s b1=%s\n",
                                               assignment_version,
                                               assignment_p1.empty() ? "-" : assignment_p1.c_str(),
                                               assignment_p2.empty() ? "-" : assignment_p2.c_str(),
                                               assignment_b1.empty() ? "-" : assignment_b1.c_str());
                            }
                        }
                    } else if (msg.type == eular::ntrs::MessageType::ERROR_RSP) {
                        printf("hub error code=%s message=%s\n",
                               MsgStrTag(msg, eular::ntrs::FieldTag::CODE),
                               MsgStrTag(msg, eular::ntrs::FieldTag::MESSAGE));
                    }
                }
            }

            if (!hub_closed && hub_conn.authed && hub_conn.registered &&
                time(NULL) - hub_conn.last_heartbeat_ts >= 5) {
                eular::ntrs::Message hb = BuildNodeHeartbeatMessage(node_id, boot_id, assignment_version,
                                                                    (uint32_t)clients.size(),
                                                                    hub_conn.next_request_id++);
                if (!SendMessage(hub_conn.fd, hb)) {
                    CloseHubConnection(&hub_conn);
                } else {
                    hub_conn.last_heartbeat_ts = time(NULL);
                }
            }
        }

        if (FD_ISSET(probe_fd, &rfds)) {
            HandleProbePacket(
                probe_fd, false, probe_fd, probe_alt_fd, self_probe_ip, self_probe_port, probe_alt_bind_port,
                address_family, BuildProbeControls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint),
                auth_secret, &federation_requests, &async_results, federation_dns_base, verbose);
        }
        if (probe_alt_fd >= 0 && FD_ISSET(probe_alt_fd, &rfds)) {
            HandleProbePacket(
                probe_alt_fd, true, probe_fd, probe_alt_fd, self_probe_ip, self_probe_port, probe_alt_bind_port,
                address_family, BuildProbeControls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint),
                auth_secret, &federation_requests, &async_results, federation_dns_base, verbose);
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                SetNonblocking(fd);
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

            std::deque<eular::ntrs::Message> pending_messages;
            bool                              peer_closed = false;
            if (!DrainControlMessages(fd, &client_rx_states[fd], &pending_messages, &peer_closed)) {
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
                    std::string                 peer_id = MsgStrTag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string                 token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    eular::ntrs::ControlSession session;
                    std::string                 reason;
                    if (!AuthManager.issueSession(peer_id, token, fd, (uint64_t)time(NULL), &session, &reason)) {
                        SendError(fd, msg.request_id, "AUTH_FAILED", reason.c_str());
                        break;
                    }

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::AUTH_RSP, msg.request_id);
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                                   (uint8_t)eular::ntrs::ResultCode::OK);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::TOKEN, session.token.c_str());
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_DEFAULT_SEC,
                                                    AuthManager.sessionTtlSec());
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)session.expire_at_sec);
                    SendMessage(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::REGISTER_REQ: {
                    std::string peer_id = MsgStrTag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string device_id = MsgStrTag(msg, eular::ntrs::FieldTag::DEVICE_ID);
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string peer_key;
                    std::string reason;
                    if (peer_id.empty()) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }
                    if (device_id.empty()) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "device_id required");
                        break;
                    }
                    if (!AuthManager.validateSession(fd, peer_id, session_token, (uint64_t)time(NULL), &reason)) {
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    PeerSession s;
                    s.peer_id = peer_id;
                    s.device_id = device_id;
                    s.local_ip = MsgStrTag(msg, eular::ntrs::FieldTag::LOCAL_IP);
                    s.local_port = MsgU16Tag(msg, eular::ntrs::FieldTag::LOCAL_PORT);
                    s.srflx_ip = MsgStrTag(msg, eular::ntrs::FieldTag::SRFLX_IP);
                    s.srflx_port = MsgU16Tag(msg, eular::ntrs::FieldTag::SRFLX_PORT);
                    s.srflx_ip_2 = MsgStrTag(msg, eular::ntrs::FieldTag::SRFLX_IP_2);
                    s.srflx_port_2 = MsgU16Tag(msg, eular::ntrs::FieldTag::SRFLX_PORT_2);
                    s.nat_class = MsgU16Tag(msg, eular::ntrs::FieldTag::NAT_CLASS, NTRS_NAT_CLASS_UNKNOWN);
                    s.nat_flags = MsgU16Tag(msg, eular::ntrs::FieldTag::NAT_FLAGS, NTRS_NAT_FLAG_NONE);
                    s.mapping_behavior =
                        MsgU16Tag(msg, eular::ntrs::FieldTag::MAPPING_BEHAVIOR, NTRS_MAPPING_UNKNOWN);
                    s.filtering_behavior =
                        MsgU16Tag(msg, eular::ntrs::FieldTag::FILTERING_BEHAVIOR, NTRS_FILTERING_UNKNOWN);
                    s.nat_type = MsgStrTag(msg, eular::ntrs::FieldTag::NAT_TYPE);
                    s.fd = fd;
                    s.expire_at = time(NULL) + kLeaseSec;
                    peer_key = MakePeerKey(peer_id, device_id);
                    peers[peer_key] = s;

                    printf(
                        "REGISTER peer=%s device=%s local=%s srflx=%s srflx2=%s stable=%s risk=%s class=%u "
                        "flags=0x%04x mapping=%u filtering=%u type=%s "
                        "probe1_ok=%s probe2_ok=%s probe1_rtt_ms=%d probe2_rtt_ms=%d rounds=%u p1succ=%u p2succ=%u "
                        "p1distinct=%u p2distinct=%u f_same_ip_port=%s f_diff_ip=%s\n",
                        peer_id.c_str(), device_id.c_str(),
                        FormatEndpoint(MsgStrTag(msg, eular::ntrs::FieldTag::LOCAL_IP),
                                       MsgU16Tag(msg, eular::ntrs::FieldTag::LOCAL_PORT)).c_str(),
                        FormatEndpoint(MsgStrTag(msg, eular::ntrs::FieldTag::SRFLX_IP),
                                       MsgU16Tag(msg, eular::ntrs::FieldTag::SRFLX_PORT)).c_str(),
                        FormatEndpoint(MsgStrTag(msg, eular::ntrs::FieldTag::SRFLX_IP_2),
                                       MsgU16Tag(msg, eular::ntrs::FieldTag::SRFLX_PORT_2)).c_str(),
                        MsgBoolTag(msg, eular::ntrs::FieldTag::MAPPING_STABLE) ? "true" : "false",
                        MsgStrTag(msg, eular::ntrs::FieldTag::NAT_RISK),
                        MsgU16Tag(msg, eular::ntrs::FieldTag::NAT_CLASS, NTRS_NAT_CLASS_UNKNOWN),
                        MsgU16Tag(msg, eular::ntrs::FieldTag::NAT_FLAGS, NTRS_NAT_FLAG_NONE),
                        MsgU16Tag(msg, eular::ntrs::FieldTag::MAPPING_BEHAVIOR, NTRS_MAPPING_UNKNOWN),
                        MsgU16Tag(msg, eular::ntrs::FieldTag::FILTERING_BEHAVIOR, NTRS_FILTERING_UNKNOWN),
                        MsgStrTag(msg, eular::ntrs::FieldTag::NAT_TYPE),
                        MsgBoolTag(msg, eular::ntrs::FieldTag::PROBE1_OK) ? "true" : "false",
                        MsgBoolTag(msg, eular::ntrs::FieldTag::PROBE2_OK) ? "true" : "false",
                        MsgI32Tag(msg, eular::ntrs::FieldTag::PROBE1_RTT_MS),
                        MsgI32Tag(msg, eular::ntrs::FieldTag::PROBE2_RTT_MS),
                        MsgU32Tag(msg, eular::ntrs::FieldTag::PROBE_ROUNDS),
                        MsgU32Tag(msg, eular::ntrs::FieldTag::PROBE1_SUCCESS_COUNT),
                        MsgU32Tag(msg, eular::ntrs::FieldTag::PROBE2_SUCCESS_COUNT),
                        MsgU32Tag(msg, eular::ntrs::FieldTag::PROBE1_DISTINCT_MAPPINGS),
                        MsgU32Tag(msg, eular::ntrs::FieldTag::PROBE2_DISTINCT_MAPPINGS),
                        MsgBoolTag(msg, eular::ntrs::FieldTag::FILTER_SAME_IP_DIFF_PORT_RX) ? "true" : "false",
                        MsgBoolTag(msg, eular::ntrs::FieldTag::FILTER_DIFF_IP_RX) ? "true" : "false");

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::REGISTER_RSP, msg.request_id);
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_SEC, 30);
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, 10);
                    SendMessage(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::HEARTBEAT_REQ: {
                    std::string peer_id = MsgStrTag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (peer_id.empty()) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }
                    if (!AuthManager.validateSession(fd, peer_id, session_token, (uint64_t)time(NULL), &reason)) {
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator pit = FindPeerByFdAndPeerId(&peers, peer_id, fd);
                    if (pit == peers.end() || pit->second.fd != fd) {
                        SendError(fd, msg.request_id, "NOT_REGISTERED", "peer not registered");
                        break;
                    }
                    pit->second.expire_at = time(NULL) + kLeaseSec;

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::HEARTBEAT_RSP, msg.request_id);
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_SEC, 30);
                    SendMessage(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::UNREGISTER_REQ: {
                    std::string peer_id = MsgStrTag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (peer_id.empty()) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }
                    if (!AuthManager.validateSession(fd, peer_id, session_token, (uint64_t)time(NULL), &reason)) {
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator pit = FindPeerByFdAndPeerId(&peers, peer_id, fd);
                    if (pit != peers.end() && pit->second.fd == fd) {
                        uint8_t reason_code = (uint8_t)eular::ntrs::ReasonCode::NONE;
                        eular::ntrs::MessageGetU8ByTag(&msg, eular::ntrs::FieldTag::REASON, &reason_code);
                        printf("UNREGISTER peer=%s device=%s reason=%s\n", peer_id.c_str(), pit->second.device_id.c_str(),
                               eular::ntrs::ReasonCodeName((eular::ntrs::ReasonCode)reason_code));
                        peers.erase(pit);
                    }

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::UNREGISTER_RSP, msg.request_id);
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                                   (uint8_t)eular::ntrs::ResultCode::OK);
                    SendMessage(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::SESSION_CREATE_REQ: {
                    std::string                   src = MsgStrTag(msg, eular::ntrs::FieldTag::SRC_PEER_ID);
                    std::string                   src_device = MsgStrTag(msg, eular::ntrs::FieldTag::SRC_DEVICE_ID);
                    std::string                   dst = MsgStrTag(msg, eular::ntrs::FieldTag::DST_PEER_ID);
                    std::string                   dst_device = MsgStrTag(msg, eular::ntrs::FieldTag::DST_DEVICE_ID);
                    std::string                   session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string                   reason;
                    eular::ntrs::PeerSessionLease peer_session;
                    uint64_t                      now_sec = (uint64_t)time(NULL);
                    bool                          dst_ambiguous = false;
                    if (src.empty() || src_device.empty() || dst.empty()) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "src peer/device and dst peer required");
                        break;
                    }
                    if (!AuthManager.validateSession(fd, src, session_token, now_sec, &reason)) {
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator src_it = FindPeerExact(&peers, src, src_device);
                    std::map<std::string, PeerSession>::iterator dst_it =
                        dst_device.empty() ? FindUniquePeerByPeerId(&peers, dst, &dst_ambiguous)
                                           : FindPeerExact(&peers, dst, dst_device);
                    if (src_it == peers.end() || src_it->second.fd != fd) {
                        SendError(fd, msg.request_id, "NOT_REGISTERED", "src peer not registered on this connection");
                        break;
                    }
                    if (dst_ambiguous) {
                        SendError(fd, msg.request_id, "DST_DEVICE_REQUIRED", "dst peer has multiple devices");
                        break;
                    }
                    if (dst_it == peers.end()) {
                        SendError(fd, msg.request_id, "DST_OFFLINE", "dst peer not online");
                        break;
                    }

                    std::string         sid = eular::ntrs::MintPeerSessionId(src, dst, now_sec);
                    SessionStrategyPlan strategy;
                    if (!AuthManager.issuePeerSession(src, dst, sid, now_sec, 60, &peer_session, &reason)) {
                        SendError(fd, msg.request_id, "AUTH_FAILED", reason.c_str());
                        break;
                    }
                    strategy = BuildSessionStrategy(src_it->second, dst_it->second);

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::SESSION_CREATE_RSP, msg.request_id);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::SESSION_ID, sid.c_str());
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::ROLE,
                                                   (uint8_t)eular::ntrs::RoleCode::INITIATOR);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::TOKEN, peer_session.token.c_str());
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::PUNCH_ORDER, strategy.src_punch_order);
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::CONNECT_ROLE, strategy.src_connect_role);
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::WARMUP_ROUNDS, strategy.src_warmup_rounds);
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::WARMUP_INTERVAL_MS, strategy.src_warmup_interval_ms);
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)peer_session.expire_at_sec);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_ID,
                                                       dst_it->second.peer_id.c_str());
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_DEVICE_ID,
                                                       dst_it->second.device_id.c_str());
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_LOCAL_IP,
                                                       dst_it->second.local_ip.c_str());
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_LOCAL_PORT,
                                                    dst_it->second.local_port);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_IP,
                                                       dst_it->second.srflx_ip.c_str());
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_PORT,
                                                    dst_it->second.srflx_port);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_IP_2,
                                                       dst_it->second.srflx_ip_2.c_str());
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_SRFLX_PORT_2,
                                                    dst_it->second.srflx_port_2);
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_NAT_CLASS,
                                                    dst_it->second.nat_class);
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_NAT_FLAGS,
                                                    dst_it->second.nat_flags);
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_MAPPING_BEHAVIOR,
                                                    dst_it->second.mapping_behavior);
                    eular::ntrs::MessageAddU16ByTag(&rsp, eular::ntrs::FieldTag::PEER_FILTERING_BEHAVIOR,
                                                    dst_it->second.filtering_behavior);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PEER_NAT_TYPE,
                                                       dst_it->second.nat_type.c_str());
                    SendMessage(fd, rsp);

                    eular::ntrs::Message notify;
                    eular::ntrs::MessageInit(&notify, eular::ntrs::MessageType::SESSION_NOTIFY, 0);
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::SESSION_ID, sid.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::TOKEN, peer_session.token.c_str());
                    eular::ntrs::MessageAddU8ByTag(&notify, eular::ntrs::FieldTag::PUNCH_ORDER, strategy.dst_punch_order);
                    eular::ntrs::MessageAddU8ByTag(&notify, eular::ntrs::FieldTag::CONNECT_ROLE, strategy.dst_connect_role);
                    eular::ntrs::MessageAddU32ByTag(&notify, eular::ntrs::FieldTag::WARMUP_ROUNDS, strategy.dst_warmup_rounds);
                    eular::ntrs::MessageAddU32ByTag(&notify, eular::ntrs::FieldTag::WARMUP_INTERVAL_MS, strategy.dst_warmup_interval_ms);
                    eular::ntrs::MessageAddU32ByTag(&notify, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)peer_session.expire_at_sec);
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::SRC_PEER_ID, src.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::SRC_DEVICE_ID, src_device.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::DST_PEER_ID, dst.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::DST_DEVICE_ID,
                                                       dst_it->second.device_id.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_ID,
                                                       src_it->second.peer_id.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_DEVICE_ID,
                                                       src_it->second.device_id.c_str());
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_LOCAL_IP,
                                                       src_it->second.local_ip.c_str());
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_LOCAL_PORT,
                                                    src_it->second.local_port);
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_IP,
                                                       src_it->second.srflx_ip.c_str());
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_PORT,
                                                    src_it->second.srflx_port);
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_IP_2,
                                                       src_it->second.srflx_ip_2.c_str());
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_SRFLX_PORT_2,
                                                    src_it->second.srflx_port_2);
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_NAT_CLASS,
                                                    src_it->second.nat_class);
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_NAT_FLAGS,
                                                    src_it->second.nat_flags);
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_MAPPING_BEHAVIOR,
                                                    src_it->second.mapping_behavior);
                    eular::ntrs::MessageAddU16ByTag(&notify, eular::ntrs::FieldTag::PEER_FILTERING_BEHAVIOR,
                                                    src_it->second.filtering_behavior);
                    eular::ntrs::MessageAddStringByTag(&notify, eular::ntrs::FieldTag::PEER_NAT_TYPE,
                                                       src_it->second.nat_type.c_str());
                    SendMessage(dst_it->second.fd, notify);
                    break;
                }
                case eular::ntrs::MessageType::NAT_PROBE_REQ: {
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (!AuthManager.validateSession(fd, "", session_token, (uint64_t)time(NULL), &reason)) {
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    AsyncFederationJob job;
                    std::vector<std::string> probe_controls =
                        BuildProbeControls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint);
                    job.type = AsyncFederationJobType::FETCH_PROBE_ENDPOINT;
                    job.fd = fd;
                    job.client_generation = client_generations[fd];
                    job.request_id = msg.request_id;
                    job.controls = probe_controls;
                    job.auth_secret = auth_secret;
                    job.federation_peer_id = "service_node_federation";
                    NodeVerboseLog(verbose,
                                     "NAT_PROBE_REQ fd=%d req=%u assignment_version=%u controls=%s\n",
                                     fd,
                                     msg.request_id,
                                     assignment_version,
                                     JoinControlEndpoints(probe_controls).c_str());
                    {
                        AsyncFederationResult result;
                        if (!StartFederationJob(job, &federation_requests, &result, federation_dns_base)) {
                            NodeVerboseLog(verbose,
                                             "NAT_PROBE_REQ immediate federation fallback fd=%d req=%u selected=%s ok=%s\n",
                                             fd,
                                             msg.request_id,
                                             result.selected_control.empty() ? "-" : result.selected_control.c_str(),
                                             result.ok ? "true" : "false");
                            async_results.push_back(result);
                        }
                    }
                    break;
                }
                case eular::ntrs::MessageType::FILTER_PROBE_REQ: {
                    std::string target_ip = MsgStrTag(msg, eular::ntrs::FieldTag::TARGET_IP);
                    uint16_t    target_port = MsgU16Tag(msg, eular::ntrs::FieldTag::TARGET_PORT);
                    std::string token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::SESSION_ID);
                    std::string peer_ip;
                    std::string owner_peer_id;
                    std::string reason;
                    if (target_ip.empty() || target_port == 0 || token.empty()) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "target_ip/target_port/token required");
                        break;
                    }
                    NodeVerboseLog(verbose, "FILTER_PROBE_REQ fd=%d req=%u target=%s token=%s\n", fd,
                                     msg.request_id, FormatEndpoint(target_ip, target_port).c_str(), token.c_str());
                    if (!AuthManager.validateSession(fd, "", session_token, (uint64_t)time(NULL), &reason)) {
                        printf("FILTER_PROBE_REQ auth failed fd=%d req=%u reason=%s\n", fd, msg.request_id,
                               reason.c_str());
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    if (!PeerIpString(fd, &peer_ip) || target_ip != peer_ip) {
                        NodeVerboseLog(verbose,
                                         "FILTER_PROBE_REQ scope mismatch fd=%d req=%u peer_ip=%s target_ip=%s\n", fd,
                                         msg.request_id, peer_ip.c_str(), target_ip.c_str());
                        SendError(fd, msg.request_id, "TARGET_SCOPE_MISMATCH", "target_ip must match control peer IP");
                        break;
                    }
                    if (!AuthManager.sessionPeerId(fd, &owner_peer_id)) {
                        NodeVerboseLog(verbose, "FILTER_PROBE_REQ missing session peer fd=%d req=%u\n", fd,
                                         msg.request_id);
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", "session peer missing");
                        break;
                    }

                    bool same_ip_diff_port = false;
                    if (probe_alt_fd >= 0) {
                        same_ip_diff_port =
                            SendUdpFilterProbe(probe_alt_fd, target_ip, target_port, token, "same_ip_diff_port");
                    }
                    NodeVerboseLog(verbose,
                                     "FILTER_PROBE_REQ local send fd=%d req=%u same_ip_diff_port_sent=%s alt_fd=%d\n",
                                     fd, msg.request_id, same_ip_diff_port ? "true" : "false", probe_alt_fd);

                    uint64_t    probe_expire_at = (uint64_t)time(NULL) + 5;
                    std::string probe_auth = eular::ntrs::MintProbeAuthorization(auth_secret, owner_peer_id, target_ip,
                                                                                 target_port, token, probe_expire_at);
                    AsyncFederationJob job;
                    job.type = AsyncFederationJobType::SEND_PROBE;
                    job.fd = fd;
                    job.client_generation = client_generations[fd];
                    job.request_id = msg.request_id;
                    job.controls =
                        BuildProbeControls(assignment_p1, assignment_p2, assignment_b1, peer_node_control_endpoint);
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
                        if (!StartFederationJob(job, &federation_requests, &result, federation_dns_base)) {
                            NodeVerboseLog(verbose,
                                             "FILTER_PROBE_REQ federation start failed fd=%d req=%u selected=%s\n",
                                             fd, msg.request_id, result.selected_control.c_str());
                            async_results.push_back(result);
                        } else {
                            NodeVerboseLog(verbose, "FILTER_PROBE_REQ federation started fd=%d req=%u controls=%zu\n",
                                             fd, msg.request_id, job.controls.size());
                        }
                    }
                    break;
                }
                case eular::ntrs::MessageType::SERVER_INFO_REQ: {
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string reason;
                    if (!AuthManager.validateSession(fd, "service_node_federation", session_token,
                                                      (uint64_t)time(NULL), &reason)) {
                        NodeVerboseLog(verbose,
                                         "SERVER_INFO_REQ auth failed fd=%d req=%u reason=%s\n",
                                         fd,
                                         msg.request_id,
                                         reason.c_str());
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    NodeVerboseLog(verbose,
                                     "SERVER_INFO_REQ fd=%d req=%u self_probe_endpoint=%s\n",
                                     fd,
                                     msg.request_id,
                                     self_probe_endpoint.empty() ? "-" : self_probe_endpoint.c_str());
                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::SERVER_INFO_RSP, msg.request_id);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PROBE_ENDPOINT,
                                                       self_probe_endpoint.c_str());
                    NodeVerboseLog(verbose,
                                     "SERVER_INFO_RSP fd=%d req=%u probe_endpoint=%s\n",
                                     fd,
                                     msg.request_id,
                                     self_probe_endpoint.empty() ? "-" : self_probe_endpoint.c_str());
                    SendMessage(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::SERVER_SEND_PROBE_REQ: {
                    std::string target_ip = MsgStrTag(msg, eular::ntrs::FieldTag::TARGET_IP);
                    uint16_t    target_port = MsgU16Tag(msg, eular::ntrs::FieldTag::TARGET_PORT);
                    std::string token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    std::string owner_peer_id = MsgStrTag(msg, eular::ntrs::FieldTag::SRC_PEER_ID);
                    std::string probe_auth = MsgStrTag(msg, eular::ntrs::FieldTag::QUERY);
                    uint32_t    expire_at = MsgU32Tag(msg, eular::ntrs::FieldTag::EXPIRE_AT);
                    std::string session_token = MsgStrTag(msg, eular::ntrs::FieldTag::SESSION_ID);
                    std::string reason;
                    if (!AuthManager.validateSession(fd, "service_node_federation", session_token,
                                                      (uint64_t)time(NULL), &reason)) {
                        NodeVerboseLog(verbose, "SERVER_SEND_PROBE_REQ auth failed fd=%d req=%u reason=%s\n", fd,
                                         msg.request_id, reason.c_str());
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    if ((uint64_t)expire_at <= (uint64_t)time(NULL) ||
                        !eular::ntrs::ValidateProbeAuthorization(auth_secret, owner_peer_id, target_ip, target_port,
                                                                 token, expire_at, probe_auth)) {
                        NodeVerboseLog(
                            verbose,
                            "SERVER_SEND_PROBE_REQ authorization invalid fd=%d req=%u target=%s token=%s\n", fd,
                            msg.request_id, FormatEndpoint(target_ip, target_port).c_str(), token.c_str());
                        SendError(fd, msg.request_id, "PROBE_AUTH_INVALID", "probe authorization invalid");
                        break;
                    }
                    bool ok = SendUdpFilterProbe(probe_fd, target_ip, target_port, token, "diff_ip");
                    NodeVerboseLog(verbose,
                                     "SERVER_SEND_PROBE_REQ fd=%d req=%u target=%s token=%s diff_ip_sent=%s\n",
                                     fd, msg.request_id, FormatEndpoint(target_ip, target_port).c_str(), token.c_str(),
                                     ok ? "true" : "false");

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::SERVER_SEND_PROBE_RSP, msg.request_id);
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                              (uint8_t)(ok ? eular::ntrs::ResultCode::OK
                                                           : eular::ntrs::ResultCode::FAILED));
                    SendMessage(fd, rsp);
                    break;
                }
                case eular::ntrs::MessageType::SERVER_PROBE_DELEGATE_REQ: {
                    std::string            target_ip = MsgStrTag(msg, eular::ntrs::FieldTag::TARGET_IP);
                    uint16_t               target_port = MsgU16Tag(msg, eular::ntrs::FieldTag::TARGET_PORT);
                    std::string            session_token = MsgStrTag(msg, eular::ntrs::FieldTag::SESSION_ID);
                    const uint8_t*         txid_data = NULL;
                    uint16_t               txid_len = 0;
                    bool                   use_alt_port = MsgBoolTag(msg, eular::ntrs::FieldTag::USE_ALT_PORT);
                    std::string            reason;
                    struct sockaddr_storage target_addr;
                    socklen_t               target_addr_len = 0;
                    int                     send_fd = use_alt_port ? probe_alt_fd : probe_fd;
                    uint16_t                response_port = use_alt_port ? probe_alt_bind_port : self_probe_port;

                    if (!AuthManager.validateSession(fd, "service_node_federation", session_token,
                                                      (uint64_t)time(NULL), &reason)) {
                        SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                        break;
                    }
                    if (target_ip.empty() || target_port == 0 ||
                        !eular::ntrs::MessageGetBytesByTag(&msg, eular::ntrs::FieldTag::PROBE_TOKEN, &txid_data,
                                                           &txid_len) ||
                        txid_len != NTRS_PROBE_TOKEN_WIRE_SIZE || send_fd < 0) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "target/txid required");
                        break;
                    }

                    if (!MakeSockaddrStorage(target_ip, target_port, &target_addr, &target_addr_len)) {
                        SendError(fd, msg.request_id, "INVALID_PARAM", "invalid target_ip");
                        break;
                    }

                    bool ok = SendBinaryProbeResponse(
                        send_fd, reinterpret_cast<const struct sockaddr*>(&target_addr), target_addr_len,
                        NTRS_BINARY_FRAME_FILTER_REQ, NTRS_BINARY_PHASE_CHANGE_IP, txid_data, txid_len,
                        reinterpret_cast<const struct sockaddr*>(&target_addr), target_addr_len, self_probe_ip,
                        response_port, self_probe_ip, use_alt_port ? self_probe_port : probe_alt_bind_port);
                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::SERVER_PROBE_DELEGATE_RSP,
                                             msg.request_id);
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                              (uint8_t)(ok ? eular::ntrs::ResultCode::OK
                                                           : eular::ntrs::ResultCode::FAILED));
                    SendMessage(fd, rsp);
                    break;
                }
                default:
                    SendError(fd, msg.request_id, "UNSUPPORTED", "message unsupported in M1");
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
            ErasePeerForFd(&peers, fd);
            AuthManager.revokeFd(fd);
            for (std::list<FederationRequest>::iterator fit = federation_requests.begin();
                 fit != federation_requests.end();) {
                if (fit->job.fd == fd) {
                    CloseFederationAttempt(&*fit);
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
            if (AdvanceFederationRequest(&*it, readable, writable, &result, federation_dns_base)) {
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
            if (result.type == AsyncFederationJobType::FETCH_PROBE_ENDPOINT) {
                NodeVerboseLog(verbose,
                                 "NAT_PROBE_RSP federation result fd=%d req=%u ok=%s selected=%s peer_probe=%s\n",
                                 result.fd,
                                 result.request_id,
                                 result.ok ? "true" : "false",
                                 result.selected_control.empty() ? "-" : result.selected_control.c_str(),
                                 result.peer_probe_endpoint.empty() ? "-" : result.peer_probe_endpoint.c_str());
                eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::NAT_PROBE_RSP, result.request_id);
                eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PROBE1, self_probe_endpoint.c_str());
                if (result.ok) {
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::SELECTED_CONTROL,
                                                       result.selected_control.c_str());
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PROBE2,
                                                       result.peer_probe_endpoint.c_str());
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::FEDERATION, "ok");
                } else {
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::PROBE2, "");
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::FEDERATION, "degraded");
                }
                SendMessage(result.fd, rsp);
                continue;
            }

            if (result.type == AsyncFederationJobType::SEND_PROBE) {
                printf(
                    "FILTER_PROBE_RSP fd=%d req=%u result=%s same_ip_diff_port_sent=%s diff_ip_sent=%s selected=%s "
                    "error=%s\n",
                    result.fd, result.request_id, (result.same_ip_diff_port || result.diff_ip) ? "ok" : "degraded",
                    result.same_ip_diff_port ? "true" : "false", result.diff_ip ? "true" : "false",
                    result.selected_control.c_str(), result.ok ? "none" : "federation_failed");
                eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::FILTER_PROBE_RSP, result.request_id);
                eular::ntrs::MessageAddBoolByTag(&rsp, eular::ntrs::FieldTag::SAME_IP_DIFF_PORT_SENT,
                                                 result.same_ip_diff_port);
                eular::ntrs::MessageAddBoolByTag(&rsp, eular::ntrs::FieldTag::DIFF_IP_SENT, result.diff_ip);
                eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                          (uint8_t)((result.same_ip_diff_port || result.diff_ip)
                                                        ? eular::ntrs::ResultCode::OK
                                                        : eular::ntrs::ResultCode::DEGRADED));
                SendMessage(result.fd, rsp);
            }
        }
    }

    if (use_hub_assignment && hub_conn.fd >= 0) {
        eular::ntrs::Message offline = BuildNodePresenceMessage(
            node_id, boot_id, eular::ntrs::NodeStatusCode::OFFLINE,
            eular::ntrs::ReasonCode::CLIENT_EXIT, hub_conn.next_request_id++);
        SendMessage(hub_conn.fd, offline);
    }

    for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
        close(*it);
    }
    for (std::list<FederationRequest>::iterator it = federation_requests.begin(); it != federation_requests.end();
         ++it) {
        CloseFederationAttempt(&*it);
    }
    evdns_base_free(federation_dns_base, 1);
    event_base_free(federation_event_base);
    close(listen_fd);
    close(probe_fd);
    if (probe_alt_fd >= 0) {
        close(probe_alt_fd);
    }

    if (use_hub_assignment && hub_conn.fd >= 0) {
        CloseHubConnection(&hub_conn);
    }

    return 0;
}
