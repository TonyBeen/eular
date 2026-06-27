#include <errno.h>
#include <fcntl.h>
#include <ntrs/auth.h>
#include <ntrs/codec.h>
#include <ntrs/hub_state.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>

static const int      kDefaultHubPort = 18083;
static const uint32_t kHubAuthTtlSec = 60;
static const uint8_t  kHubStateMagic[8] = {'N', 'T', 'R', 'S', 'H', 'U', 'B', 'S'};
static const uint32_t kHubStateVersion = 1;
static const uint32_t kMaxHubStatePayloadSize = 16u * 1024u * 1024u;
static const uint32_t kAssignmentMinStableSec = 15;
static const uint32_t kAssignmentMinHeartbeatCount = 2;

static volatile sig_atomic_t g_stop = 0;

struct Assignment {
    std::string primary1;
    std::string primary2;
    std::string backup1;
};

static bool operator==(const Assignment& lhs, const Assignment& rhs)
{
    return lhs.primary1 == rhs.primary1 && lhs.primary2 == rhs.primary2 && lhs.backup1 == rhs.backup1;
}

struct HubClientRxState {
    std::vector<uint8_t> buffer;
};

struct HubClientSession {
    HubClientRxState rx;
    bool             authed;
    std::string      node_id;
    std::string      session_token;
    bool             has_assignment;
    Assignment       last_assignment;
    uint32_t         last_assignment_version;
    uint64_t         connected_at_sec;
    uint64_t         connected_order;
    uint32_t         heartbeat_count;

    HubClientSession()
        : authed(false)
        , has_assignment(false)
        , last_assignment_version(0)
        , connected_at_sec(0)
        , connected_order(0)
        , heartbeat_count(0)
    {
    }
};

struct AssignmentCandidate {
    std::string node_id;
    std::string control_endpoint;
    uint64_t    connected_order;
    uint64_t    hrw_score;
};

static bool AssignmentCandidateScoreGreater(const AssignmentCandidate& lhs, const AssignmentCandidate& rhs)
{
    return lhs.hrw_score > rhs.hrw_score;
}

