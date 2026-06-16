#include <mqtt_client.h>
#include <ntrs_auth.h>
#include <ntrs_codec.h>
#include <ntrs_hub_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <event/loop.h>
#include <event/timer.h>

struct Assignment {
    std::string primary1;
    std::string primary2;
    std::string backup1;
};

static void print_usage(const char* program)
{
    printf(
        "Usage: %s [mqtt_host] [mqtt_port] [mqtt_username] [mqtt_password]\n"
        "       %s [--host mqtt_host] [--port mqtt_port] [--username mqtt_username] [--password mqtt_password]\n"
        "\n"
        "Options:\n"
        "  -h, --help              Show this help message.\n"
        "  --host                  MQTT broker host or domain. Default: 127.0.0.1\n"
        "  --port                  MQTT broker port. Default: 1883\n"
        "  --username              MQTT username.\n"
        "  --password              MQTT password.\n"
        "\n"
        "Arguments:\n"
        "  mqtt_host               MQTT broker host or domain. Default: 127.0.0.1\n"
        "  mqtt_port               MQTT broker port. Default: 1883\n"
        "  mqtt_username           Optional MQTT username.\n"
        "  mqtt_password           Optional MQTT password.\n"
        "\n"
        "Example:\n"
        "  %s --host bd.eular.top --username ntrs --password '<password>'\n",
        program, program, program);
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

static std::string parse_node_id_from_topic(const std::string& topic)
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

static bool mqtt_publish_message(eular::orion::MqttClient* mqtt, const std::string& topic,
                                 const eular::ntrs::Message& msg, bool retain)
{
    uint8_t buf[8192];
    size_t  len = 0;
    if (mqtt == NULL || eular::ntrs::encodeMessage(msg, buf, sizeof(buf), &len) != 0) {
        printf("hub mqtt publish encode failed topic=%s retain=%s\n", topic.c_str(), retain ? "true" : "false");
        return false;
    }
    if (!mqtt->publish(topic, buf, len, 1, retain)) {
        printf("hub mqtt publish failed topic=%s len=%zu retain=%s\n", topic.c_str(), len, retain ? "true" : "false");
        return false;
    }
    printf("hub mqtt publish ok topic=%s len=%zu retain=%s\n", topic.c_str(), len, retain ? "true" : "false");
    return true;
}

static void publish_cluster_event(eular::orion::MqttClient* mqtt, const std::string& event,
                                  const eular::ntrs::ClusterNodeState& node, uint64_t cluster_version)
{
    eular::ntrs::Message msg;
    eular::ntrs::EventCode event_code = eular::ntrs::EventCode::UNKNOWN;
    eular::ntrs::NodeStatusCode status_code = eular::ntrs::NodeStatusCode::UNKNOWN;
    if (event == "node_registered") {
        event_code = eular::ntrs::EventCode::NODE_REGISTERED;
    } else if (event == "node_generation_replaced") {
        event_code = eular::ntrs::EventCode::NODE_GENERATION_REPLACED;
    } else if (event == "node_online") {
        event_code = eular::ntrs::EventCode::NODE_ONLINE;
    } else if (event == "node_abnormal_offline") {
        event_code = eular::ntrs::EventCode::NODE_ABNORMAL_OFFLINE;
    } else if (event == "node_offline") {
        event_code = eular::ntrs::EventCode::NODE_OFFLINE;
    } else if (event == "node_status_changed") {
        event_code = eular::ntrs::EventCode::NODE_STATUS_CHANGED;
    } else if (event == "node_heartbeat") {
        event_code = eular::ntrs::EventCode::NODE_HEARTBEAT;
    } else if (event == "node_evicted") {
        event_code = eular::ntrs::EventCode::NODE_EVICTED;
    }
    if (node.status == "registered") {
        status_code = eular::ntrs::NodeStatusCode::REGISTERED;
    } else if (node.status == "online") {
        status_code = eular::ntrs::NodeStatusCode::ONLINE;
    } else if (node.status == "offline") {
        status_code = eular::ntrs::NodeStatusCode::OFFLINE;
    }
    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::HUB_CLUSTER_EVENT, (uint32_t)cluster_version);
    eular::ntrs::messageAddU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, (uint8_t)event_code);
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node.node_id.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::BOOT_ID, node.boot_id.c_str());
    eular::ntrs::messageAddU8ByTag(&msg, eular::ntrs::FieldTag::STATUS, (uint8_t)status_code);
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::REGION, node.region.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::PROBE_ENDPOINT, node.probe_endpoint.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::CONTROL_ENDPOINT, node.control_endpoint.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::NAT_TYPE, node.nat_type.c_str());
    eular::ntrs::messageAddU32ByTag(&msg, eular::ntrs::FieldTag::LOAD, (uint32_t)node.load);
    eular::ntrs::messageAddU32ByTag(&msg, eular::ntrs::FieldTag::HEARTBEAT_INTERVAL_SEC, node.heartbeat_interval_sec);
    const std::string ts = now_iso8601();
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());

    mqtt_publish_message(mqtt, "ntrs/hub/cluster/events", msg, false);
}

