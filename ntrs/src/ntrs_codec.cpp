#include "ntrs_codec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits>

namespace eular {
namespace ntrs {

namespace {

struct FieldDef {
    const char* key;
    FieldTag    tag;
    FieldType   type;
};

static const FieldDef kFieldDefs[] = {
    {"assignment_backup1", FieldTag::ASSIGNMENT_BACKUP1, FieldType::BYTES},
    {"assignment_primary1", FieldTag::ASSIGNMENT_PRIMARY1, FieldType::BYTES},
    {"assignment_primary2", FieldTag::ASSIGNMENT_PRIMARY2, FieldType::BYTES},
    {"assignment_version", FieldTag::ASSIGNMENT_VERSION, FieldType::U32},
    {"backup1_control", FieldTag::BACKUP1_CONTROL, FieldType::BYTES},
    {"boot_id", FieldTag::BOOT_ID, FieldType::BYTES},
    {"clusterVersion", FieldTag::CLUSTER_VERSION, FieldType::U64},
    {"code", FieldTag::CODE, FieldType::BYTES},
    {"control_endpoint", FieldTag::CONTROL_ENDPOINT, FieldType::BYTES},
    {"connect_role", FieldTag::CONNECT_ROLE, FieldType::U8},
    {"device_id", FieldTag::DEVICE_ID, FieldType::BYTES},
    {"dst_device_id", FieldTag::DST_DEVICE_ID, FieldType::BYTES},
    {"diff_ip_sent", FieldTag::DIFF_IP_SENT, FieldType::BOOL},
    {"dst_peer_id", FieldTag::DST_PEER_ID, FieldType::BYTES},
    {"event", FieldTag::EVENT, FieldType::U8},
    {"expire_at", FieldTag::EXPIRE_AT, FieldType::U32},
    {"federation", FieldTag::FEDERATION, FieldType::BYTES},
    {"filter_diff_ip_rx", FieldTag::FILTER_DIFF_IP_RX, FieldType::BOOL},
    {"filter_same_ip_diff_port_rx", FieldTag::FILTER_SAME_IP_DIFF_PORT_RX, FieldType::BOOL},
    {"filtering_behavior", FieldTag::FILTERING_BEHAVIOR, FieldType::U16},
    {"heartbeat_interval_sec", FieldTag::HEARTBEAT_INTERVAL_SEC, FieldType::U32},
    {"lease_default_sec", FieldTag::LEASE_DEFAULT_SEC, FieldType::U32},
    {"lease_sec", FieldTag::LEASE_SEC, FieldType::U32},
    {"lease_seq", FieldTag::LEASE_SEQ, FieldType::U32},
    {"load", FieldTag::LOAD, FieldType::U32},
    {"local_ip", FieldTag::LOCAL_IP, FieldType::BYTES},
    {"local_port", FieldTag::LOCAL_PORT, FieldType::U16},
    {"mapping_stable", FieldTag::MAPPING_STABLE, FieldType::BOOL},
    {"mapping_behavior", FieldTag::MAPPING_BEHAVIOR, FieldType::U16},
    {"message", FieldTag::MESSAGE, FieldType::BYTES},
    {"nat_class", FieldTag::NAT_CLASS, FieldType::U16},
    {"nat_flags", FieldTag::NAT_FLAGS, FieldType::U16},
    {"nat_risk", FieldTag::NAT_RISK, FieldType::BYTES},
    {"nat_type", FieldTag::NAT_TYPE, FieldType::BYTES},
    {"node_count", FieldTag::NODE_COUNT, FieldType::U32},
    {"node_id", FieldTag::NODE_ID, FieldType::BYTES},
    {"nodes", FieldTag::NODES, FieldType::BYTES},
    {"peer_id", FieldTag::PEER_ID, FieldType::BYTES},
    {"peer_device_id", FieldTag::PEER_DEVICE_ID, FieldType::BYTES},
    {"peer_nat_class", FieldTag::PEER_NAT_CLASS, FieldType::U16},
    {"peer_nat_flags", FieldTag::PEER_NAT_FLAGS, FieldType::U16},
    {"peer_nat_type", FieldTag::PEER_NAT_TYPE, FieldType::BYTES},
    {"peer_filtering_behavior", FieldTag::PEER_FILTERING_BEHAVIOR, FieldType::U16},
    {"peer_local_ip", FieldTag::PEER_LOCAL_IP, FieldType::BYTES},
    {"peer_local_port", FieldTag::PEER_LOCAL_PORT, FieldType::U16},
    {"peer_mapping_behavior", FieldTag::PEER_MAPPING_BEHAVIOR, FieldType::U16},
    {"peer_srflx_ip", FieldTag::PEER_SRFLX_IP, FieldType::BYTES},
    {"peer_srflx_ip_2", FieldTag::PEER_SRFLX_IP_2, FieldType::BYTES},
    {"peer_srflx_port", FieldTag::PEER_SRFLX_PORT, FieldType::U16},
    {"peer_srflx_port_2", FieldTag::PEER_SRFLX_PORT_2, FieldType::U16},
    {"primary1_control", FieldTag::PRIMARY1_CONTROL, FieldType::BYTES},
    {"primary2_control", FieldTag::PRIMARY2_CONTROL, FieldType::BYTES},
    {"probe1", FieldTag::PROBE1, FieldType::BYTES},
    {"probe2", FieldTag::PROBE2, FieldType::BYTES},
    {"probe_endpoint", FieldTag::PROBE_ENDPOINT, FieldType::BYTES},
    {"probe_token", FieldTag::PROBE_TOKEN, FieldType::BYTES},
    {"warmup_interval_ms", FieldTag::WARMUP_INTERVAL_MS, FieldType::U32},
    {"warmup_rounds", FieldTag::WARMUP_ROUNDS, FieldType::U32},
    {"punch_order", FieldTag::PUNCH_ORDER, FieldType::U8},
    {"probe1_distinct_mappings", FieldTag::PROBE1_DISTINCT_MAPPINGS, FieldType::U32},
    {"probe1_ok", FieldTag::PROBE1_OK, FieldType::BOOL},
    {"probe1_rtt_ms", FieldTag::PROBE1_RTT_MS, FieldType::I32},
    {"probe1_success_count", FieldTag::PROBE1_SUCCESS_COUNT, FieldType::U32},
    {"probe2_distinct_mappings", FieldTag::PROBE2_DISTINCT_MAPPINGS, FieldType::U32},
    {"probe2_ok", FieldTag::PROBE2_OK, FieldType::BOOL},
    {"probe2_rtt_ms", FieldTag::PROBE2_RTT_MS, FieldType::I32},
    {"probe2_success_count", FieldTag::PROBE2_SUCCESS_COUNT, FieldType::U32},
    {"probe_rounds", FieldTag::PROBE_ROUNDS, FieldType::U32},
    {"query", FieldTag::QUERY, FieldType::BYTES},
    {"reason", FieldTag::REASON, FieldType::U8},
    {"region", FieldTag::REGION, FieldType::BYTES},
    {"result", FieldTag::RESULT, FieldType::U8},
    {"role", FieldTag::ROLE, FieldType::U8},
    {"same_ip_diff_port_sent", FieldTag::SAME_IP_DIFF_PORT_SENT, FieldType::BOOL},
    {"selected_control", FieldTag::SELECTED_CONTROL, FieldType::BYTES},
    {"session_id", FieldTag::SESSION_ID, FieldType::BYTES},
    {"src_peer_id", FieldTag::SRC_PEER_ID, FieldType::BYTES},
    {"src_device_id", FieldTag::SRC_DEVICE_ID, FieldType::BYTES},
    {"srflx_ip", FieldTag::SRFLX_IP, FieldType::BYTES},
    {"srflx_ip_2", FieldTag::SRFLX_IP_2, FieldType::BYTES},
    {"srflx_port", FieldTag::SRFLX_PORT, FieldType::U16},
    {"srflx_port_2", FieldTag::SRFLX_PORT_2, FieldType::U16},
    {"status", FieldTag::STATUS, FieldType::U8},
    {"target_ip", FieldTag::TARGET_IP, FieldType::BYTES},
    {"target_port", FieldTag::TARGET_PORT, FieldType::U16},
    {"token", FieldTag::TOKEN, FieldType::BYTES},
    {"ts", FieldTag::TS, FieldType::BYTES},
    {"use_alt_port", FieldTag::USE_ALT_PORT, FieldType::BOOL},
    {"version", FieldTag::VERSION, FieldType::BYTES},
};

static void PutU64Be(uint8_t* p, uint64_t value)
{
    p[0] = (uint8_t)((value >> 56) & 0xFFu);
    p[1] = (uint8_t)((value >> 48) & 0xFFu);
    p[2] = (uint8_t)((value >> 40) & 0xFFu);
    p[3] = (uint8_t)((value >> 32) & 0xFFu);
    p[4] = (uint8_t)((value >> 24) & 0xFFu);
    p[5] = (uint8_t)((value >> 16) & 0xFFu);
    p[6] = (uint8_t)((value >> 8) & 0xFFu);
    p[7] = (uint8_t)(value & 0xFFu);
}

static uint64_t GetU64Be(const uint8_t* p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static const FieldDef* FindFieldDefByKey(const char* key)
{
    size_t i = 0;
    if (key == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(kFieldDefs) / sizeof(kFieldDefs[0]); ++i) {
        if (strcmp(kFieldDefs[i].key, key) == 0) {
            return &kFieldDefs[i];
        }
    }
    return NULL;
}

static const FieldDef* FindFieldDefByTag(uint16_t tag)
{
    static const FieldDef* tag_index[Message::TAG_INDEX_SIZE] = {0};
    static bool            initialized = false;
    if (!initialized) {
        for (size_t i = 0; i < sizeof(kFieldDefs) / sizeof(kFieldDefs[0]); ++i) {
            uint16_t def_tag = (uint16_t)kFieldDefs[i].tag;
            if (def_tag < Message::TAG_INDEX_SIZE) {
                tag_index[def_tag] = &kFieldDefs[i];
            }
        }
        initialized = true;
    }
    if (tag < Message::TAG_INDEX_SIZE) {
        return tag_index[tag];
    }
    for (size_t i = 0; i < sizeof(kFieldDefs) / sizeof(kFieldDefs[0]); ++i) {
        if ((uint16_t)kFieldDefs[i].tag == tag) {
            return &kFieldDefs[i];
        }
    }
    return NULL;
}

static void PutU16Be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)((value >> 8) & 0xFFu);
    p[1] = (uint8_t)(value & 0xFFu);
}

static void PutU32Be(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)((value >> 24) & 0xFFu);
    p[1] = (uint8_t)((value >> 16) & 0xFFu);
    p[2] = (uint8_t)((value >> 8) & 0xFFu);
    p[3] = (uint8_t)(value & 0xFFu);
}

static uint16_t GetU16Be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]); }