static uint64_t Fnv1a64Update(uint64_t hash, const std::string& value)
{
    for (size_t i = 0; i < value.size(); ++i) {
        hash ^= (uint8_t)value[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t AssignmentHrwScore(const std::string& self_node, const std::string& candidate_node)
{
    uint64_t hash = 1469598103934665603ULL;

    hash = Fnv1a64Update(hash, self_node);
    hash ^= 0xFFu;
    hash *= 1099511628211ULL;
    hash = Fnv1a64Update(hash, candidate_node);
    return hash;
}

static bool IsAssignmentCandidateStable(const HubClientSession& session, uint64_t now_sec)
{
    if (session.heartbeat_count >= kAssignmentMinHeartbeatCount) {
        return true;
    }
    return session.connected_at_sec > 0 &&
           now_sec >= session.connected_at_sec &&
           now_sec - session.connected_at_sec >= kAssignmentMinStableSec;
}

static void OnSignal(int)
{
    g_stop = 1;
}

static void PrintUsage(const char* program)
{
    printf(
        "Usage:\n"
        "  %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --host TEXT             Hub listen host/IP [default: ::]\n"
        "  --port UINT             Hub listen port [default: 18083]\n"
        "  --state-file TEXT       Snapshot state file [default: ./ntrs_hub_state.bin]\n"
        "  --auth-secret TEXT      Node-hub auth secret [default: ntrs-dev-secret]\n"
        "  --help, -h              Show this help message\n"
        "\n"
        "Long option forms:\n"
        "  --name value            Space separated form\n"
        "  --name=value            Equals form\n"
        "\n"
        "Example:\n"
        "  %s --host :: --port 18083 --state-file ./ntrs_hub_state.bin --auth-secret '<secret>'\n",
        program,
        program);
}

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

static void PrintMissingOptionValue(const char* option)
{
    printf("missing value for %s\n", option);
}

static const char* MsgStrTag(const eular::ntrs::Message& msg, eular::ntrs::FieldTag tag)
{
    const char* value = eular::ntrs::MessageGetStringByTag(&msg, tag);
    return value == NULL ? "" : value;
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

static bool SendAll(int fd, const void* buf, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t         left = len;

    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static void AppendUniqueFd(std::vector<int>* fds, int fd)
{
    if (fds == NULL || fd < 0) {
        return;
    }
    if (std::find(fds->begin(), fds->end(), fd) == fds->end()) {
        fds->push_back(fd);
    }
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

static void AppendU32(std::vector<uint8_t>* out, uint32_t value)
{
    out->push_back((uint8_t)((value >> 24) & 0xFFu));
    out->push_back((uint8_t)((value >> 16) & 0xFFu));
    out->push_back((uint8_t)((value >> 8) & 0xFFu));
    out->push_back((uint8_t)(value & 0xFFu));
}

static void AppendU64(std::vector<uint8_t>* out, uint64_t value)
{
    out->push_back((uint8_t)((value >> 56) & 0xFFu));
    out->push_back((uint8_t)((value >> 48) & 0xFFu));
    out->push_back((uint8_t)((value >> 40) & 0xFFu));
    out->push_back((uint8_t)((value >> 32) & 0xFFu));
    out->push_back((uint8_t)((value >> 24) & 0xFFu));
    out->push_back((uint8_t)((value >> 16) & 0xFFu));
    out->push_back((uint8_t)((value >> 8) & 0xFFu));
    out->push_back((uint8_t)(value & 0xFFu));
}

static uint32_t ReadU32(const uint8_t* data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static uint64_t ReadU64(const uint8_t* data)
{
    return ((uint64_t)data[0] << 56) |
           ((uint64_t)data[1] << 48) |
           ((uint64_t)data[2] << 40) |
           ((uint64_t)data[3] << 32) |
           ((uint64_t)data[4] << 24) |
           ((uint64_t)data[5] << 16) |
           ((uint64_t)data[6] << 8) |
           (uint64_t)data[7];
}

static bool DrainMessages(int fd, HubClientRxState* state, std::deque<eular::ntrs::Message>* messages,
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

static bool SetNonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool IsIpv6Literal(const std::string& host)
{
    struct in6_addr addr6;
    return inet_pton(AF_INET6, host.c_str(), &addr6) == 1;
}

static bool CreateListenSocket(const std::string& host, uint16_t port, int* listen_fd)
{
    int fd = -1;

    if (listen_fd == NULL || port == 0) {
        return false;
    }
    *listen_fd = -1;

    if (host.empty() || IsIpv6Literal(host) || host == "::") {
        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        if (host.empty() || host == "::") {
            addr6.sin6_addr = in6addr_any;
        } else if (inet_pton(AF_INET6, host.c_str(), &addr6.sin6_addr) != 1) {
            return false;
        }

        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        int on = 1;
        int off = 0;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr6), sizeof(addr6)) != 0 ||
            listen(fd, 128) != 0 ||
            !SetNonblocking(fd)) {
            close(fd);
            return false;
        }
        *listen_fd = fd;
        return true;
    }

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    if (host == "0.0.0.0") {
        addr4.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host.c_str(), &addr4.sin_addr) != 1) {
        return false;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr4), sizeof(addr4)) != 0 ||
        listen(fd, 128) != 0 ||
        !SetNonblocking(fd)) {
        close(fd);
        return false;
    }
    *listen_fd = fd;
    return true;
}

static eular::ntrs::EventCode EventCodeForName(const std::string& event)
{
    if (event == "node_registered") {
        return eular::ntrs::EventCode::NODE_REGISTERED;
    }
    if (event == "node_generation_replaced") {
        return eular::ntrs::EventCode::NODE_GENERATION_REPLACED;
    }
    if (event == "node_online") {
        return eular::ntrs::EventCode::NODE_ONLINE;
    }
    if (event == "node_abnormal_offline") {
        return eular::ntrs::EventCode::NODE_ABNORMAL_OFFLINE;
    }
    if (event == "node_offline") {
        return eular::ntrs::EventCode::NODE_OFFLINE;
    }
    if (event == "node_status_changed") {
        return eular::ntrs::EventCode::NODE_STATUS_CHANGED;
    }
    if (event == "node_heartbeat") {
        return eular::ntrs::EventCode::NODE_HEARTBEAT;
    }
    if (event == "node_evicted") {
        return eular::ntrs::EventCode::NODE_EVICTED;
    }
    return eular::ntrs::EventCode::UNKNOWN;
}

static eular::ntrs::NodeStatusCode StatusCodeForName(const std::string& status)
{
    if (status == "registered") {
        return eular::ntrs::NodeStatusCode::REGISTERED;
    }
    if (status == "online") {
        return eular::ntrs::NodeStatusCode::ONLINE;
    }
    if (status == "offline") {
        return eular::ntrs::NodeStatusCode::OFFLINE;
    }
    return eular::ntrs::NodeStatusCode::UNKNOWN;
}

static eular::ntrs::Message BuildClusterEventMessage(const std::string& event,
                                                     const eular::ntrs::ClusterNodeState& node,
                                                     uint64_t cluster_version)
{
    eular::ntrs::Message msg;

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::HUB_CLUSTER_EVENT, (uint32_t)cluster_version);
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, (uint8_t)EventCodeForName(event));
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node.node_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, node.boot_id.c_str());
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::STATUS, (uint8_t)StatusCodeForName(node.status));
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::REGION, node.region.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::PROBE_ENDPOINT, node.probe_endpoint.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::CONTROL_ENDPOINT, node.control_endpoint.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NAT_TYPE, node.nat_type.c_str());
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::LOAD, (uint32_t)node.load);
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC,
                                    node.heartbeat_interval_sec);
    const std::string ts = NowIso8601();
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    return msg;
}

