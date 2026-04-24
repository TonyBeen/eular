#include "ntrs_codec.h"

#include <sstream>
#include <vector>

namespace eular {
namespace ntrs {

static std::vector<std::string> split(const std::string &s, char delim)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}

const char *messageTypeToString(MessageType type)
{
    switch (type) {
    case MessageType::AUTH_REQ: return "AUTH_REQ";
    case MessageType::AUTH_RSP: return "AUTH_RSP";
    case MessageType::REGISTER_REQ: return "REGISTER_REQ";
    case MessageType::REGISTER_RSP: return "REGISTER_RSP";
    case MessageType::HEARTBEAT_REQ: return "HEARTBEAT_REQ";
    case MessageType::HEARTBEAT_RSP: return "HEARTBEAT_RSP";
    case MessageType::SESSION_CREATE_REQ: return "SESSION_CREATE_REQ";
    case MessageType::SESSION_CREATE_RSP: return "SESSION_CREATE_RSP";
    case MessageType::SESSION_NOTIFY: return "SESSION_NOTIFY";
    case MessageType::UNREGISTER_REQ: return "UNREGISTER_REQ";
    case MessageType::UNREGISTER_RSP: return "UNREGISTER_RSP";
    case MessageType::NAT_PROBE_REQ: return "NAT_PROBE_REQ";
    case MessageType::NAT_PROBE_RSP: return "NAT_PROBE_RSP";
    case MessageType::SERVER_INFO_REQ: return "SERVER_INFO_REQ";
    case MessageType::SERVER_INFO_RSP: return "SERVER_INFO_RSP";
    case MessageType::FILTER_PROBE_REQ: return "FILTER_PROBE_REQ";
    case MessageType::FILTER_PROBE_RSP: return "FILTER_PROBE_RSP";
    case MessageType::SERVER_SEND_PROBE_REQ: return "SERVER_SEND_PROBE_REQ";
    case MessageType::SERVER_SEND_PROBE_RSP: return "SERVER_SEND_PROBE_RSP";
    case MessageType::NODE_REGISTER: return "NODE_REGISTER";
    case MessageType::NODE_PRESENCE: return "NODE_PRESENCE";
    case MessageType::NODE_HEARTBEAT: return "NODE_HEARTBEAT";
    case MessageType::HUB_CLUSTER_SNAPSHOT: return "HUB_CLUSTER_SNAPSHOT";
    case MessageType::HUB_CLUSTER_EVENT: return "HUB_CLUSTER_EVENT";
    case MessageType::ERROR_RSP: return "ERROR_RSP";
    default: return "UNKNOWN";
    }
}

MessageType messageTypeFromString(const std::string &value)
{
    if (value == "AUTH_REQ") return MessageType::AUTH_REQ;
    if (value == "AUTH_RSP") return MessageType::AUTH_RSP;
    if (value == "REGISTER_REQ") return MessageType::REGISTER_REQ;
    if (value == "REGISTER_RSP") return MessageType::REGISTER_RSP;
    if (value == "HEARTBEAT_REQ") return MessageType::HEARTBEAT_REQ;
    if (value == "HEARTBEAT_RSP") return MessageType::HEARTBEAT_RSP;
    if (value == "SESSION_CREATE_REQ") return MessageType::SESSION_CREATE_REQ;
    if (value == "SESSION_CREATE_RSP") return MessageType::SESSION_CREATE_RSP;
    if (value == "SESSION_NOTIFY") return MessageType::SESSION_NOTIFY;
    if (value == "UNREGISTER_REQ") return MessageType::UNREGISTER_REQ;
    if (value == "UNREGISTER_RSP") return MessageType::UNREGISTER_RSP;
    if (value == "NAT_PROBE_REQ") return MessageType::NAT_PROBE_REQ;
    if (value == "NAT_PROBE_RSP") return MessageType::NAT_PROBE_RSP;
    if (value == "SERVER_INFO_REQ") return MessageType::SERVER_INFO_REQ;
    if (value == "SERVER_INFO_RSP") return MessageType::SERVER_INFO_RSP;
    if (value == "FILTER_PROBE_REQ") return MessageType::FILTER_PROBE_REQ;
    if (value == "FILTER_PROBE_RSP") return MessageType::FILTER_PROBE_RSP;
    if (value == "SERVER_SEND_PROBE_REQ") return MessageType::SERVER_SEND_PROBE_REQ;
    if (value == "SERVER_SEND_PROBE_RSP") return MessageType::SERVER_SEND_PROBE_RSP;
    if (value == "NODE_REGISTER") return MessageType::NODE_REGISTER;
    if (value == "NODE_PRESENCE") return MessageType::NODE_PRESENCE;
    if (value == "NODE_HEARTBEAT") return MessageType::NODE_HEARTBEAT;
    if (value == "HUB_CLUSTER_SNAPSHOT") return MessageType::HUB_CLUSTER_SNAPSHOT;
    if (value == "HUB_CLUSTER_EVENT") return MessageType::HUB_CLUSTER_EVENT;
    if (value == "ERROR_RSP") return MessageType::ERROR_RSP;
    return MessageType::UNKNOWN;
}

std::string encodeMessage(const Message &msg)
{
    std::ostringstream os;
    os << "TYPE=" << messageTypeToString(msg.type) << "|REQ=" << msg.request_id;
    for (std::map<std::string, std::string>::const_iterator it = msg.fields.begin(); it != msg.fields.end(); ++it) {
        os << "|" << it->first << "=" << it->second;
    }
    os << "\n";
    return os.str();
}

bool decodeMessage(const std::string &line, Message *msg)
{
    if (msg == NULL) {
        return false;
    }

    msg->type = MessageType::UNKNOWN;
    msg->request_id = 0;
    msg->fields.clear();

    std::vector<std::string> pairs = split(line, '|');
    for (size_t i = 0; i < pairs.size(); ++i) {
        const std::string &kv = pairs[i];
        size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }

        std::string key = kv.substr(0, eq);
        std::string value = kv.substr(eq + 1);
        if (key == "TYPE") {
            msg->type = messageTypeFromString(value);
        } else if (key == "REQ") {
            msg->request_id = (uint64_t)strtoull(value.c_str(), NULL, 10);
        } else {
            msg->fields[key] = value;
        }
    }

    return msg->type != MessageType::UNKNOWN;
}

} // namespace ntrs
} // namespace eular
