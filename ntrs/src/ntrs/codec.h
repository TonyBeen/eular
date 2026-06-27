#ifndef __NTRS_CODEC_H__
#define __NTRS_CODEC_H__

#include <stddef.h>
#include <stdint.h>

namespace eular {
namespace ntrs {

enum class FieldTag : uint16_t {
    RESULT = 0x0001,
    REASON = 0x0002,
    CODE = 0x0003,
    MESSAGE = 0x0004,
    PEER_ID = 0x0010,
    DEVICE_ID = 0x0011,
    SRC_PEER_ID = 0x0012,
    DST_PEER_ID = 0x0013,
    SESSION_ID = 0x0014,
    TOKEN = 0x0015,
    ROLE = 0x0016,
    QUERY = 0x0017,
    SRC_DEVICE_ID = 0x0018,
    DST_DEVICE_ID = 0x0019,
    PEER_DEVICE_ID = 0x001A,
    LOCAL_IP = 0x0020,
    LOCAL_PORT = 0x0021,
    SRFLX_IP = 0x0022,
    SRFLX_PORT = 0x0023,
    SRFLX_IP_2 = 0x0024,
    SRFLX_PORT_2 = 0x0025,
    TARGET_IP = 0x0026,
    TARGET_PORT = 0x0027,
    CONTROL_ENDPOINT = 0x002B,
    SELECTED_CONTROL = 0x002C,
    USE_ALT_PORT = 0x002E,
    NAT_TYPE = 0x0031,
    PROBE1_OK = 0x0033,
    PROBE2_OK = 0x0034,
    PROBE1_RTT_MS = 0x0035,
    PROBE2_RTT_MS = 0x0036,
    PROBE_ROUNDS = 0x0037,
    PROBE1_SUCCESS_COUNT = 0x0038,
    PROBE2_SUCCESS_COUNT = 0x0039,
    PROBE1_DISTINCT_MAPPINGS = 0x003A,
    PROBE2_DISTINCT_MAPPINGS = 0x003B,
    SAME_IP_DIFF_PORT_SENT = 0x003E,
    DIFF_IP_SENT = 0x003F,
    NODE_ID = 0x0040,
    BOOT_ID = 0x0041,
    REGION = 0x0042,
    STATUS = 0x0043,
    LOAD = 0x0044,
    TS = 0x0045,
    EVENT = 0x0046,
    CLUSTER_VERSION = 0x0047,
    NODE_COUNT = 0x0048,
    NODES = 0x0049,
    LEASE_SEC = 0x0050,
    HEARTBEAT_INTERVAL_SEC = 0x0051,
    LEASE_DEFAULT_SEC = 0x0052,
    LEASE_SEQ = 0x0053,
    EXPIRE_AT = 0x0054,
    VERSION = 0x0055,
    FEDERATION = 0x0056,
    ASSIGNMENT_VERSION = 0x0060,
    ASSIGNMENT_PRIMARY1 = 0x0061,
    ASSIGNMENT_PRIMARY2 = 0x0062,
    ASSIGNMENT_BACKUP1 = 0x0063,
    PRIMARY1_CONTROL = 0x0064,
    PRIMARY2_CONTROL = 0x0065,
    BACKUP1_CONTROL = 0x0066,
    NAT_CLASS = 0x0067,
    PEER_SRFLX_IP = 0x0071,
    PEER_SRFLX_PORT = 0x0072,
    PEER_SRFLX_IP_2 = 0x0073,
    PEER_SRFLX_PORT_2 = 0x0074,
    PEER_NAT_CLASS = 0x0075,
    PEER_LOCAL_IP = 0x0079,
    PEER_LOCAL_PORT = 0x007A,
    PUNCH_ORDER = 0x007B,
    CONNECT_ROLE = 0x007C,
    WARMUP_ROUNDS = 0x007D,
    WARMUP_INTERVAL_MS = 0x007E,
    PROBE1 = 0x007F,
    PROBE2 = 0x0080,
    PROBE_ENDPOINT = 0x0081,
    PROBE_TOKEN = 0x0082,
};

enum class MessageType : uint8_t {
    UNKNOWN = 0,
    AUTH_REQ = 0x01,
    AUTH_RSP = 0x02,
    REGISTER_REQ = 0x10,
    REGISTER_RSP = 0x11,
    HEARTBEAT_REQ = 0x12,
    HEARTBEAT_RSP = 0x13,
    SESSION_CREATE_REQ = 0x20,
    SESSION_CREATE_RSP = 0x21,
    SESSION_NOTIFY = 0x22,
    UNREGISTER_REQ = 0x14,
    UNREGISTER_RSP = 0x15,
    NAT_PROBE_REQ = 0x30,
    NAT_PROBE_RSP = 0x31,
    SERVER_INFO_REQ = 0x34,
    SERVER_INFO_RSP = 0x35,
    FILTER_PROBE_REQ = 0x32,
    FILTER_PROBE_RSP = 0x33,
    SERVER_SEND_PROBE_REQ = 0x36,
    SERVER_SEND_PROBE_RSP = 0x37,
    /**
     * @brief 请求协同节点代发私有过滤探测响应。
     *
     * 该消息用于 `change-ip` 场景下，要求另一台服务节点代为发送一帧私有二进制
     * `FILTER_RSP`。
     */
    SERVER_PROBE_DELEGATE_REQ = 0x38,
    SERVER_PROBE_DELEGATE_RSP = 0x39,
    NODE_REGISTER = 0x40,
    NODE_PRESENCE = 0x41,
    NODE_HEARTBEAT = 0x42,
    HUB_CLUSTER_SNAPSHOT = 0x43,
    HUB_CLUSTER_EVENT = 0x44,
    ERROR_RSP = 0xF0,
};

enum class FieldType : uint8_t {
    BYTES = 0,
    BOOL,
    U8,
    U16,
    U32,
    I32,
    U64,
};

enum class ResultCode : uint8_t {
    UNKNOWN = 0,
    OK = 1,
    DEGRADED = 2,
    FAILED = 3,
};

enum class RoleCode : uint8_t {
    UNKNOWN = 0,
    INITIATOR = 1,
    RESPONDER = 2,
};

enum class PunchOrderCode : uint8_t {
    UNKNOWN = 0,
    SEND_FIRST = 1,
    WAIT_FIRST = 2,
    SIMULTANEOUS = 3,
};

enum class NodeStatusCode : uint8_t {
    UNKNOWN = 0,
    REGISTERED = 1,
    ONLINE = 2,
    OFFLINE = 3,
};

enum class ReasonCode : uint8_t {
    NONE = 0,
    STARTUP = 1,
    LWT = 2,
    CLIENT_EXIT = 3,
};

enum class EventCode : uint8_t {
    UNKNOWN = 0,
    ASSIGNMENT = 1,
    NODE_REGISTERED = 2,
    NODE_GENERATION_REPLACED = 3,
    NODE_ONLINE = 4,
    NODE_ABNORMAL_OFFLINE = 5,
    NODE_OFFLINE = 6,
    NODE_STATUS_CHANGED = 7,
    NODE_HEARTBEAT = 8,
    NODE_EVICTED = 9,
};

struct MessageField {
    const char* key;
    uint16_t    tag;
    FieldType   type;
    union {
        const uint8_t* bytes;
        uint8_t        boolean;
        uint8_t        u8;
        uint16_t       u16;
        uint32_t       u32;
        int32_t        i32;
        uint64_t       u64;
    } value;
    uint16_t value_len;
};

struct Message {
    static const uint16_t MAX_FIELDS = 96;
    static const size_t   STORAGE_SIZE = 8192;
    static const uint16_t TAG_INDEX_SIZE = 128;

