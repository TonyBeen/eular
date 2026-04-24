#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include <string>

#include <vector>

#include <mqtt_client.h>
#include <ntrs_codec.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

static std::string now_iso8601()
{
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

static std::string topic_for(const std::string &node_id, const std::string &suffix)
{
    return std::string("ntrs/node/") + node_id + "/" + suffix;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 7) {
        printf("Usage: %s <broker> <port> <node_id> <region> <stun_host:port> <control_host:port> [nat_type] [username] [password]\n", argv[0]);
        return 1;
    }

    std::string broker = argv[1];
    int port = atoi(argv[2]);
    std::string node_id = argv[3];
    std::string region = argv[4];
    std::string stun_endpoint = argv[5];
    std::string control_endpoint = argv[6];
    std::string nat_type = (argc > 7) ? argv[7] : "unknown";
    std::string username = (argc > 8) ? argv[8] : "";
    std::string password = (argc > 9) ? argv[9] : "";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    std::string boot_id = std::to_string((unsigned long long)time(NULL)) + "-" + std::to_string((unsigned long long)getpid());
    std::string client_id = "ntrs-node-" + node_id + "-" + std::to_string((unsigned long long)getpid());

    eular::orion::MqttClient mqtt(broker, port, client_id, username, password);
    std::string assignment_primary1;
    std::string assignment_primary2;
    std::string assignment_backup1;
    std::string assignment_version = "0";

    eular::ntrs::Message will;
    will.type = eular::ntrs::MessageType::NODE_PRESENCE;
    will.request_id = 0;
    will.fields["node_id"] = node_id;
    will.fields["boot_id"] = boot_id;
    will.fields["status"] = "offline";
    will.fields["reason"] = "lwt";
    will.fields["ts"] = now_iso8601();
    mqtt.setWillMessage(topic_for(node_id, "presence"), eular::ntrs::encodeMessage(will), 1, true);

    if (!mqtt.connect()) {
        printf("node mqtt connect failed: %s:%d\n", broker.c_str(), port);
        return 1;
    }

    mqtt.setMessageCallback([&](const std::string &topic, const std::string &payload) {
        eular::ntrs::Message msg;
        if (!eular::ntrs::decodeMessage(payload, &msg)) {
            return;
        }

        if (topic == "ntrs/hub/cluster/snapshot" && msg.type == eular::ntrs::MessageType::HUB_CLUSTER_SNAPSHOT) {
            printf("[snapshot] version=%s count=%s\n",
                   msg.fields["cluster_version"].c_str(),
                   msg.fields["node_count"].c_str());
        } else if (topic == "ntrs/hub/cluster/events" && msg.type == eular::ntrs::MessageType::HUB_CLUSTER_EVENT) {
            printf("[event] %s node=%s status=%s\n",
                   msg.fields["event"].c_str(),
                   msg.fields["node_id"].c_str(),
                   msg.fields["status"].c_str());
        } else if (topic == topic_for(node_id, "assignment") &&
                   msg.type == eular::ntrs::MessageType::HUB_CLUSTER_EVENT &&
                   msg.fields["event"] == "assignment") {
            assignment_primary1 = msg.fields["primary1_control"];
            assignment_primary2 = msg.fields["primary2_control"];
            assignment_backup1 = msg.fields["backup1_control"];
            assignment_version = std::to_string((unsigned long long)msg.request_id);

            printf("[assignment] v=%s p1=%s p2=%s b1=%s\n",
                   assignment_version.c_str(),
                   assignment_primary1.c_str(),
                   assignment_primary2.c_str(),
                   assignment_backup1.c_str());
        }
    });

    mqtt.subscribe("ntrs/hub/cluster/snapshot", 1);
    mqtt.subscribe("ntrs/hub/cluster/events", 1);
    mqtt.subscribe(topic_for(node_id, "assignment"), 1);

    eular::ntrs::Message reg;
    reg.type = eular::ntrs::MessageType::NODE_REGISTER;
    reg.request_id = 1;
    reg.fields["node_id"] = node_id;
    reg.fields["boot_id"] = boot_id;
    reg.fields["region"] = region;
    reg.fields["stun_endpoint"] = stun_endpoint;
    reg.fields["control_endpoint"] = control_endpoint;
    reg.fields["nat_type"] = nat_type;
    reg.fields["ts"] = now_iso8601();
    mqtt.publish(topic_for(node_id, "register"), eular::ntrs::encodeMessage(reg), 1, true);

    eular::ntrs::Message online;
    online.type = eular::ntrs::MessageType::NODE_PRESENCE;
    online.request_id = 2;
    online.fields["node_id"] = node_id;
    online.fields["boot_id"] = boot_id;
    online.fields["status"] = "online";
    online.fields["reason"] = "startup";
    online.fields["ts"] = now_iso8601();
    mqtt.publish(topic_for(node_id, "presence"), eular::ntrs::encodeMessage(online), 1, true);

    printf("ntrs_node online node_id=%s boot_id=%s\n", node_id.c_str(), boot_id.c_str());

    int beat = 0;
    while (g_running && mqtt.isConnected()) {
        eular::ntrs::Message hb;
        hb.type = eular::ntrs::MessageType::NODE_HEARTBEAT;
        hb.request_id = 100 + (uint64_t)beat;
        hb.fields["node_id"] = node_id;
        hb.fields["boot_id"] = boot_id;
        hb.fields["status"] = "online";
        hb.fields["load"] = std::to_string(beat % 100);
        hb.fields["nat_type"] = nat_type;
        hb.fields["assignment_version"] = assignment_version;
        hb.fields["assignment_primary1"] = assignment_primary1;
        hb.fields["assignment_primary2"] = assignment_primary2;
        hb.fields["assignment_backup1"] = assignment_backup1;
        hb.fields["ts"] = now_iso8601();
        mqtt.publish(topic_for(node_id, "heartbeat"), eular::ntrs::encodeMessage(hb), 1, false);

        for (int i = 0; i < 50 && g_running; ++i) {
            mqtt.poll(20);
        }
        beat++;
    }

    eular::ntrs::Message offline;
    offline.type = eular::ntrs::MessageType::NODE_PRESENCE;
    offline.request_id = 3;
    offline.fields["node_id"] = node_id;
    offline.fields["boot_id"] = boot_id;
    offline.fields["status"] = "offline";
    offline.fields["reason"] = "graceful_shutdown";
    offline.fields["ts"] = now_iso8601();
    mqtt.publish(topic_for(node_id, "presence"), eular::ntrs::encodeMessage(offline), 1, true);

    mqtt.disconnect();
    printf("ntrs_node offline node_id=%s\n", node_id.c_str());
    return 0;
}
