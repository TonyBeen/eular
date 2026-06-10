#include "mqtt_client.h"

#include <event/poll.h>
#include <event/timer.h>
#include <mosquitto.h>

#include <cstdlib>
#include <cstring>
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
      will_payload(nullptr), will_payload_len(0),
      will_qos(1), will_retain(true),
      ev_base(nullptr),
      ev_read(new ev::EventPoll()),
      ev_write(new ev::EventPoll()),
      ev_misc(new ev::EventTimer()),
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
    if (will_payload) {
        free(will_payload);
        will_payload = nullptr;
        will_payload_len = 0;
    }
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
            static_cast<int>(will_payload_len),
            will_payload,
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

    connected = false;
    if (libevent_attached) {
        refreshSocketEvents();
        startMiscEvent();
    }
    std::cout << "Connecting to MQTT broker " << broker << ":" << port << std::endl;
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

void MqttClient::setWillMessage(const std::string& topic,
                                const void* payload,
                                size_t payload_len,
                                int qos,
                                bool retain) {
    will_enabled = !topic.empty();
    will_topic = topic;
    if (will_payload) {
        free(will_payload);
        will_payload = nullptr;
        will_payload_len = 0;
    }
    if (payload_len > 0) {
        will_payload = static_cast<uint8_t *>(malloc(payload_len));
        if (will_payload && payload) {
            memcpy(will_payload, payload, payload_len);
            will_payload_len = payload_len;
        }
    }
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

bool MqttClient::publish(const std::string& topic, const void* payload, size_t payload_len,
                        int qos, bool retain) {
    if (!mosq || !connected) {
        std::cerr << "Not connected to broker" << std::endl;
        return false;
    }

    const int rc = mosquitto_publish(
        mosq,
        nullptr,
        topic.c_str(),
        static_cast<int>(payload_len),
        payload,
        qos,
        retain
    );
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "Failed to publish: " << mosquitto_strerror(rc) << std::endl;
        return false;
    }

    return true;
}

void MqttClient::setMessageCallback(std::function<void(const std::string&, const uint8_t*, size_t)> callback) {
    message_callback = callback;
}

void MqttClient::setConnectCallback(std::function<void()> callback) {
    connect_callback = callback;
}

void MqttClient::setDisconnectCallback(std::function<void(int)> callback) {
    disconnect_callback = callback;
}

bool MqttClient::isConnected() const {
    return connected;
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

    if (ev_read) {
        const int rc = ev_read->reset(
            ev_base,
            socket_fd,
            ev::EventPoll::Event::Read,
            [this](socket_t, ev::EventPoll::event_t) {
                onReadEvent();
            }
        );
        if (rc == 0) {
            ev_read->start();
        }
    }

    if (mosquitto_want_write(mosq)) {
        if (ev_write) {
            const int rc = ev_write->reset(
                ev_base,
                socket_fd,
                ev::EventPoll::Event::Write,
                [this](socket_t, ev::EventPoll::event_t) {
                    onWriteEvent();
                }
            );
            if (rc == 0) {
                ev_write->start();
            }
        }
    } else if (ev_write) {
        ev_write->stop();
        ev_write->reset();
    }
}

void MqttClient::clearSocketEvents() {
    if (ev_read) {
        ev_read->stop();
        ev_read->reset();
    }
    if (ev_write) {
        ev_write->stop();
        ev_write->reset();
    }
    socket_fd = -1;
}

void MqttClient::startMiscEvent() {
    if (!libevent_attached || !ev_base) {
        return;
    }

    if (ev_misc && ev_misc->reset(ev_base, [this]() { onMiscEvent(); })) {
        ev_misc->start(1000, 1000);
    }
}

void MqttClient::stopMiscEvent() {
    if (ev_misc) {
        ev_misc->stop();
        ev_misc->reset();
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
        std::cout << "Connected to MQTT broker" << std::endl;
        client->refreshSocketEvents();
        if (client->connect_callback) {
            client->connect_callback();
        }
    } else {
        std::cerr << "MQTT connect callback error: " << mosquitto_strerror(rc) << std::endl;
    }
}

void MqttClient::on_disconnect(struct mosquitto* mosq, void* obj, int rc) {
    (void)mosq;
    MqttClient* client = static_cast<MqttClient*>(obj);
    if (!client) {
        return;
    }
    client->connected = false;
    client->clearSocketEvents();
    if (client->disconnect_callback) {
        client->disconnect_callback(rc);
    }
}

void MqttClient::on_message(struct mosquitto* mosq, void* obj, const struct mosquitto_message* message) {
    (void)mosq;
    MqttClient* client = static_cast<MqttClient*>(obj);
    if (!client || !message || !message->topic) {
        return;
    }

    std::string topic_str(message->topic);
    if (client->message_callback) {
        client->message_callback(
            topic_str,
            static_cast<const uint8_t *>(message->payload),
            message->payloadlen > 0 ? static_cast<size_t>(message->payloadlen) : 0u
        );
    }
}

void MqttClient::onReadEvent() {
    if (!mosq) {
        return;
    }

    const int rc = mosquitto_loop_read(mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop_read failed: " << mosquitto_strerror(rc) << std::endl;
        connected = false;
    }

    refreshSocketEvents();
}

void MqttClient::onWriteEvent() {
    if (!mosq) {
        return;
    }

    const int rc = mosquitto_loop_write(mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop_write failed: " << mosquitto_strerror(rc) << std::endl;
        connected = false;
    }

    refreshSocketEvents();
}

void MqttClient::onMiscEvent() {
    if (!mosq) {
        return;
    }

    const int rc = mosquitto_loop_misc(mosq);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        std::cerr << "mosquitto_loop_misc failed: " << mosquitto_strerror(rc) << std::endl;
    }

    refreshSocketEvents();
}

} // namespace orion
} // namespace eular