    MessageType  type;
    uint32_t     request_id;
    uint16_t     field_count;
    MessageField fields[MAX_FIELDS];
    int16_t      tag_index[TAG_INDEX_SIZE];
    uint8_t      storage[STORAGE_SIZE];
    size_t       storage_len;
};

static const uint32_t FRAME_MAGIC = 0x4E545253u;
static const uint8_t  FRAME_VERSION = 1u;
static const size_t   FRAME_HDR_SIZE = 16u;
static const size_t   TLV_HDR_SIZE = 4u;

void MessageInit(Message* msg, MessageType type = MessageType::UNKNOWN, uint32_t request_id = 0);
bool MessageAddString(Message* msg, const char* key, const char* value);
bool MessageAddStringByTag(Message* msg, FieldTag tag, const char* value);
bool MessageAddBytes(Message* msg, const char* key, const void* data, uint16_t len);
bool MessageAddBytesByTag(Message* msg, FieldTag tag, const void* data, uint16_t len);
bool MessageAddBool(Message* msg, const char* key, bool value);
bool MessageAddBoolByTag(Message* msg, FieldTag tag, bool value);
bool MessageAddU8(Message* msg, const char* key, uint8_t value);
bool MessageAddU8ByTag(Message* msg, FieldTag tag, uint8_t value);
bool MessageAddU16(Message* msg, const char* key, uint16_t value);
bool MessageAddU16ByTag(Message* msg, FieldTag tag, uint16_t value);
bool MessageAddU32(Message* msg, const char* key, uint32_t value);
bool MessageAddU32ByTag(Message* msg, FieldTag tag, uint32_t value);
bool MessageAddI32(Message* msg, const char* key, int32_t value);
bool MessageAddI32ByTag(Message* msg, FieldTag tag, int32_t value);
bool MessageAddU64(Message* msg, const char* key, uint64_t value);
bool MessageAddU64ByTag(Message* msg, FieldTag tag, uint64_t value);

const char* MessageGetString(const Message* msg, const char* key);
const char* MessageGetStringByTag(const Message* msg, FieldTag tag);
bool        MessageGetBytes(const Message* msg, const char* key, const uint8_t** data, uint16_t* len);
bool        MessageGetBytesByTag(const Message* msg, FieldTag tag, const uint8_t** data, uint16_t* len);
bool        MessageGetBool(const Message* msg, const char* key, bool* value);
bool        MessageGetBoolByTag(const Message* msg, FieldTag tag, bool* value);
bool        MessageGetU8(const Message* msg, const char* key, uint8_t* value);
bool        MessageGetU8ByTag(const Message* msg, FieldTag tag, uint8_t* value);
bool        MessageGetU16(const Message* msg, const char* key, uint16_t* value);
bool        MessageGetU16ByTag(const Message* msg, FieldTag tag, uint16_t* value);
bool        MessageGetU32(const Message* msg, const char* key, uint32_t* value);
bool        MessageGetU32ByTag(const Message* msg, FieldTag tag, uint32_t* value);
bool        MessageGetI32(const Message* msg, const char* key, int32_t* value);
bool        MessageGetI32ByTag(const Message* msg, FieldTag tag, int32_t* value);
bool        MessageGetU64(const Message* msg, const char* key, uint64_t* value);
bool        MessageGetU64ByTag(const Message* msg, FieldTag tag, uint64_t* value);
bool        MessageFieldEquals(const Message* msg, const char* key, const char* value);
bool        MessageFieldEqualsByTag(const Message* msg, FieldTag tag, const char* value);

const char* ResultCodeName(ResultCode code);
const char* RoleCodeName(RoleCode code);
const char* NodeStatusCodeName(NodeStatusCode code);
const char* ReasonCodeName(ReasonCode code);
const char* EventCodeName(EventCode code);

int  EncodeMessage(const Message& msg, void* buf, size_t cap, size_t* out_len);
bool DecodeMessage(const void* data, size_t len, Message* msg);
bool FrameSizeFromHeader(const void* data, size_t len, uint32_t* frame_size);

}  // namespace ntrs
}  // namespace eular

#endif  // __NTRS_CODEC_H__
