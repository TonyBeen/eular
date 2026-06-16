#ifndef __MQTT_CLIENT_H__
#define __MQTT_CLIENT_H__

#include <string>
#include <functional>
#include <memory>
#include <stddef.h>
#include <stdint.h>

struct event_base;
struct mosquitto;
struct mosquitto_message;

namespace ev {
class EventPoll;
class EventTimer;
}

namespace eular {
namespace orion {

class MqttClient {
private:
    struct mosquitto* mosq;
    std::string broker;
    int port;
    std::string client_id;
    std::string username;
    std::string password;
    bool connected;
    std::function<void(const std::string&, const uint8_t*, size_t)> message_callback;
    std::function<void()> connect_callback;
    std::function<void(int)> disconnect_callback;
    bool will_enabled;
    std::string will_topic;
    uint8_t *will_payload;
    size_t will_payload_len;
    int will_qos;
    bool will_retain;
    struct event_base* ev_base;
    std::unique_ptr<ev::EventPoll> ev_read;
    std::unique_ptr<ev::EventPoll> ev_write;
    std::unique_ptr<ev::EventTimer> ev_misc;
    int socket_fd;
    bool libevent_attached;

public:
    MqttClient(const std::string& broker, int port, 
               const std::string& client_id, 
               const std::string& username = "", 
               const std::string& password = "");
    ~MqttClient();

    bool connect();
    void disconnect();

    void setWillMessage(const std::string& topic,
                        const void* payload,
                        size_t payload_len,
                        int qos = 1,
                        bool retain = true);
    bool attachEventLoop(struct event_base* base);

    bool subscribe(const std::string& topic, int qos = 1);
    bool publish(const std::string& topic, const void* payload, size_t payload_len,
                 int qos = 1, bool retain = false);

    void setMessageCallback(std::function<void(const std::string&, const uint8_t*, size_t)> callback);
    void setConnectCallback(std::function<void()> callback);
    void setDisconnectCallback(std::function<void(int)> callback);
    bool isConnected() const;

private:
    void detachEventLoop();
    void refreshSocketEvents();
    void clearSocketEvents();
    void startMiscEvent();
    void stopMiscEvent();
    void onReadEvent();
    void onWriteEvent();
    void onMiscEvent();

    static void on_connect(struct mosquitto* mosq, void* obj, int rc);
    static void on_disconnect(struct mosquitto* mosq, void* obj, int rc);
    static void on_message(struct mosquitto* mosq, void* obj, const struct mosquitto_message* message);
};

} // namespace orion
} // namespace eular

#endif // __MQTT_CLIENT_H__