static Assignment BuildAssignmentFor(const std::string& self_node,
                                     const std::map<std::string, eular::ntrs::ClusterNodeState>& nodes,
                                     const std::map<int, HubClientSession>& sessions,
                                     uint64_t now_sec)
{
    Assignment                       a;
    std::vector<AssignmentCandidate> all_candidates;
    std::vector<AssignmentCandidate> stable_candidates;

    for (std::map<int, HubClientSession>::const_iterator it = sessions.begin(); it != sessions.end(); ++it) {
        if (!it->second.authed || it->second.node_id.empty() || it->second.node_id == self_node) {
            continue;
        }
        std::map<std::string, eular::ntrs::ClusterNodeState>::const_iterator node_it =
            nodes.find(it->second.node_id);
        if (node_it == nodes.end() ||
            node_it->second.status != "online" ||
            node_it->second.control_endpoint.empty()) {
            continue;
        }
        AssignmentCandidate c;
        c.node_id = it->second.node_id;
        c.control_endpoint = node_it->second.control_endpoint;
        c.connected_order = it->second.connected_order;
        c.hrw_score = AssignmentHrwScore(self_node, c.node_id);
        all_candidates.push_back(c);
        if (IsAssignmentCandidateStable(it->second, now_sec)) {
            stable_candidates.push_back(c);
        }
    }

    std::vector<AssignmentCandidate>& candidates =
        stable_candidates.size() >= 3u ? stable_candidates : all_candidates;

    std::stable_sort(candidates.begin(), candidates.end(), AssignmentCandidateScoreGreater);
    if (candidates.size() > 0) {
        a.primary1 = candidates[0].control_endpoint;
    }
    if (candidates.size() > 1) {
        a.primary2 = candidates[1].control_endpoint;
    }
    if (candidates.size() > 2) {
        a.backup1 = candidates[2].control_endpoint;
    }
    return a;
}

static eular::ntrs::Message BuildAssignmentMessage(const std::string& node_id, const Assignment& a,
                                                   uint32_t assignment_version)
{
    eular::ntrs::Message msg;

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::HUB_CLUSTER_EVENT, assignment_version);
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, (uint8_t)eular::ntrs::EventCode::ASSIGNMENT);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::MessageAddU32ByTag(&msg, eular::ntrs::FieldTag::ASSIGNMENT_VERSION, assignment_version);
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::PRIMARY1_CONTROL, a.primary1.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::PRIMARY2_CONTROL, a.primary2.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BACKUP1_CONTROL, a.backup1.c_str());
    const std::string ts = NowIso8601();
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    return msg;
}

