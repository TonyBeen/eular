#ifndef __NTRS_CODEC_H__
#define __NTRS_CODEC_H__

#include <stdint.h>

#include <map>
#include <string>

namespace eular {
namespace ntrs {

enum class MessageType {
    UNKNOWN = 0,
    AUTH_REQ,
    AUTH_RSP,
    REGISTER_REQ,
    REGISTER_RSP,
    HEARTBEAT_REQ,
    HEARTBEAT_RSP,
    SESSION_CREATE_REQ,
    SESSION_CREATE_RSP,
    SESSION_NOTIFY,
    UNREGISTER_REQ,
    UNREGISTER_RSP,
    NAT_PROBE_REQ,
    NAT_PROBE_RSP,
    SERVER_INFO_REQ,
    SERVER_INFO_RSP,
    FILTER_PROBE_REQ,
    FILTER_PROBE_RSP,
    SERVER_SEND_PROBE_REQ,
    SERVER_SEND_PROBE_RSP,
    NODE_REGISTER,
    NODE_PRESENCE,
    NODE_HEARTBEAT,
    HUB_CLUSTER_SNAPSHOT,
    HUB_CLUSTER_EVENT,
    ERROR_RSP,
};

struct Message {
    MessageType type;
    uint64_t request_id;
    std::map<std::string, std::string> fields;
};

const char *messageTypeToString(MessageType type);
MessageType messageTypeFromString(const std::string &value);

// Line based protocol:
// TYPE=<type>|REQ=<id>|k1=v1|k2=v2\n
std::string encodeMessage(const Message &msg);
bool decodeMessage(const std::string &line, Message *msg);

} // namespace ntrs
} // namespace eular

#endif // __NTRS_CODEC_H__
