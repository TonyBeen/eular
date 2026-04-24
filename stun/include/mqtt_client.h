#ifndef __MQTT_CLIENT_H__
#define __MQTT_CLIENT_H__

#include <string>
#include <functional>

struct event_base;
struct event;
struct mosquitto;
struct mosquitto_message;

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
    std::function<void(const std::string&, const std::string&)> message_callback;
    bool will_enabled;
    std::string will_topic;
    std::string will_payload;
    int will_qos;
    bool will_retain;
    struct event_base* ev_base;
    struct event* ev_read;
    struct event* ev_write;
    struct event* ev_misc;
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
    bool isConnected() const;

    void setWillMessage(const std::string& topic,
                        const std::string& payload,
                        int qos = 1,
                        bool retain = true);
    bool attachEventLoop(struct event_base* base);
    void detachEventLoop();
    void poll(int timeout_ms = 50);

    bool subscribe(const std::string& topic, int qos = 1);
    bool publish(const std::string& topic, const std::string& message, 
                 int qos = 1, bool retain = false);

    void setMessageCallback(std::function<void(const std::string&, const std::string&)> callback);

private:
    void refreshSocketEvents();
    void clearSocketEvents();
    void startMiscEvent();
    void stopMiscEvent();

    static void on_connect(struct mosquitto* mosq, void* obj, int rc);
    static void on_disconnect(struct mosquitto* mosq, void* obj, int rc);
    static void on_message(struct mosquitto* mosq, void* obj, const struct mosquitto_message* message);

    static void on_read_event(int fd, short events, void* arg);
    static void on_write_event(int fd, short events, void* arg);
    static void on_misc_event(int fd, short events, void* arg);
};

} // namespace orion
} // namespace eular

#endif // __MQTT_CLIENT_H__