static uint32_t GetU32Be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool AppendField(Message* msg, const char* key, FieldType type, const void* data, uint16_t len, uint64_t u64_num,
                        int32_t i32_num)
{
    MessageField*   field = NULL;
    const FieldDef* def = NULL;
    if (msg == NULL || key == NULL || msg->field_count >= Message::MAX_FIELDS) {
        return false;
    }

    def = FindFieldDefByKey(key);
    if (type == FieldType::BYTES && msg->storage_len + (size_t)len + 1u > Message::STORAGE_SIZE) {
        return false;
    }
    field = &msg->fields[msg->field_count++];
    field->key = def != NULL ? def->key : key;
    field->tag = def != NULL ? (uint16_t)def->tag : 0;
    field->type = type;
    field->value_len = len;
    switch (type) {
    case FieldType::BOOL:
        field->value.boolean = (uint8_t)u64_num;
        break;
    case FieldType::U8:
        field->value.u8 = (uint8_t)u64_num;
        break;
    case FieldType::U16:
        field->value.u16 = (uint16_t)u64_num;
        break;
    case FieldType::U32:
        field->value.u32 = (uint32_t)u64_num;
        break;
    case FieldType::I32:
        field->value.i32 = i32_num;
        break;
    case FieldType::U64:
        field->value.u64 = u64_num;
        break;
    case FieldType::BYTES:
    default:
        if (len > 0 && data != NULL) {
            memcpy(msg->storage + msg->storage_len, data, len);
        }
        msg->storage[msg->storage_len + len] = '\0';
        field->value.bytes = msg->storage + msg->storage_len;
        msg->storage_len += (size_t)len + 1u;
        break;
    }
    if (field->tag < Message::TAG_INDEX_SIZE) {
        msg->tag_index[field->tag] = (int16_t)(msg->field_count - 1);
    }
    return true;
}