static void SendAssignments(const eular::ntrs::HubClusterState& cluster_state,
                            std::map<int, HubClientSession>* sessions,
                            uint32_t* assignment_version,
                            std::vector<int>* failed_fds)
{
    if (sessions == NULL || assignment_version == NULL) {
        return;
    }

    uint64_t now_sec = (uint64_t)time(NULL);
    for (std::map<int, HubClientSession>::iterator it = sessions->begin(); it != sessions->end(); ++it) {
        if (!it->second.authed || it->second.node_id.empty()) {
            continue;
        }
        Assignment a = BuildAssignmentFor(it->second.node_id, cluster_state.nodes(), *sessions, now_sec);
        if (it->second.has_assignment && it->second.last_assignment == a) {
            continue;
        }

        ++(*assignment_version);
        if (*assignment_version == 0) {
            *assignment_version = 1;
        }
        eular::ntrs::Message msg = BuildAssignmentMessage(it->second.node_id, a, *assignment_version);
        printf("hub send assignment node=%s assignment_version=%u cluster_version=%llu p1=%s p2=%s b1=%s\n",
               it->second.node_id.c_str(),
               *assignment_version,
               (unsigned long long)cluster_state.clusterVersion(),
               a.primary1.empty() ? "-" : a.primary1.c_str(),
               a.primary2.empty() ? "-" : a.primary2.c_str(),
               a.backup1.empty() ? "-" : a.backup1.c_str());
        if (!SendMessage(it->first, msg)) {
            AppendUniqueFd(failed_fds, it->first);
            continue;
        }
        it->second.has_assignment = true;
        it->second.last_assignment = a;
        it->second.last_assignment_version = *assignment_version;
    }
}

static bool SaveSnapshot(const std::string& state_file, const eular::ntrs::HubClusterState& cluster_state)
{
    std::vector<uint8_t> payload;
    std::vector<uint8_t> buf;
    std::string          tmp_file = state_file + ".tmp";

    if (state_file.empty()) {
        return false;
    }
    if (!eular::ntrs::EncodeClusterSnapshotNodes(cluster_state.nodes(), &payload) ||
        payload.size() > kMaxHubStatePayloadSize ||
        cluster_state.nodes().size() > 0xFFFFFFFFu) {
        return false;
    }

    buf.reserve(sizeof(kHubStateMagic) + 20u + payload.size());
    buf.insert(buf.end(), kHubStateMagic, kHubStateMagic + sizeof(kHubStateMagic));
    AppendU32(&buf, kHubStateVersion);
    AppendU64(&buf, cluster_state.clusterVersion());
    AppendU32(&buf, (uint32_t)cluster_state.nodes().size());
    AppendU32(&buf, (uint32_t)payload.size());
    buf.insert(buf.end(), payload.begin(), payload.end());

    if (buf.empty()) {
        return false;
    }

    FILE* fp = fopen(tmp_file.c_str(), "wb");
    if (fp == NULL) {
        return false;
    }
    bool ok = fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp_file.c_str());
        return false;
    }
    if (rename(tmp_file.c_str(), state_file.c_str()) != 0) {
        unlink(tmp_file.c_str());
        return false;
    }
    return true;
}