static void publish_cluster_snapshot(eular::orion::MqttClient*                                   mqtt,
                                     const std::map<std::string, eular::ntrs::ClusterNodeState>& nodes,
                                     uint64_t                                                    cluster_version)
{
    eular::ntrs::Message msg;
    const std::string    ts = now_iso8601();
    if (!eular::ntrs::buildClusterSnapshotMessage(nodes, cluster_version, ts, &msg)) {
        return;
    }

    mqtt_publish_message(mqtt, "ntrs/hub/cluster/snapshot", msg, true);
}

static Assignment build_assignment_for(const std::string&                                          self_node,
                                       const std::map<std::string, eular::ntrs::ClusterNodeState>& nodes)
{
    Assignment               a;
    std::vector<std::string> candidates;
    for (std::map<std::string, eular::ntrs::ClusterNodeState>::const_iterator it = nodes.begin(); it != nodes.end();
         ++it) {
        if (it->first == self_node) {
            continue;
        }
        const eular::ntrs::ClusterNodeState& n = it->second;
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

static void publish_assignment_for_node(eular::orion::MqttClient* mqtt, const std::string& node_id, const Assignment& a,
                                        uint64_t cluster_version)
{
    eular::ntrs::Message msg;
    eular::ntrs::messageInit(&msg, eular::ntrs::MessageType::HUB_CLUSTER_EVENT, (uint32_t)cluster_version);
    eular::ntrs::messageAddU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, (uint8_t)eular::ntrs::EventCode::ASSIGNMENT);
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::NODE_ID, node_id.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::PRIMARY1_CONTROL, a.primary1.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::PRIMARY2_CONTROL, a.primary2.c_str());
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::BACKUP1_CONTROL, a.backup1.c_str());
    const std::string ts = now_iso8601();
    eular::ntrs::messageAddStringByTag(&msg, eular::ntrs::FieldTag::TS, ts.c_str());

    std::string topic = "ntrs/hub/node/" + node_id + "/assignment";
    printf("hub publish assignment node=%s v=%llu p1=%s p2=%s b1=%s\n", node_id.c_str(),
           (unsigned long long)cluster_version, a.primary1.c_str(), a.primary2.c_str(), a.backup1.c_str());
    mqtt_publish_message(mqtt, topic, msg, true);
}

static void publish_all_assignments(eular::orion::MqttClient*                                   mqtt,
                                    const std::map<std::string, eular::ntrs::ClusterNodeState>& nodes,
                                    uint64_t                                                    cluster_version)
{
    for (std::map<std::string, eular::ntrs::ClusterNodeState>::const_iterator it = nodes.begin(); it != nodes.end();
         ++it) {
        Assignment a = build_assignment_for(it->first, nodes);
        publish_assignment_for_node(mqtt, it->first, a, cluster_version);
    }
}