static bool AppendFieldByTag(Message* msg, FieldTag tag, FieldType type, const void* data, uint16_t len,
                             uint64_t u64_num, int32_t i32_num)
{
    MessageField*   field = NULL;
    const FieldDef* def = NULL;
    uint16_t        tag_value = (uint16_t)tag;
    if (msg == NULL || msg->field_count >= Message::MAX_FIELDS) {
        return false;
    }

    def = FindFieldDefByTag(tag_value);
    if (type == FieldType::BYTES && msg->storage_len + (size_t)len + 1u > Message::STORAGE_SIZE) {
        return false;
    }
    field = &msg->fields[msg->field_count++];
    field->key = def != NULL ? def->key : NULL;
    field->tag = tag_value;
    field->type = type;
    field->value_len = len;
    switch (type) {
    case FieldType::BOOL:
        field->value.boolean = (uint8_t)u64_num;
        break;
    case FieldType::U8:
        field->value.u8 = (uint8_t)u64_num;
        break;
    case FieldType::U16:
        field->value.u16 = (uint16_t)u64_num;
        break;
    case FieldType::U32:
        field->value.u32 = (uint32_t)u64_num;
        break;
    case FieldType::I32:
        field->value.i32 = i32_num;
        break;
    case FieldType::U64:
        field->value.u64 = u64_num;
        break;
    case FieldType::BYTES:
    default:
        if (len > 0 && data != NULL) {
            memcpy(msg->storage + msg->storage_len, data, len);
        }
        msg->storage[msg->storage_len + len] = '\0';
        field->value.bytes = msg->storage + msg->storage_len;
        msg->storage_len += (size_t)len + 1u;
        break;
    }
    if (field->tag < Message::TAG_INDEX_SIZE) {
        msg->tag_index[field->tag] = (int16_t)(msg->field_count - 1);
    }
    return true;
}