static bool LoadSnapshot(const std::string& state_file, eular::ntrs::HubClusterState* cluster_state)
{
    static const size_t kHeaderSize = 8u + 4u + 8u + 4u + 4u;

    FILE* fp = NULL;
    long  size = 0;

    if (state_file.empty() || cluster_state == NULL) {
        return false;
    }
    fp = fopen(state_file.c_str(), "rb");
    if (fp == NULL) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    size = ftell(fp);
    if (size < (long)kHeaderSize || size > (long)(kHeaderSize + kMaxHubStatePayloadSize)) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    std::vector<uint8_t> data((size_t)size);
    bool                ok = fread(data.data(), 1, data.size(), fp) == data.size();
    fclose(fp);
    if (!ok) {
        return false;
    }

    size_t offset = 0;
    if (memcmp(data.data(), kHubStateMagic, sizeof(kHubStateMagic)) != 0) {
        eular::ntrs::Message msg;
        return eular::ntrs::DecodeMessage(data.data(), data.size(), &msg) && cluster_state->restoreFromSnapshot(msg);
    }
    offset += sizeof(kHubStateMagic);

    uint32_t state_version = ReadU32(data.data() + offset);
    offset += 4u;
    if (state_version != kHubStateVersion) {
        return false;
    }

    uint64_t cluster_version = ReadU64(data.data() + offset);
    offset += 8u;
    uint32_t node_count = ReadU32(data.data() + offset);
    offset += 4u;
    uint32_t payload_len = ReadU32(data.data() + offset);
    offset += 4u;
    if (payload_len > kMaxHubStatePayloadSize || offset + payload_len != data.size()) {
        return false;
    }

    std::vector<eular::ntrs::ClusterNodeState> nodes;
    if (!eular::ntrs::DecodeClusterSnapshotNodes(data.data() + offset, payload_len, &nodes) ||
        nodes.size() != node_count) {
        return false;
    }
    return cluster_state->restoreFromNodes(cluster_version, nodes);
}

static std::string MessageNodeId(const eular::ntrs::Message& msg)
{
    return MsgStrTag(msg, eular::ntrs::FieldTag::NODE_ID);
}

static bool ApplyNodeMessage(const std::string& node_id, const eular::ntrs::Message& msg,
                             eular::ntrs::HubClusterState* cluster_state,
                             std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> >* events)
{
    std::string                   event_name;
    eular::ntrs::ClusterNodeState node;

    if (cluster_state == NULL || events == NULL || node_id.empty()) {
        return false;
    }
    if (!cluster_state->applyMessage(node_id, msg, (uint64_t)time(NULL), NowIso8601(), &event_name, &node)) {
        return false;
    }
    events->push_back(std::make_pair(event_name, node));
    return true;
}

static eular::ntrs::Message BuildPresenceMessage(const std::string& node_id, const std::string& boot_id,
                                                 eular::ntrs::NodeStatusCode status,
                                                 eular::ntrs::ReasonCode reason_code)
{
    eular::ntrs::Message msg;

    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::NODE_PRESENCE, (uint32_t)time(NULL));
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, boot_id.c_str());
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::STATUS, (uint8_t)status);
    eular::ntrs::MessageAddU8ByTag(&msg, eular::ntrs::FieldTag::REASON, (uint8_t)reason_code);
    const std::string ts = NowIso8601();
    eular::ntrs::MessageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());
    return msg;
}

static std::string CurrentBootIdForNode(const eular::ntrs::HubClusterState& cluster_state,
                                        const std::string& node_id)
{
    std::map<std::string, eular::ntrs::ClusterNodeState>::const_iterator it = cluster_state.nodes().find(node_id);
    if (it == cluster_state.nodes().end()) {
        return "";
    }
    return it->second.boot_id;
}

static bool IsNodeOffline(const eular::ntrs::HubClusterState& cluster_state, const std::string& node_id)
{
    std::map<std::string, eular::ntrs::ClusterNodeState>::const_iterator it = cluster_state.nodes().find(node_id);
    return it != cluster_state.nodes().end() && it->second.status == "offline";
}

static void PublishUpdates(const std::string& state_file,
                           eular::ntrs::HubClusterState* cluster_state,
                           std::map<int, HubClientSession>* sessions,
                           const std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> >& events,
                           uint32_t* assignment_version,
                           std::vector<int>* failed_fds)
{
    if (cluster_state == NULL || events.empty()) {
        return;
    }
    if (!SaveSnapshot(state_file, *cluster_state)) {
        printf("hub save snapshot failed state_file=%s nodes=%zu version=%llu\n",
               state_file.c_str(),
               cluster_state->nodes().size(),
               (unsigned long long)cluster_state->clusterVersion());
    }
    for (size_t i = 0; i < events.size(); ++i) {
        eular::ntrs::Message event_msg =
            BuildClusterEventMessage(events[i].first, events[i].second, cluster_state->clusterVersion());
        for (std::map<int, HubClientSession>::const_iterator it = sessions->begin(); it != sessions->end(); ++it) {
            if (it->second.authed) {
                if (!SendMessage(it->first, event_msg)) {
                    AppendUniqueFd(failed_fds, it->first);
                }
            }
        }
    }
    SendAssignments(*cluster_state, sessions, assignment_version, failed_fds);
}

