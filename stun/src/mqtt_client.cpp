#include "mqtt_client.h"

#include <event2/event.h>
#include <mosquitto.h>

#include <iostream>
#include <mutex>

namespace {

std::mutex g_mosquitto_init_mu;
int g_mosquitto_user_count = 0;

bool acquire_mosquitto_library() {
    std::lock_guard<std::mutex> lock(g_mosquitto_init_mu);
    if (g_mosquitto_user_count == 0) {
        const int rc = mosquitto_lib_init();
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "mosquitto_lib_init failed: " << mosquitto_strerror(rc) << std::endl;
            return false;
        }
    }
    ++g_mosquitto_user_count;
    return true;
}

void release_mosquitto_library() {
    std::lock_guard<std::mutex> lock(g_mosquitto_init_mu);
    if (g_mosquitto_user_count <= 0) {
        return;
    }
    --g_mosquitto_user_count;
    if (g_mosquitto_user_count == 0) {
        mosquitto_lib_cleanup();
    }
}

} // namespace

namespace eular {
namespace orion {

MqttClient::MqttClient(const std::string& broker, int port, 
                       const std::string& client_id, 
                       const std::string& username, 
                       const std::string& password)
    : mosq(nullptr),
      broker(broker), port(port), client_id(client_id),
      username(username), password(password), connected(false),
      message_callback(nullptr), will_enabled(false),
      will_qos(1), will_retain(true),
      ev_base(nullptr), ev_read(nullptr), ev_write(nullptr), ev_misc(nullptr),
      socket_fd(-1), libevent_attached(false) {
    if (!acquire_mosquitto_library()) {
        return;
    }

    mosq = mosquitto_new(client_id.c_str(), true, this);
    if (!mosq) {
        std::cerr << "mosquitto_new failed" << std::endl;
        release_mosquitto_library();
        return;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_message);

    if (!username.empty() && !password.empty()) {
        const int rc = mosquitto_username_pw_set(mosq, username.c_str(), password.c_str());
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "mosquitto_username_pw_set failed: " << mosquitto_strerror(rc) << std::endl;
        }
    }
}

MqttClient::~MqttClient() {
    detachEventLoop();
    disconnect();
    if (mosq) {
        mosquitto_destroy(mosq);
        mosq = nullptr;
    }
    release_mosquitto_library();
}

bool MqttClient::connect() {
    if (!mosq) {
        return false;
    }

    if (will_enabled) {
        const int will_rc = mosquitto_will_set(
            mosq,
            will_topic.c_str(),
            static_cast<int>(will_payload.size()),
            will_payload.data(),
            will_qos,
            will_retain
        );
        if (will_rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "mosquitto_will_set failed: " << mosquitto_strerror(will_rc) << std::endl;
            return false;
        }
    }

    mosquitto_reconnect_delay_set(mosq, 1, 10, true);

    const int rc = mosquitto_connect(mosq, broker.c_str(), port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to connect to broker: " << mosquitto_strerror(rc) << std::endl;
        return false;
    }

    connected = true;
    if (libevent_attached) {
        refreshSocketEvents();
        startMiscEvent();
    }
    std::cout << "Connected to MQTT broker" << std::endl;
    return true;
}

void MqttClient::disconnect() {
    if (!mosq) {
        connected = false;
        return;
    }

    if (connected) {
        const int rc = mosquitto_disconnect(mosq);
        if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
            std::cerr << "mosquitto_disconnect failed: " << mosquitto_strerror(rc) << std::endl;
        }
    }
    connected = false;
    clearSocketEvents();
}

bool MqttClient::isConnected() const {
    return connected;
}

void MqttClient::setWillMessage(const std::string& topic,
                                const std::string& payload,
                                int qos,
                                bool retain) {
    will_enabled = !topic.empty();
    will_topic = topic;
    will_payload = payload;
    will_qos = qos;
    will_retain = retain;
}

bool MqttClient::attachEventLoop(struct event_base* base) {
    if (!base || !mosq) {
        return false;
    }

    if (libevent_attached && ev_base == base) {
        refreshSocketEvents();
        startMiscEvent();
        return true;
    }

    detachEventLoop();
    ev_base = base;
    libevent_attached = true;
    refreshSocketEvents();
    startMiscEvent();
    return true;
}

void MqttClient::detachEventLoop() {
    stopMiscEvent();
    clearSocketEvents();
    ev_base = nullptr;
    libevent_attached = false;
}

void MqttClient::poll(int timeout_ms) {
    if (!mosq || libevent_attached) {
        return;
    }

    const int rc = mosquitto_loop(mosq, timeout_ms, 1);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop failed: " << mosquitto_strerror(rc) << std::endl;
    }
}

bool MqttClient::subscribe(const std::string& topic, int qos) {
    if (!mosq || !connected) {
        std::cerr << "Not connected to broker" << std::endl;
        return false;
    }

    const int rc = mosquitto_subscribe(mosq, nullptr, topic.c_str(), qos);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to subscribe: " << mosquitto_strerror(rc) << std::endl;
        return false;
    }

    std::cout << "Subscribed to topic: " << topic << std::endl;
    return true;
}