static MessageField* FindField(Message* msg, const char* key)
{
    uint16_t i = 0;
    if (msg == NULL || key == NULL) {
        return NULL;
    }
    for (i = 0; i < msg->field_count; ++i) {
        if (msg->fields[i].key != NULL && strcmp(msg->fields[i].key, key) == 0) {
            return &msg->fields[i];
        }
    }
    return NULL;
}

static const MessageField* FindFieldConst(const Message* msg, const char* key)
{
    return FindField(const_cast<Message*>(msg), key);
}

static MessageField* FindFieldByTag(Message* msg, FieldTag tag)
{
    if (msg == NULL) {
        return NULL;
    }
    if ((uint16_t)tag < Message::TAG_INDEX_SIZE) {
        int16_t field_index = msg->tag_index[(uint16_t)tag];
        if (field_index >= 0 && (uint16_t)field_index < msg->field_count) {
            return &msg->fields[(uint16_t)field_index];
        }
    }
    for (uint16_t i = 0; i < msg->field_count; ++i) {
        if (msg->fields[i].tag == (uint16_t)tag) {
            if ((uint16_t)tag < Message::TAG_INDEX_SIZE) {
                msg->tag_index[(uint16_t)tag] = (int16_t)i;
            }
            return &msg->fields[i];
        }
    }
    return NULL;
}