static void CloseClient(int fd,
                        std::set<int>* clients,
                        std::map<int, HubClientSession>* sessions,
                        eular::ntrs::ControlAuthManager* auth_manager,
                        eular::ntrs::HubClusterState* cluster_state,
                        const std::string& state_file,
                        uint32_t* assignment_version,
                        bool mark_offline,
                        eular::ntrs::ReasonCode reason_code)
{
    std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> > events;
    std::string                                                        node_id;

    if (sessions != NULL) {
        std::map<int, HubClientSession>::iterator it = sessions->find(fd);
        if (it != sessions->end()) {
            node_id = it->second.node_id;
            sessions->erase(it);
        }
    }
    if (auth_manager != NULL) {
        auth_manager->revokeFd(fd);
    }
    if (clients != NULL) {
        clients->erase(fd);
    }
    close(fd);

    if (mark_offline && !node_id.empty() && cluster_state != NULL && !IsNodeOffline(*cluster_state, node_id)) {
        std::string boot_id = CurrentBootIdForNode(*cluster_state, node_id);
        if (!boot_id.empty()) {
            std::vector<int> failed_fds;
            eular::ntrs::Message offline =
                BuildPresenceMessage(node_id, boot_id, eular::ntrs::NodeStatusCode::OFFLINE, reason_code);
            ApplyNodeMessage(node_id, offline, cluster_state, &events);
            PublishUpdates(state_file, cluster_state, sessions, events, assignment_version, &failed_fds);
            for (size_t i = 0; i < failed_fds.size(); ++i) {
                if (clients != NULL && clients->find(failed_fds[i]) != clients->end()) {
                    CloseClient(failed_fds[i], clients, sessions, auth_manager, cluster_state, state_file,
                                assignment_version, false, eular::ntrs::ReasonCode::NONE);
                }
            }
        }
    }
}

static void CloseDuplicateNode(const std::string& node_id,
                               int keep_fd,
                               std::set<int>* clients,
                               std::map<int, HubClientSession>* sessions,
                               eular::ntrs::ControlAuthManager* auth_manager)
{
    std::vector<int> duplicates;

    if (sessions == NULL) {
        return;
    }
    for (std::map<int, HubClientSession>::const_iterator it = sessions->begin(); it != sessions->end(); ++it) {
        if (it->first != keep_fd && it->second.node_id == node_id) {
            duplicates.push_back(it->first);
        }
    }
    for (size_t i = 0; i < duplicates.size(); ++i) {
        int fd = duplicates[i];
        if (auth_manager != NULL) {
            auth_manager->revokeFd(fd);
        }
        if (clients != NULL) {
            clients->erase(fd);
        }
        sessions->erase(fd);
        close(fd);
        printf("hub closed duplicate node session node=%s fd=%d\n", node_id.c_str(), fd);
    }
}

