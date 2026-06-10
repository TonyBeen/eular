#include <ntrs_hub_state.h>

#include <stdlib.h>
#include <string.h>

namespace eular {
namespace ntrs {

namespace {

static std::string safeMsgStringTag(const Message &msg, FieldTag tag)
{
    const char *value = messageGetStringByTag(&msg, tag);
    return value == NULL ? "" : value;
}

static uint32_t safeMsgU32Tag(const Message &msg, FieldTag tag, uint32_t default_value)
{
    uint32_t value = default_value;
    if (!messageGetU32ByTag(&msg, tag, &value)) {
        return default_value;
    }
    return value;
}

static uint64_t parseBootPrefix(const std::string &boot_id)
{
    if (boot_id.empty()) {
        return 0;
    }

    size_t pos = boot_id.find('-');
    std::string prefix = pos == std::string::npos ? boot_id : boot_id.substr(0, pos);
    char *end = NULL;
    unsigned long long parsed = strtoull(prefix.c_str(), &end, 10);
    if (end == prefix.c_str() || end == NULL || *end != '\0') {
        return 0;
    }

    return (uint64_t)parsed;
}

static void appendU16(std::vector<uint8_t> *out, uint16_t value)
{
    out->push_back((uint8_t)((value >> 8) & 0xFFu));
    out->push_back((uint8_t)(value & 0xFFu));
}

static void appendU32(std::vector<uint8_t> *out, uint32_t value)
{
    out->push_back((uint8_t)((value >> 24) & 0xFFu));
    out->push_back((uint8_t)((value >> 16) & 0xFFu));
    out->push_back((uint8_t)((value >> 8) & 0xFFu));
    out->push_back((uint8_t)(value & 0xFFu));
}

static void appendU64(std::vector<uint8_t> *out, uint64_t value)
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

static uint16_t readU16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t readU32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static uint64_t readU64(const uint8_t *data)
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

static bool appendString(std::vector<uint8_t> *out, const std::string &value)
{
    if (value.size() > 0xFFFFu) {
        return false;
    }

    appendU16(out, (uint16_t)value.size());
    out->insert(out->end(), value.begin(), value.end());
    return true;
}

static bool readString(const uint8_t *data, size_t len, size_t *offset, std::string *out)
{
    uint16_t field_len = 0;

    if (data == NULL || offset == NULL || out == NULL || *offset + 2u > len) {
        return false;
    }

    field_len = readU16(data + *offset);
    *offset += 2u;
    if (*offset + field_len > len) {
        return false;
    }

    out->assign(reinterpret_cast<const char *>(data + *offset), field_len);
    *offset += field_len;
    return true;
}

static bool appendNode(std::vector<uint8_t> *out, const ClusterNodeState &node)
{
    if (!appendString(out, node.node_id) ||
        !appendString(out, node.boot_id) ||
        !appendString(out, node.status) ||
        !appendString(out, node.region) ||
        !appendString(out, node.stun_endpoint) ||
        !appendString(out, node.control_endpoint) ||
        !appendString(out, node.nat_type) ||
        !appendString(out, node.last_heartbeat)) {
        return false;
    }

    appendU32(out, node.heartbeat_interval_sec);
    appendU64(out, node.last_seen_mono_sec);
    appendU32(out, (uint32_t)node.load);
    return true;
}

static bool readNode(const uint8_t *data, size_t len, size_t *offset, ClusterNodeState *node)
{
    if (node == NULL ||
        !readString(data, len, offset, &node->node_id) ||
        !readString(data, len, offset, &node->boot_id) ||
        !readString(data, len, offset, &node->status) ||
        !readString(data, len, offset, &node->region) ||
        !readString(data, len, offset, &node->stun_endpoint) ||
        !readString(data, len, offset, &node->control_endpoint) ||
        !readString(data, len, offset, &node->nat_type) ||
        !readString(data, len, offset, &node->last_heartbeat)) {
        return false;
    }

    if (*offset + 16u > len) {
        return false;
    }

    node->heartbeat_interval_sec = readU32(data + *offset);
    *offset += 4u;
    node->last_seen_mono_sec = readU64(data + *offset);
    *offset += 8u;
    node->load = (int32_t)readU32(data + *offset);
    *offset += 4u;
    return !node->node_id.empty();
}

}  // namespace

HubClusterState::HubClusterState()
    : cluster_version_(0)
{
}

bool HubClusterState::applyMessage(const std::string &topic_node_id,
                                   const Message &msg,
                                   uint64_t now_mono_sec,
                                   const std::string &now_iso8601,
                                   std::string *event_name,
                                   ClusterNodeState *event_node)
{
    std::string field_node_id;
    std::string incoming_boot_id;
    std::map<std::string, ClusterNodeState>::iterator it;
    bool created = false;

    if (topic_node_id.empty()) {
        return false;
    }

    field_node_id = safeMsgStringTag(msg, FieldTag::NODE_ID);
    if (!field_node_id.empty() && field_node_id != topic_node_id) {
        return false;
    }

    incoming_boot_id = safeMsgStringTag(msg, FieldTag::BOOT_ID);
    if (incoming_boot_id.empty()) {
        return false;
    }

    it = nodes_.find(topic_node_id);
    if (it == nodes_.end()) {
        uint8_t incoming_status = (uint8_t)NodeStatusCode::UNKNOWN;
        if (msg.type == MessageType::NODE_PRESENCE &&
            messageGetU8ByTag(&msg, FieldTag::STATUS, &incoming_status) &&
            incoming_status == (uint8_t)NodeStatusCode::OFFLINE) {
            return false;
        }

        ClusterNodeState node;
        node.node_id = topic_node_id;
        node.status = "unknown";
        node.heartbeat_interval_sec = 5;
        node.last_seen_mono_sec = now_mono_sec;
        node.last_heartbeat = now_iso8601;
        nodes_[topic_node_id] = node;
        it = nodes_.find(topic_node_id);
        created = true;
    }

    ClusterNodeState &node = it->second;
    if (!created && !isCurrentGeneration(node, incoming_boot_id)) {
        if (!shouldReplaceGeneration(node, incoming_boot_id)) {
            return false;
        }

        node = ClusterNodeState();
        node.node_id = topic_node_id;
        node.heartbeat_interval_sec = 5;
        node.last_seen_mono_sec = now_mono_sec;
        node.last_heartbeat = now_iso8601;
    }

    std::string next_event;
    bool changed = false;

    if (msg.type == MessageType::NODE_REGISTER) {
        node.node_id = topic_node_id;
        node.boot_id = incoming_boot_id;
        node.region = safeMsgStringTag(msg, FieldTag::REGION);
        node.stun_endpoint = safeMsgStringTag(msg, FieldTag::STUN_ENDPOINT);
        node.control_endpoint = safeMsgStringTag(msg, FieldTag::CONTROL_ENDPOINT);
        node.nat_type = safeMsgStringTag(msg, FieldTag::NAT_TYPE);
        node.heartbeat_interval_sec = safeMsgU32Tag(
            msg, FieldTag::HEARTBEAT_INTERVAL_SEC, node.heartbeat_interval_sec == 0 ? 5 : node.heartbeat_interval_sec);
        node.last_seen_mono_sec = now_mono_sec;
        node.last_heartbeat = now_iso8601;
        if (node.status.empty()) {
            node.status = node_status_code_name(NodeStatusCode::REGISTERED);
        }
        next_event = created ? "node_registered" : "node_generation_replaced";
        changed = true;
    } else if (msg.type == MessageType::NODE_PRESENCE) {
        uint8_t status_code = (uint8_t)NodeStatusCode::UNKNOWN;
        if (!messageGetU8ByTag(&msg, FieldTag::STATUS, &status_code) ||
            status_code == (uint8_t)NodeStatusCode::UNKNOWN) {
            return false;
        }
        std::string status = node_status_code_name((NodeStatusCode)status_code);

        if (status_code == (uint8_t)NodeStatusCode::OFFLINE && !isCurrentGeneration(node, incoming_boot_id)) {
            return false;
        }

        node.node_id = topic_node_id;
        node.boot_id = incoming_boot_id;
        node.status = status;
        node.last_seen_mono_sec = now_mono_sec;
        node.last_heartbeat = safeMsgStringTag(msg, FieldTag::TS);
        if (node.last_heartbeat.empty()) {
            node.last_heartbeat = now_iso8601;
        }
        if (status_code == (uint8_t)NodeStatusCode::ONLINE) {
            next_event = created ? "node_online" : "node_online";
        } else if (status_code == (uint8_t)NodeStatusCode::OFFLINE) {
            uint8_t reason_code = (uint8_t)ReasonCode::NONE;
            messageGetU8ByTag(&msg, FieldTag::REASON, &reason_code);
            next_event = reason_code == (uint8_t)ReasonCode::LWT ? "node_abnormal_offline" : "node_offline";
        } else {
            next_event = "node_status_changed";
        }
        changed = true;
    } else if (msg.type == MessageType::NODE_HEARTBEAT) {
        std::string nat_type = safeMsgStringTag(msg, FieldTag::NAT_TYPE);
        node.node_id = topic_node_id;
        node.boot_id = incoming_boot_id;
        node.status = node_status_code_name(NodeStatusCode::ONLINE);
        if (!nat_type.empty()) {
            node.nat_type = nat_type;
        }
        node.load = (int32_t)safeMsgU32Tag(msg, FieldTag::LOAD, 0);
        node.last_seen_mono_sec = now_mono_sec;
        node.last_heartbeat = safeMsgStringTag(msg, FieldTag::TS);
        if (node.last_heartbeat.empty()) {
            node.last_heartbeat = now_iso8601;
        }
        node.heartbeat_interval_sec = safeMsgU32Tag(
            msg, FieldTag::HEARTBEAT_INTERVAL_SEC, node.heartbeat_interval_sec == 0 ? 5 : node.heartbeat_interval_sec);
        next_event = "node_heartbeat";
        changed = true;
    }

    if (!changed) {
        return false;
    }

    bumpVersion();
    if (event_name != NULL) {
        *event_name = next_event;
    }
    if (event_node != NULL) {
        *event_node = node;
    }
    return true;
}

bool HubClusterState::sweepExpired(uint64_t now_mono_sec,
                                   std::vector<ClusterNodeState> *evicted_nodes)
{
    bool changed = false;
    std::map<std::string, ClusterNodeState>::iterator it = nodes_.begin();
    while (it != nodes_.end()) {
        if (it->second.last_seen_mono_sec == 0 ||
            now_mono_sec < it->second.last_seen_mono_sec ||
            now_mono_sec - it->second.last_seen_mono_sec <= heartbeatTimeoutSec(it->second)) {
            ++it;
            continue;
        }

        if (evicted_nodes != NULL) {
            evicted_nodes->push_back(it->second);
        }
        it = nodes_.erase(it);
        changed = true;
    }

    if (changed) {
        bumpVersion();
    }
    return changed;
}

bool HubClusterState::restoreFromSnapshot(const Message &msg)
{
    uint64_t cluster_version = 0;
    std::vector<ClusterNodeState> nodes;

    if (!nodes_.empty() || cluster_version_ != 0) {
        return false;
    }
    if (!parseClusterSnapshotMessage(msg, &cluster_version, &nodes)) {
        return false;
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes_[nodes[i].node_id] = nodes[i];
    }
    cluster_version_ = cluster_version;
    return true;
}

const std::map<std::string, ClusterNodeState> &HubClusterState::nodes() const
{
    return nodes_;
}

uint64_t HubClusterState::cluster_version() const
{
    return cluster_version_;
}

bool HubClusterState::shouldReplaceGeneration(const ClusterNodeState &current,
                                              const std::string &incoming_boot_id) const
{
    uint64_t current_order = bootGenerationOrder(current.boot_id);
    uint64_t incoming_order = bootGenerationOrder(incoming_boot_id);

    if (incoming_order > current_order) {
        return true;
    }
    if (incoming_order < current_order) {
        return false;
    }
    return incoming_boot_id > current.boot_id;
}

bool HubClusterState::isCurrentGeneration(const ClusterNodeState &current,
                                          const std::string &incoming_boot_id) const
{
    return current.boot_id == incoming_boot_id;
}

uint64_t HubClusterState::bootGenerationOrder(const std::string &boot_id) const
{
    return parseBootPrefix(boot_id);
}

uint32_t HubClusterState::heartbeatTimeoutSec(const ClusterNodeState &node) const
{
    uint32_t interval = node.heartbeat_interval_sec == 0 ? 5 : node.heartbeat_interval_sec;
    uint32_t timeout = interval * 3;
    return timeout < 3 ? 3 : timeout;
}

void HubClusterState::bumpVersion()
{
    ++cluster_version_;
}

bool encodeClusterSnapshotNodes(const std::map<std::string, ClusterNodeState> &nodes, std::vector<uint8_t> *out)
{
    if (out == NULL) {
        return false;
    }

    out->clear();
    if (nodes.size() > 0xFFFFu) {
        return false;
    }

    out->push_back(1u);
    appendU16(out, (uint16_t)nodes.size());
    for (std::map<std::string, ClusterNodeState>::const_iterator it = nodes.begin();
         it != nodes.end();
         ++it) {
        if (!appendNode(out, it->second)) {
            out->clear();
            return false;
        }
    }
    return true;
}

bool decodeClusterSnapshotNodes(const void *data, size_t len, std::vector<ClusterNodeState> *nodes)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    uint16_t expected_count = 0;
    size_t offset = 0;