static const MessageField* FindFieldConstByTag(const Message* msg, FieldTag tag)
{
    return FindFieldByTag(const_cast<Message*>(msg), tag);
}

static bool CopyDecodedBytes(Message* msg, const FieldDef* def, const uint8_t* data, uint16_t len)
{
    MessageField* field = NULL;
    if (msg == NULL || def == NULL || data == NULL) {
        return false;
    }

    if (msg->field_count >= Message::MAX_FIELDS) {
        return false;
    }
    switch (def->type) {
    case FieldType::BOOL:
    case FieldType::U8:
        if (len != 1) {
            return false;
        }
        break;
    case FieldType::U16:
        if (len != 2) {
            return false;
        }
        break;
    case FieldType::U32:
    case FieldType::I32:
        if (len != 4) {
            return false;
        }
        break;
    case FieldType::U64:
        if (len != 8) {
            return false;
        }
        break;
    case FieldType::BYTES:
    default:
        if (msg->storage_len + (size_t)len + 1u > Message::STORAGE_SIZE) {
            return false;
        }
        break;
    }

    field = &msg->fields[msg->field_count++];
    field->key = def->key;
    field->tag = (uint16_t)def->tag;
    field->type = def->type;

    switch (def->type) {
    case FieldType::BOOL:
        field->value.boolean = data[0] == 0 ? 0 : 1;
        field->value_len = 1;
        break;
    case FieldType::U8:
        field->value.u8 = data[0];
        field->value_len = 1;
        break;
    case FieldType::U16:
        field->value.u16 = GetU16Be(data);
        field->value_len = 2;
        break;
    case FieldType::U32:
        field->value.u32 = GetU32Be(data);
        field->value_len = 4;
        break;
    case FieldType::I32:
        field->value.i32 = (int32_t)GetU32Be(data);
        field->value_len = 4;
        break;
    case FieldType::U64:
        field->value.u64 = GetU64Be(data);
        field->value_len = 8;
        break;
    case FieldType::BYTES:
    default:
        field->value_len = len;
        memcpy(msg->storage + msg->storage_len, data, len);
        msg->storage[msg->storage_len + len] = '\0';
        field->value.bytes = msg->storage + msg->storage_len;
        msg->storage_len += (size_t)len + 1u;
        break;
    }
    if (field->tag < Message::TAG_INDEX_SIZE) {
        msg->tag_index[field->tag] = (int16_t)(msg->field_count - 1);
    }
    return true;
}

}  // namespace

