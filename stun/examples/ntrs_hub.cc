#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <ctime>
#include <vector>

#include <mqtt_client.h>
#include <ntrs_codec.h>

struct NodeState {
    std::string node_id;
    std::string boot_id;
    std::string status;
    std::string region;
    std::string stun_endpoint;
    std::string control_endpoint;
    std::string nat_type;
    std::string last_heartbeat;
    int load;
};

struct Assignment {
    std::string primary1;
    std::string primary2;
    std::string backup1;
};

static std::string now_iso8601()
{
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

static std::string parse_node_id_from_topic(const std::string &topic)
{
    const std::string prefix = "ntrs/node/";
    const std::string suffix_register = "/register";
    const std::string suffix_presence = "/presence";
    const std::string suffix_heartbeat = "/heartbeat";
    if (topic.find(prefix) != 0) {
        return "";
    }

    size_t pos = topic.find('/', prefix.size());
    if (pos == std::string::npos) {
        return "";
    }

    std::string node_id = topic.substr(prefix.size(), pos - prefix.size());
    std::string suffix = topic.substr(pos);
    if (suffix != suffix_register && suffix != suffix_presence && suffix != suffix_heartbeat) {
        return "";
    }
    return node_id;
}

static void publish_cluster_event(eular::orion::MqttClient *mqtt,
                                  const std::string &event,
                                  const NodeState &node,
                                  uint64_t cluster_version)
{
    eular::ntrs::Message msg;
    msg.type = eular::ntrs::MessageType::HUB_CLUSTER_EVENT;
    msg.request_id = cluster_version;
    msg.fields["event"] = event;
    msg.fields["node_id"] = node.node_id;
    msg.fields["boot_id"] = node.boot_id;
    msg.fields["status"] = node.status;
    msg.fields["region"] = node.region;
    msg.fields["stun_endpoint"] = node.stun_endpoint;
    msg.fields["control_endpoint"] = node.control_endpoint;
    msg.fields["nat_type"] = node.nat_type;
    msg.fields["load"] = std::to_string(node.load);
    msg.fields["ts"] = now_iso8601();

    mqtt->publish("ntrs/hub/cluster/events", eular::ntrs::encodeMessage(msg), 1, false);
}

static void publish_cluster_snapshot(eular::orion::MqttClient *mqtt,
                                     const std::map<std::string, NodeState> &nodes,
                                     uint64_t cluster_version)
{
    std::string list;
    for (std::map<std::string, NodeState>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!list.empty()) {
            list += ";";
        }
        const NodeState &n = it->second;
        list += n.node_id + "," + n.boot_id + "," + n.status + "," +
                n.region + "," + n.stun_endpoint + "," + n.control_endpoint + "," +
                n.nat_type + "," + std::to_string(n.load) + "," + n.last_heartbeat;
    }

    eular::ntrs::Message msg;
    msg.type = eular::ntrs::MessageType::HUB_CLUSTER_SNAPSHOT;
    msg.request_id = cluster_version;
    msg.fields["cluster_version"] = std::to_string(cluster_version);
    msg.fields["node_count"] = std::to_string(nodes.size());
    msg.fields["nodes"] = list;
    msg.fields["ts"] = now_iso8601();

    mqtt->publish("ntrs/hub/cluster/snapshot", eular::ntrs::encodeMessage(msg), 1, true);
}

static Assignment build_assignment_for(const std::string &self_node,
                                       const std::map<std::string, NodeState> &nodes)
{
    Assignment a;
    std::vector<std::string> candidates;
    for (std::map<std::string, NodeState>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (it->first == self_node) {
            continue;
        }
        const NodeState &n = it->second;
        if (n.status == "online" && !n.control_endpoint.empty()) {
            candidates.push_back(n.control_endpoint);
        }
    }

    if (candidates.size() > 0) {
        a.primary1 = candidates[0];
    }
    if (candidates.size() > 1) {
        a.primary2 = candidates[1];
    }
    if (candidates.size() > 2) {
        a.backup1 = candidates[2];
    }

    return a;
}