static void publish_cluster_update(eular::orion::MqttClient* mqtt, const eular::ntrs::HubClusterState& cluster_state,
                                   const std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> >& events)
{
    publish_cluster_snapshot(mqtt, cluster_state.nodes(), cluster_state.cluster_version());
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].first.empty()) {
            continue;
        }
        publish_cluster_event(mqtt, events[i].first, events[i].second, cluster_state.cluster_version());
    }
    publish_all_assignments(mqtt, cluster_state.nodes(), cluster_state.cluster_version());
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    std::string broker = "127.0.0.1";
    int         port = 1883;
    std::string username;
    std::string password;
    bool        use_named_args = false;
    int         positional_index = 0;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strncmp(arg, "--", 2) == 0) {
            use_named_args = true;
            if (value == NULL) {
                print_usage(argv[0]);
                return 1;
            }
            if (strcmp(arg, "--host") == 0) {
                broker = value;
            } else if (strcmp(arg, "--port") == 0) {
                port = atoi(value);
                if (port <= 0 || port > 65535) {
                    printf("invalid mqtt_port: %s\n", value);
                    print_usage(argv[0]);
                    return 1;
                }
            } else if (strcmp(arg, "--username") == 0) {
                username = value;
            } else if (strcmp(arg, "--password") == 0) {
                password = value;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            ++i;
            continue;
        }

        if (use_named_args) {
            print_usage(argv[0]);
            return 1;
        }
        if (positional_index == 0) {
            broker = arg;
        } else if (positional_index == 1) {
            port = atoi(arg);
            if (port <= 0 || port > 65535) {
                printf("invalid mqtt_port: %s\n", arg);
                print_usage(argv[0]);
                return 1;
            }
        } else if (positional_index == 2) {
            username = arg;
        } else if (positional_index == 3) {
            password = arg;
        } else {
            print_usage(argv[0]);
            return 1;
        }
        ++positional_index;
    }

    eular::orion::MqttClient mqtt(broker, port, "ntrs-hub", username, password);
    ev::EventLoop            loop;
    if (!mqtt.attachEventLoop(loop.loop())) {
        printf("hub attach event loop failed\n");
        return 1;
    }

    eular::ntrs::HubClusterState cluster_state;
    bool                         mqtt_bootstrap_done = false;

    auto ensure_hub_subscriptions = [&]() {
        if (!mqtt.isConnected()) {
            return;
        }
        mqtt.subscribe("ntrs/node/+/register", 1);
        mqtt.subscribe("ntrs/node/+/presence", 1);
        mqtt.subscribe("ntrs/node/+/heartbeat", 1);
        mqtt.subscribe("ntrs/hub/cluster/snapshot", 1);
        if (!mqtt_bootstrap_done) {
            mqtt_bootstrap_done = true;
            printf("hub mqtt subscriptions ready\n");
        } else {
            printf("hub mqtt subscriptions refreshed\n");
        }
    };

    mqtt.setMessageCallback([&](const std::string& topic, const uint8_t* payload, size_t payload_len) {
        eular::ntrs::Message msg;
        printf("hub mqtt message topic=%s len=%zu\n", topic.c_str(), payload_len);
        if (!eular::ntrs::decodeMessage(payload, payload_len, &msg)) {
            printf("hub ignore undecodable payload topic=%s\n", topic.c_str());
            return;
        }
        uint8_t event_code = (uint8_t)eular::ntrs::EventCode::UNKNOWN;
        eular::ntrs::messageGetU8ByTag(&msg, eular::ntrs::FieldTag::EVENT, &event_code);
        printf("hub mqtt decoded topic=%s type=%u node_id=%s event=%s request_id=%u\n", topic.c_str(),
               (unsigned)msg.type, eular::ntrs::messageGetString(&msg, "node_id"),
               eular::ntrs::event_code_name((eular::ntrs::EventCode)event_code), msg.request_id);

        if (topic == "ntrs/hub/cluster/snapshot") {
            if (cluster_state.restoreFromSnapshot(msg)) {
                printf("hub restored snapshot version=%llu nodes=%zu\n",
                       (unsigned long long)cluster_state.cluster_version(), cluster_state.nodes().size());
            }
            return;
        }

        std::string node_id = parse_node_id_from_topic(topic);
        if (node_id.empty()) {
            return;
        }

        std::string                   event_name;
        eular::ntrs::ClusterNodeState node;
        uint64_t                      now_sec = (uint64_t)time(NULL);
        if (!cluster_state.applyMessage(node_id, msg, now_sec, now_iso8601(), &event_name, &node)) {
            return;
        }

        std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> > events;
        events.push_back(std::make_pair(event_name, node));
        publish_cluster_update(&mqtt, cluster_state, events);

        printf("hub updated node=%s status=%s version=%llu\n", node.node_id.c_str(), node.status.c_str(),
               (unsigned long long)cluster_state.cluster_version());
    });
    mqtt.setConnectCallback([&]() { ensure_hub_subscriptions(); });
    mqtt.setDisconnectCallback([&](int rc) {
        printf("hub mqtt disconnected rc=%d\n", rc);
    });
    if (!mqtt.connect()) {
        printf("hub mqtt connect failed: %s:%d\n", broker.c_str(), port);
        return 1;
    }

    ev::EventTimer sweep_timer;
    if (!sweep_timer.reset(loop.loop(),
                           [&]() {
                               std::vector<eular::ntrs::ClusterNodeState> evicted_nodes;
                               uint64_t                                   now_sec = (uint64_t)time(NULL);
                               if (!cluster_state.sweepExpired(now_sec, &evicted_nodes)) {
                                   return;
                               }

                               std::vector<std::pair<std::string, eular::ntrs::ClusterNodeState> > events;
                               for (size_t i = 0; i < evicted_nodes.size(); ++i) {
                                   events.push_back(std::make_pair("node_evicted", evicted_nodes[i]));
                               }
                               publish_cluster_update(&mqtt, cluster_state, events);
                           }) ||
        !sweep_timer.start(1, 1000)) {
        printf("hub setup sweep timer failed\n");
        return 1;
    }

    printf("ntrs_hub running broker=%s:%d\n", broker.c_str(), port);
    loop.dispatch();

    return 0;
}