const char* ResultCodeName(ResultCode code)
{
    switch (code) {
    case ResultCode::OK:
        return "ok";
    case ResultCode::DEGRADED:
        return "degraded";
    case ResultCode::FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

const char* RoleCodeName(RoleCode code)
{
    switch (code) {
    case RoleCode::INITIATOR:
        return "initiator";
    case RoleCode::RESPONDER:
        return "responder";
    default:
        return "unknown";
    }
}

const char* NodeStatusCodeName(NodeStatusCode code)
{
    switch (code) {
    case NodeStatusCode::REGISTERED:
        return "registered";
    case NodeStatusCode::ONLINE:
        return "online";
    case NodeStatusCode::OFFLINE:
        return "offline";
    default:
        return "unknown";
    }
}

const char* ReasonCodeName(ReasonCode code)
{
    switch (code) {
    case ReasonCode::STARTUP:
        return "startup";
    case ReasonCode::LWT:
        return "lwt";
    case ReasonCode::CLIENT_EXIT:
        return "client_exit";
    default:
        return "none";
    }
}

const char* EventCodeName(EventCode code)
{
    switch (code) {
    case EventCode::ASSIGNMENT:
        return "assignment";
    case EventCode::NODE_REGISTERED:
        return "node_registered";
    case EventCode::NODE_GENERATION_REPLACED:
        return "node_generation_replaced";
    case EventCode::NODE_ONLINE:
        return "node_online";
    case EventCode::NODE_ABNORMAL_OFFLINE:
        return "node_abnormal_offline";
    case EventCode::NODE_OFFLINE:
        return "node_offline";
    case EventCode::NODE_STATUS_CHANGED:
        return "node_status_changed";
    case EventCode::NODE_HEARTBEAT:
        return "node_heartbeat";
    case EventCode::NODE_EVICTED:
        return "node_evicted";
    default:
        return "unknown";
    }
}

void MessageInit(Message* msg, MessageType type, uint32_t request_id)
{
    if (msg == NULL) {
        return;
    }
    memset(msg, 0, sizeof(*msg));
    msg->type = type;
    msg->request_id = request_id;
    for (uint16_t i = 0; i < Message::TAG_INDEX_SIZE; ++i) {
        msg->tag_index[i] = -1;
    }
}

bool MessageAddString(Message* msg, const char* key, const char* value)
{
    if (value == NULL) {
        value = "";
    }

    return AppendField(msg, key, FieldType::BYTES, value, (uint16_t)strlen(value), 0, 0);
}

bool MessageAddStringByTag(Message* msg, FieldTag tag, const char* value)
{
    if (value == NULL) {
        value = "";
    }
    return AppendFieldByTag(msg, tag, FieldType::BYTES, value, (uint16_t)strlen(value), 0, 0);
}

bool MessageAddBytes(Message* msg, const char* key, const void* data, uint16_t len)
{
    if (data == NULL && len != 0) {
        return false;
    }
    return AppendField(msg, key, FieldType::BYTES, data, len, 0, 0);
}

bool MessageAddBytesByTag(Message* msg, FieldTag tag, const void* data, uint16_t len)
{
    if (data == NULL && len != 0) {
        return false;
    }
    return AppendFieldByTag(msg, tag, FieldType::BYTES, data, len, 0, 0);
}

bool MessageAddBool(Message* msg, const char* key, bool value)
{
    return AppendField(msg, key, FieldType::BOOL, NULL, 1, value ? 1u : 0u, 0);
}

bool MessageAddBoolByTag(Message* msg, FieldTag tag, bool value)
{
    return AppendFieldByTag(msg, tag, FieldType::BOOL, NULL, 1, value ? 1u : 0u, 0);
}

bool MessageAddU8(Message* msg, const char* key, uint8_t value)
{
    return AppendField(msg, key, FieldType::U8, NULL, 1, value, 0);
}

bool MessageAddU8ByTag(Message* msg, FieldTag tag, uint8_t value)
{
    return AppendFieldByTag(msg, tag, FieldType::U8, NULL, 1, value, 0);
}

bool MessageAddU16(Message* msg, const char* key, uint16_t value)
{
    return AppendField(msg, key, FieldType::U16, NULL, 2, value, 0);
}

bool MessageAddU16ByTag(Message* msg, FieldTag tag, uint16_t value)
{
    return AppendFieldByTag(msg, tag, FieldType::U16, NULL, 2, value, 0);
}

bool MessageAddU32(Message* msg, const char* key, uint32_t value)
{
    return AppendField(msg, key, FieldType::U32, NULL, 4, value, 0);
}

bool MessageAddU32ByTag(Message* msg, FieldTag tag, uint32_t value)
{
    return AppendFieldByTag(msg, tag, FieldType::U32, NULL, 4, value, 0);
}

bool MessageAddI32(Message* msg, const char* key, int32_t value)
{
    return AppendField(msg, key, FieldType::I32, NULL, 4, 0, value);
}

bool MessageAddI32ByTag(Message* msg, FieldTag tag, int32_t value)
{
    return AppendFieldByTag(msg, tag, FieldType::I32, NULL, 4, 0, value);
}

bool MessageAddU64(Message* msg, const char* key, uint64_t value)
{
    return AppendField(msg, key, FieldType::U64, NULL, 8, value, 0);
}

bool MessageAddU64ByTag(Message* msg, FieldTag tag, uint64_t value)
{
    return AppendFieldByTag(msg, tag, FieldType::U64, NULL, 8, value, 0);
}

const char* MessageGetString(const Message* msg, const char* key)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || field->type != FieldType::BYTES || field->value.bytes == NULL) {
        return "";
    }
    return reinterpret_cast<const char*>(field->value.bytes);
}