static void SendError(int fd, uint32_t request_id, const char* code, const char* message)
{
    eular::ntrs::Message rsp;

    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::ERROR_RSP, request_id);
    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::CODE, code == NULL ? "ERROR" : code);
    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::MESSAGE, message == NULL ? "" : message);
    SendMessage(fd, rsp);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    std::string host = "::";
    int         port = kDefaultHubPort;
    std::string state_file = "./ntrs_hub_state.bin";
    std::string auth_secret = "ntrs-dev-secret";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const char* next_value = (i + 1 < argc) ? argv[i + 1] : NULL;
        std::string value;
        bool        consumed_next = false;

        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else if (MatchLongOption(arg, "--host", next_value, &consumed_next, &value)) {
            if (value.empty()) {
                PrintMissingOptionValue("--host");
                return 1;
            }
            host = value;
        } else if (MatchLongOption(arg, "--port", next_value, &consumed_next, &value)) {
            int parsed = value.empty() ? 0 : atoi(value.c_str());
            if (value.empty()) {
                PrintMissingOptionValue("--port");
                return 1;
            }
            if (parsed <= 0 || parsed > 65535) {
                printf("invalid --port: %s\n", value.c_str());
                return 1;
            }
            port = parsed;
        } else if (MatchLongOption(arg, "--state-file", next_value, &consumed_next, &value)) {
            if (value.empty()) {
                PrintMissingOptionValue("--state-file");
                return 1;
            }
            state_file = value;
        } else if (MatchLongOption(arg, "--auth-secret", next_value, &consumed_next, &value)) {
            if (value.empty()) {
                PrintMissingOptionValue("--auth-secret");
                return 1;
            }
            auth_secret = value;
        } else {
            printf("unknown argument: %s\n\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
        if (consumed_next) {
            ++i;
        }
    }

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif

    int listen_fd = -1;
    if (!CreateListenSocket(host, (uint16_t)port, &listen_fd)) {
        printf("hub listen failed host=%s port=%d errno=%d\n", host.c_str(), port, errno);
        return 1;
    }

    eular::ntrs::HubClusterState      cluster_state;
    eular::ntrs::ControlAuthManager   auth_manager(auth_secret, kHubAuthTtlSec);
    std::set<int>                     clients;
    std::map<int, HubClientSession>   sessions;
    uint32_t                          assignment_version = 0;
    uint64_t                          next_connected_order = 1;

    if (LoadSnapshot(state_file, &cluster_state)) {
        printf("hub restored snapshot version=%llu nodes=%zu state_file=%s\n",
               (unsigned long long)cluster_state.clusterVersion(),
               cluster_state.nodes().size(),
               state_file.c_str());
    }

    printf("ntrs_hub listening on %s:%d state_file=%s\n", host.c_str(), port, state_file.c_str());

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
            FD_SET(*it, &rfds);
            if (*it > maxfd) {
                maxfd = *it;
            }
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            continue;
        }

        auth_manager.sweepExpired((uint64_t)time(NULL));

        if (FD_ISSET(listen_fd, &rfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                SetNonblocking(fd);
                clients.insert(fd);
                sessions[fd] = HubClientSession();
                printf("hub client connected fd=%d\n", fd);
            }
        }

        std::vector<int> closed;
        for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
            int fd = *it;
            if (!FD_ISSET(fd, &rfds)) {
                continue;
            }

            bool                              peer_closed = false;
            std::deque<eular::ntrs::Message> messages;
            if (!DrainMessages(fd, &sessions[fd].rx, &messages, &peer_closed)) {
                peer_closed = true;
            }
            if (peer_closed) {
                closed.push_back(fd);
                continue;
            }

            for (size_t i = 0; i < messages.size(); ++i) {
                eular::ntrs::Message& msg = messages[i];
                if (msg.type == eular::ntrs::MessageType::AUTH_REQ) {
                    std::string                 node_id = MsgStrTag(msg, eular::ntrs::FieldTag::PEER_ID);
                    std::string                 token = MsgStrTag(msg, eular::ntrs::FieldTag::TOKEN);
                    eular::ntrs::ControlSession session;
                    std::string                 reason;

                    if (sessions[fd].authed) {
                        SendError(fd, msg.request_id, "AUTH_ALREADY_DONE", "hub auth already completed");
                        closed.push_back(fd);
                        break;
                    }
                    if (!auth_manager.issueSession(node_id, token, fd, (uint64_t)time(NULL), &session, &reason)) {
                        SendError(fd, msg.request_id, "AUTH_FAILED", reason.c_str());
                        closed.push_back(fd);
                        break;
                    }

                    sessions[fd].authed = true;
                    sessions[fd].node_id = node_id;
                    sessions[fd].session_token = session.token;
                    sessions[fd].connected_at_sec = (uint64_t)time(NULL);
                    sessions[fd].connected_order = next_connected_order++;

                    eular::ntrs::Message rsp;
                    eular::ntrs::MessageInit(&rsp, eular::ntrs::MessageType::AUTH_RSP, msg.request_id);
                    eular::ntrs::MessageAddU8ByTag(&rsp, eular::ntrs::FieldTag::RESULT,
                                                   (uint8_t)eular::ntrs::ResultCode::OK);
                    eular::ntrs::MessageAddStringByTag(&rsp, eular::ntrs::FieldTag::TOKEN, session.token.c_str());
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::LEASE_DEFAULT_SEC,
                                                    auth_manager.sessionTtlSec());
                    eular::ntrs::MessageAddU32ByTag(&rsp, eular::ntrs::FieldTag::EXPIRE_AT,
                                                    (uint32_t)session.expire_at_sec);
                    SendMessage(fd, rsp);
                    printf("hub auth ok node=%s fd=%d\n", node_id.c_str(), fd);
                    continue;
                }

                if (!sessions[fd].authed) {
                    SendError(fd, msg.request_id, "AUTH_REQUIRED", "hub auth required");
                    closed.push_back(fd);
                    break;
                }

                std::string message_node_id = MessageNodeId(msg);
                if (message_node_id.empty() || message_node_id != sessions[fd].node_id) {
                    SendError(fd, msg.request_id, "NODE_ID_MISMATCH", "node_id mismatch");
                    closed.push_back(fd);
                    break;
                }

                if (msg.type != eular::ntrs::MessageType::NODE_REGISTER &&
                    msg.type != eular::ntrs::MessageType::NODE_PRESENCE &&
                    msg.type != eular::ntrs::MessageType::NODE_HEARTBEAT) {
                    SendError(fd, msg.request_id, "UNSUPPORTED", "unsupported hub message");
                    continue;
                }

                std::string reason;
                if (!auth_manager.validateSession(fd, sessions[fd].node_id, sessions[fd].session_token,
                                                  (uint64_t)time(NULL), &reason)) {
                    SendError(fd, msg.request_id, "AUTH_REQUIRED", reason.c_str());
                    closed.push_back(fd);
                    break;
                }

                std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> > events;
                if (!ApplyNodeMessage(message_node_id, msg, &cluster_state, &events)) {
                    continue;
                }
                if (msg.type == eular::ntrs::MessageType::NODE_HEARTBEAT) {
                    ++sessions[fd].heartbeat_count;
                }
                if (msg.type == eular::ntrs::MessageType::NODE_REGISTER) {
                    CloseDuplicateNode(message_node_id, fd, &clients, &sessions, &auth_manager);
                }
                PublishUpdates(state_file, &cluster_state, &sessions, events, &assignment_version, &closed);

                printf("hub updated node=%s type=%u status=%s version=%llu\n",
                       message_node_id.c_str(),
                       (unsigned)msg.type,
                       cluster_state.nodes().find(message_node_id) == cluster_state.nodes().end()
                           ? "-"
                           : cluster_state.nodes().find(message_node_id)->second.status.c_str(),
                       (unsigned long long)cluster_state.clusterVersion());
            }
        }

        std::sort(closed.begin(), closed.end());
        closed.erase(std::unique(closed.begin(), closed.end()), closed.end());
        for (size_t i = 0; i < closed.size(); ++i) {
            if (clients.find(closed[i]) != clients.end()) {
                CloseClient(closed[i], &clients, &sessions, &auth_manager, &cluster_state, state_file,
                            &assignment_version, true, eular::ntrs::ReasonCode::LWT);
            }
        }

        std::vector<eular::ntrs::ClusterNodeState> evicted_nodes;
        if (cluster_state.sweepExpired((uint64_t)time(NULL), &evicted_nodes)) {
            std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> > events;
            for (size_t i = 0; i < evicted_nodes.size(); ++i) {
                events.push_back(std::make_pair("node_evicted", evicted_nodes[i]));
            }
            PublishUpdates(state_file, &cluster_state, &sessions, events, &assignment_version, &closed);
        }
    }

    std::vector<int> all_clients(clients.begin(), clients.end());
    for (size_t i = 0; i < all_clients.size(); ++i) {
        CloseClient(all_clients[i], &clients, &sessions, &auth_manager, &cluster_state, state_file,
                    &assignment_version, false, eular::ntrs::ReasonCode::NONE);
    }
    close(listen_fd);
    return 0;
}