bool MqttClient::publish(const std::string& topic, const std::string& message, 
                        int qos, bool retain) {
    if (!mosq || !connected) {
        std::cerr << "Not connected to broker" << std::endl;
        return false;
    }

    const int rc = mosquitto_publish(
        mosq,
        nullptr,
        topic.c_str(),
        static_cast<int>(message.size()),
        message.data(),
        qos,
        retain
    );
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to publish: " << mosquitto_strerror(rc) << std::endl;
        return false;
    }

    return true;
}

void MqttClient::setMessageCallback(std::function<void(const std::string&, const std::string&)> callback) {
    message_callback = callback;
}

void MqttClient::refreshSocketEvents() {
    if (!libevent_attached || !ev_base || !mosq) {
        return;
    }

    const int fd = mosquitto_socket(mosq);
    if (fd < 0) {
        clearSocketEvents();
        return;
    }

    if (socket_fd != fd) {
        clearSocketEvents();
        socket_fd = fd;
    }

    if (!ev_read) {
        ev_read = event_new(ev_base, socket_fd, EV_READ | EV_PERSIST, on_read_event, this);
        if (ev_read) {
            event_add(ev_read, nullptr);
        }
    }

    if (mosquitto_want_write(mosq)) {
        if (!ev_write) {
            ev_write = event_new(ev_base, socket_fd, EV_WRITE | EV_PERSIST, on_write_event, this);
            if (ev_write) {
                event_add(ev_write, nullptr);
            }
        }
    } else if (ev_write) {
        event_free(ev_write);
        ev_write = nullptr;
    }
}

void MqttClient::clearSocketEvents() {
    if (ev_read) {
        event_free(ev_read);
        ev_read = nullptr;
    }
    if (ev_write) {
        event_free(ev_write);
        ev_write = nullptr;
    }
    socket_fd = -1;
}

void MqttClient::startMiscEvent() {
    if (!libevent_attached || !ev_base) {
        return;
    }

    if (!ev_misc) {
        ev_misc = evtimer_new(ev_base, on_misc_event, this);
    }
    if (ev_misc) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        event_add(ev_misc, &tv);
    }
}

void MqttClient::stopMiscEvent() {
    if (ev_misc) {
        event_free(ev_misc);
        ev_misc = nullptr;
    }
}

void MqttClient::on_connect(struct mosquitto* mosq, void* obj, int rc) {
    (void)mosq;
    MqttClient* client = static_cast<MqttClient*>(obj);
    if (!client) {
        return;
    }

    client->connected = (rc == MOSQ_ERR_SUCCESS);
    if (client->connected) {
        client->refreshSocketEvents();
    } else {
        std::cerr << "MQTT connect callback error: " << mosquitto_strerror(rc) << std::endl;
    }
}

void MqttClient::on_disconnect(struct mosquitto* mosq, void* obj, int rc) {
    (void)mosq;
    (void)rc;
    MqttClient* client = static_cast<MqttClient*>(obj);
    if (!client) {
        return;
    }
    client->connected = false;
    client->clearSocketEvents();
}

void MqttClient::on_message(struct mosquitto* mosq, void* obj, const struct mosquitto_message* message) {
    (void)mosq;
    MqttClient* client = static_cast<MqttClient*>(obj);
    if (!client || !message || !message->topic) {
        return;
    }

    std::string topic_str(message->topic);
    std::string payload_str;
    if (message->payload && message->payloadlen > 0) {
        payload_str.assign(static_cast<const char*>(message->payload), static_cast<size_t>(message->payloadlen));
    }

    std::cout << "Received message on topic " << topic_str << ": " << payload_str << std::endl;

    if (client->message_callback) {
        client->message_callback(topic_str, payload_str);
    }
}

void MqttClient::on_read_event(int fd, short events, void* arg) {
    (void)fd;
    (void)events;
    MqttClient* client = static_cast<MqttClient*>(arg);
    if (!client || !client->mosq) {
        return;
    }

    const int rc = mosquitto_loop_read(client->mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop_read failed: " << mosquitto_strerror(rc) << std::endl;
        client->connected = false;
    }

    client->refreshSocketEvents();
}

void MqttClient::on_write_event(int fd, short events, void* arg) {
    (void)fd;
    (void)events;
    MqttClient* client = static_cast<MqttClient*>(arg);
    if (!client || !client->mosq) {
        return;
    }

    const int rc = mosquitto_loop_write(client->mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop_write failed: " << mosquitto_strerror(rc) << std::endl;
        client->connected = false;
    }

    client->refreshSocketEvents();
}

void MqttClient::on_misc_event(int fd, short events, void* arg) {
    (void)fd;
    (void)events;
    MqttClient* client = static_cast<MqttClient*>(arg);
    if (!client || !client->mosq) {
        return;
    }

    const int rc = mosquitto_loop_misc(client->mosq);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop_misc failed: " << mosquitto_strerror(rc) << std::endl;
    }

    client->refreshSocketEvents();
    client->startMiscEvent();
}

} // namespace orion
} // namespace eular