const char* MessageGetStringByTag(const Message* msg, FieldTag tag)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || field->type != FieldType::BYTES || field->value.bytes == NULL) {
        return "";
    }
    return reinterpret_cast<const char*>(field->value.bytes);
}

bool MessageGetBytes(const Message* msg, const char* key, const uint8_t** data, uint16_t* len)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || data == NULL || len == NULL || field->type != FieldType::BYTES) {
        return false;
    }

    *data = field->value.bytes;
    *len = field->value_len;
    return true;
}

bool MessageGetBytesByTag(const Message* msg, FieldTag tag, const uint8_t** data, uint16_t* len)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || data == NULL || len == NULL || field->type != FieldType::BYTES) {
        return false;
    }

    *data = field->value.bytes;
    *len = field->value_len;
    return true;
}

bool MessageGetBool(const Message* msg, const char* key, bool* value)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || value == NULL || field->type != FieldType::BOOL) {
        return false;
    }
    *value = field->value.boolean != 0;
    return true;
}

bool MessageGetBoolByTag(const Message* msg, FieldTag tag, bool* value)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || value == NULL || field->type != FieldType::BOOL) {
        return false;
    }
    *value = field->value.boolean != 0;
    return true;
}

bool MessageGetU8(const Message* msg, const char* key, uint8_t* value)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || value == NULL || field->type != FieldType::U8) {
        return false;
    }
    *value = field->value.u8;
    return true;
}

bool MessageGetU8ByTag(const Message* msg, FieldTag tag, uint8_t* value)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || value == NULL || field->type != FieldType::U8) {
        return false;
    }
    *value = field->value.u8;
    return true;
}

bool MessageGetU16(const Message* msg, const char* key, uint16_t* value)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || value == NULL || field->type != FieldType::U16) {
        return false;
    }
    *value = field->value.u16;
    return true;
}

bool MessageGetU16ByTag(const Message* msg, FieldTag tag, uint16_t* value)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || value == NULL || field->type != FieldType::U16) {
        return false;
    }
    *value = field->value.u16;
    return true;
}

bool MessageGetU32(const Message* msg, const char* key, uint32_t* value)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || value == NULL || field->type != FieldType::U32) {
        return false;
    }
    *value = field->value.u32;
    return true;
}

bool MessageGetU32ByTag(const Message* msg, FieldTag tag, uint32_t* value)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || value == NULL || field->type != FieldType::U32) {
        return false;
    }
    *value = field->value.u32;
    return true;
}

bool MessageGetI32(const Message* msg, const char* key, int32_t* value)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || value == NULL || field->type != FieldType::I32) {
        return false;
    }
    *value = field->value.i32;
    return true;
}

bool MessageGetI32ByTag(const Message* msg, FieldTag tag, int32_t* value)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || value == NULL) {
        return false;
    }
    if (field->type != FieldType::I32 || field->value_len != 4) {
        return false;
    }
    *value = field->value.i32;
    return true;
}

bool MessageGetU64(const Message* msg, const char* key, uint64_t* value)
{
    const MessageField* field = FindFieldConst(msg, key);
    if (field == NULL || value == NULL || field->type != FieldType::U64) {
        return false;
    }
    *value = field->value.u64;
    return true;
}

bool MessageGetU64ByTag(const Message* msg, FieldTag tag, uint64_t* value)
{
    const MessageField* field = FindFieldConstByTag(msg, tag);
    if (field == NULL || value == NULL) {
        return false;
    }
    if (field->type != FieldType::U64 || field->value_len != 8) {
        return false;
    }
    *value = field->value.u64;
    return true;
}

bool MessageFieldEquals(const Message* msg, const char* key, const char* value)
{
    const char* current = MessageGetString(msg, key);
    if (value == NULL) {
        value = "";
    }
    return strcmp(current, value) == 0;
}