    if (bytes == NULL || nodes == NULL || len < 3u) {
        return false;
    }
    if (bytes[0] != 1u) {
        return false;
    }

    offset = 1u;
    expected_count = readU16(bytes + offset);
    offset += 2u;
    nodes->clear();
    nodes->reserve(expected_count);

    for (uint16_t i = 0; i < expected_count; ++i) {
        ClusterNodeState node;
        if (!readNode(bytes, len, &offset, &node)) {
            nodes->clear();
            return false;
        }
        nodes->push_back(node);
    }

    if (offset != len) {
        nodes->clear();
        return false;
    }
    return true;
}

bool buildClusterSnapshotMessage(const std::map<std::string, ClusterNodeState> &nodes,
                                 uint64_t cluster_version,
                                 const std::string &ts,
                                 Message *msg)
{
    std::vector<uint8_t> encoded_nodes;

    if (msg == NULL || !encodeClusterSnapshotNodes(nodes, &encoded_nodes)) {
        return false;
    }

    messageInit(msg, MessageType::HUB_CLUSTER_SNAPSHOT, (uint32_t)cluster_version);
    if (!messageAddU64ByTag(msg, FieldTag::CLUSTER_VERSION, cluster_version) ||
        !messageAddU32ByTag(msg, FieldTag::NODE_COUNT, (uint32_t)nodes.size()) ||
        !messageAddBytesByTag(msg, FieldTag::NODES, encoded_nodes.data(), (uint16_t)encoded_nodes.size()) ||
        !messageAddStringByTag(msg, FieldTag::TS, ts.c_str())) {
        return false;
    }

    return true;
}

bool parseClusterSnapshotMessage(const Message &msg, uint64_t *cluster_version, std::vector<ClusterNodeState> *nodes)
{
    const uint8_t *encoded_nodes = NULL;
    uint16_t encoded_nodes_len = 0;
    uint32_t node_count = 0;
    uint64_t parsed_cluster_version = 0;

    if (cluster_version == NULL || nodes == NULL || msg.type != MessageType::HUB_CLUSTER_SNAPSHOT) {
        return false;
    }
    if (!messageGetU64(&msg, "cluster_version", &parsed_cluster_version) ||
        !messageGetU32ByTag(&msg, FieldTag::NODE_COUNT, &node_count) ||
        !messageGetBytesByTag(&msg, FieldTag::NODES, &encoded_nodes, &encoded_nodes_len) ||
        !decodeClusterSnapshotNodes(encoded_nodes, encoded_nodes_len, nodes)) {
        return false;
    }
    if (nodes->size() != node_count) {
        nodes->clear();
        return false;
    }

    *cluster_version = parsed_cluster_version;
    return true;
}

}  // namespace ntrs
}  // namespace eular