static void publish_assignment_for_node(eular::orion::MqttClient *mqtt,
                                        const std::string &node_id,
                                        const Assignment &a,
                                        uint64_t cluster_version)
{
    eular::ntrs::Message msg;
    msg.type = eular::ntrs::MessageType::HUB_CLUSTER_EVENT;
    msg.request_id = cluster_version;
    msg.fields["event"] = "assignment";
    msg.fields["node_id"] = node_id;
    msg.fields["primary1_control"] = a.primary1;
    msg.fields["primary2_control"] = a.primary2;
    msg.fields["backup1_control"] = a.backup1;
    msg.fields["ts"] = now_iso8601();

    std::string topic = "ntrs/hub/node/" + node_id + "/assignment";
    mqtt->publish(topic, eular::ntrs::encodeMessage(msg), 1, true);
}

static void publish_all_assignments(eular::orion::MqttClient *mqtt,
                                    const std::map<std::string, NodeState> &nodes,
                                    uint64_t cluster_version)
{
    for (std::map<std::string, NodeState>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        Assignment a = build_assignment_for(it->first, nodes);
        publish_assignment_for_node(mqtt, it->first, a, cluster_version);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    std::string broker = "127.0.0.1";
    int port = 1883;
    std::string username;
    std::string password;

    if (argc > 1) {
        broker = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    if (argc > 3) {
        username = argv[3];
    }
    if (argc > 4) {
        password = argv[4];
    }

    eular::orion::MqttClient mqtt(broker, port, "ntrs-hub", username, password);
    if (!mqtt.connect()) {
        printf("hub mqtt connect failed: %s:%d\n", broker.c_str(), port);
        return 1;
    }

    std::map<std::string, NodeState> nodes;
    uint64_t cluster_version = 0;

    mqtt.setMessageCallback([&](const std::string &topic, const std::string &payload) {
        eular::ntrs::Message msg;
        if (!eular::ntrs::decodeMessage(payload, &msg)) {
            printf("hub ignore undecodable payload topic=%s\n", topic.c_str());
            return;
        }

        std::string node_id = parse_node_id_from_topic(topic);
        if (node_id.empty()) {
            return;
        }

        if (nodes.find(node_id) == nodes.end()) {
            NodeState n;
            n.node_id = node_id;
            n.status = "unknown";
            n.load = 0;
            n.last_heartbeat = now_iso8601();
            nodes[node_id] = n;
        }

        NodeState &node = nodes[node_id];
        bool changed = false;
        std::string event_name;

        if (msg.type == eular::ntrs::MessageType::NODE_REGISTER) {
            node.node_id = node_id;
            node.boot_id = msg.fields["boot_id"];
            node.region = msg.fields["region"];
            node.stun_endpoint = msg.fields["stun_endpoint"];
            node.control_endpoint = msg.fields["control_endpoint"];
            node.nat_type = msg.fields["nat_type"];
            node.last_heartbeat = now_iso8601();
            changed = true;
            event_name = "node_registered";
        } else if (msg.type == eular::ntrs::MessageType::NODE_PRESENCE) {
            std::string old_status = node.status;
            node.boot_id = msg.fields["boot_id"];
            node.status = msg.fields["status"];
            node.last_heartbeat = now_iso8601();
            changed = true;
            if (node.status == "online") {
                event_name = "node_online";
            } else if (node.status == "offline") {
                event_name = (msg.fields["reason"] == "lwt") ? "node_abnormal_offline" : "node_offline";
            } else if (node.status != old_status) {
                event_name = "node_status_changed";
            }
        } else if (msg.type == eular::ntrs::MessageType::NODE_HEARTBEAT) {
            node.status = "online";
            node.boot_id = msg.fields["boot_id"];
            node.nat_type = msg.fields["nat_type"];
            node.last_heartbeat = msg.fields["ts"];
            node.load = atoi(msg.fields["load"].c_str());
            changed = true;
            event_name = "node_heartbeat";
        }

        if (!changed) {
            return;
        }

        cluster_version++;
        publish_cluster_snapshot(&mqtt, nodes, cluster_version);
        publish_all_assignments(&mqtt, nodes, cluster_version);
        if (!event_name.empty()) {
            publish_cluster_event(&mqtt, event_name, node, cluster_version);
        }

        printf("hub updated node=%s status=%s version=%llu\n",
               node.node_id.c_str(),
               node.status.c_str(),
               (unsigned long long)cluster_version);
    });

    mqtt.subscribe("ntrs/node/+/register", 1);
    mqtt.subscribe("ntrs/node/+/presence", 1);
    mqtt.subscribe("ntrs/node/+/heartbeat", 1);

    printf("ntrs_hub running broker=%s:%d\n", broker.c_str(), port);
    while (mqtt.isConnected()) {
        mqtt.poll(50);
    }

    return 0;
}