bool MessageFieldEqualsByTag(const Message* msg, FieldTag tag, const char* value)
{
    const char* current = MessageGetStringByTag(msg, tag);
    if (value == NULL) {
        value = "";
    }
    return strcmp(current, value) == 0;
}

int EncodeMessage(const Message& msg, void* buf, size_t cap, size_t* out_len)
{
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t   off = FRAME_HDR_SIZE;
    uint16_t i = 0;

    if (buf == NULL || out_len == NULL || cap < FRAME_HDR_SIZE) {
        return -1;
    }

    for (i = 0; i < msg.field_count; ++i) {
        const MessageField& field = msg.fields[i];
        size_t              required = TLV_HDR_SIZE + field.value_len;
        if (field.tag == 0) {
            continue;
        }
        if (off + required > cap) {
            return -2;
        }

        PutU16Be(out + off, field.tag);
        PutU16Be(out + off + 2u, field.value_len);
        off += TLV_HDR_SIZE;

        switch (field.type) {
        case FieldType::BOOL:
            out[off] = field.value.boolean;
            break;
        case FieldType::U8:
            out[off] = field.value.u8;
            break;
        case FieldType::U16:
            PutU16Be(out + off, field.value.u16);
            break;
        case FieldType::U32:
            PutU32Be(out + off, field.value.u32);
            break;
        case FieldType::I32:
            PutU32Be(out + off, (uint32_t)field.value.i32);
            break;
        case FieldType::U64:
            PutU64Be(out + off, field.value.u64);
            break;
        case FieldType::BYTES:
        default:
            if (field.value_len > 0 && field.value.bytes != NULL) {
                memcpy(out + off, field.value.bytes, field.value_len);
            }
            break;
        }
        off += field.value_len;
    }

    PutU32Be(out + 0u, FRAME_MAGIC);
    out[4] = FRAME_VERSION;
    out[5] = (uint8_t)msg.type;
    PutU16Be(out + 6u, 0);
    PutU32Be(out + 8u, msg.request_id);
    PutU32Be(out + 12u, (uint32_t)(off - FRAME_HDR_SIZE));
    *out_len = off;
    return 0;
}

bool FrameSizeFromHeader(const void* data, size_t len, uint32_t* frame_size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t       body_len = 0;
    if (p == NULL || frame_size == NULL || len < FRAME_HDR_SIZE) {
        return false;
    }
    if (GetU32Be(p) != FRAME_MAGIC || p[4] != FRAME_VERSION) {
        return false;
    }
    body_len = GetU32Be(p + 12u);
    *frame_size = (uint32_t)(FRAME_HDR_SIZE + body_len);
    return true;
}

bool DecodeMessage(const void* data, size_t len, Message* msg)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t       frame_size = 0;
    uint32_t       body_len = 0;
    size_t         off = FRAME_HDR_SIZE;

    if (p == NULL || msg == NULL) {
        return false;
    }

    MessageInit(msg, MessageType::UNKNOWN, 0);
    if (!FrameSizeFromHeader(data, len, &frame_size)) {
        return false;
    }
    if ((size_t)frame_size > len || frame_size < FRAME_HDR_SIZE) {
        return false;
    }
    body_len = frame_size - (uint32_t)FRAME_HDR_SIZE;

    msg->type = (MessageType)p[5];
    msg->request_id = GetU32Be(p + 8u);

    while (off + TLV_HDR_SIZE <= FRAME_HDR_SIZE + (size_t)body_len) {
        const FieldDef* def = NULL;
        uint16_t        tag = GetU16Be(p + off);
        uint16_t        value_len = GetU16Be(p + off + 2u);
        off += TLV_HDR_SIZE;
        if (off + value_len > FRAME_HDR_SIZE + (size_t)body_len) {
            return false;
        }
        def = FindFieldDefByTag(tag);
        if (def != NULL && !CopyDecodedBytes(msg, def, p + off, value_len)) {
            return false;
        }
        off += value_len;
    }

    return msg->type != MessageType::UNKNOWN;
}

}  // namespace ntrs
}  // namespace eular
