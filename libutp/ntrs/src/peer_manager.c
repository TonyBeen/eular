#include "peer_manager.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>

#include "service_util.h"
#include "tls_stream.h"

typedef struct utp_ntrs_peer utp_ntrs_peer_t;

struct utp_ntrs_peer_manager {
    struct event_base*       base;            // Node 主 event loop
    struct evconnlistener*   listener;        // control_endpoint 监听器
    SSL_CTX*                 client_tls;      // 出站 TLS 配置，空指针表示明文 TCP
    SSL_CTX*                 server_tls;      // 入站 TLS 配置，空指针表示明文 TCP
    utp_ntrs_node_instance_t local;           // 本 Node 实例
    const char*              interface_name;  // 指定的 Linux 网卡
    utp_ntrs_peer_active_fn  on_active;       // 链路激活通知
    utp_ntrs_peer_failed_fn  on_failed;       // 链路失效通知
    utp_ntrs_peer_forward_fn on_forward;      // 接收 CHANGE_IP 转发请求
    void*                    user_data;       // Node 状态
    utp_ntrs_peer_t*         peers;           // 全部进行中或已建立的连接
};

struct utp_ntrs_peer {
    utp_ntrs_peer_manager_t*  manager;                                    // 所属管理器
    utp_ntrs_peer_t*          next;                                       // 连接链表
    utp_ntrs_tls_stream_t*    tls;                                        // Node 间控制流
    utp_ntrs_control_stream_t control;                                    // 控制流重组状态
    utp_ntrs_node_instance_t  expected_remote;                            // 出站连接的预期对端
    utp_ntrs_node_instance_t  remote;                                     // HELLO 校验后的对端
    struct event*             close_event;                                // 延迟关闭事件，可在 stop 时取消
    uint8_t                   initiator_node_id[UTP_NTRS_NODE_ID_SIZE];   // 原 TCP 发起方
    uint8_t                   initiator_nonce[UTP_NTRS_LINK_NONCE_SIZE];  // 本连接发起随机数
    uint8_t                   unanswered_pings;                           // 连续未响应的 PING 数
    bool                      outbound : 1;                               // 是否由本 Node 主动拨号
    bool                      hello_received : 1;                         // 是否完成 HELLO
    bool                      active : 1;                                 // 是否为 remote 的保留连接
    bool                      suppress_failure : 1;                       // 本地替换或 duplicate，不报告对端失效
    bool                      close_scheduled : 1;                        // 已进入延迟关闭队列
};

static void utp_ntrs_peer_destroy(utp_ntrs_peer_t* peer);

static bool utp_ntrs_peer_manager_has_active_remote(const utp_ntrs_peer_manager_t*  manager,
                                                    const utp_ntrs_peer_t*          excluded,
                                                    const utp_ntrs_node_instance_t* remote)
{
    const utp_ntrs_peer_t* current;

    for (current = manager->peers; current != NULL; current = current->next) {
        if (current != excluded && current->active && !current->close_scheduled &&
            utp_ntrs_node_instance_equal(&current->remote, remote)) {
            return true;
        }
    }
    return false;
}

static bool utp_ntrs_peer_message_from_payload(uint8_t type, const uint8_t* payload, uint32_t payload_length,
                                               uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE], size_t* length)
{
    *length = utp_ntrs_control_encode_header(message, UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE, type, payload_length);
    if (*length == 0u) {
        return false;
    }
    (void)memcpy(message + UTP_NTRS_CONTROL_HEADER_SIZE, payload, payload_length);
    return true;
}

static void utp_ntrs_peer_close_deferred(evutil_socket_t fd, int16_t events, void* user_data)
{
    (void)fd;
    (void)events;
    utp_ntrs_peer_destroy(user_data);
}

static void utp_ntrs_peer_schedule_close(utp_ntrs_peer_t* peer)
{
    const struct timeval timeout = {.tv_sec = 0, .tv_usec = 0};

    if (!peer->close_scheduled) {
        peer->close_scheduled = true;
        if (event_add(peer->close_event, &timeout) != 0) {
            peer->close_scheduled = false;
        }
    }
}

static bool utp_ntrs_peer_send(utp_ntrs_peer_t* peer, const uint8_t* message, size_t length)
{
    if (!utp_ntrs_tls_stream_send(peer->tls, message, length)) {
        utp_ntrs_peer_schedule_close(peer);
        return false;
    }
    return true;
}

static bool utp_ntrs_peer_send_header(utp_ntrs_peer_t* peer, uint8_t type)
{
    uint8_t message[UTP_NTRS_CONTROL_HEADER_SIZE];

    return utp_ntrs_control_encode_header(message, sizeof(message), type, 0u) != 0u &&
           utp_ntrs_peer_send(peer, message, sizeof(message));
}

static bool utp_ntrs_peer_send_hello(utp_ntrs_peer_t* peer)
{
    utp_ntrs_node_link_hello_t hello = {
        .instance          = peer->manager->local,
        .initiator_node_id = {0},
        .initiator_nonce   = {0},
    };
    uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t  length;

    (void)memcpy(hello.initiator_node_id, peer->initiator_node_id, sizeof(hello.initiator_node_id));
    (void)memcpy(hello.initiator_nonce, peer->initiator_nonce, sizeof(hello.initiator_nonce));
    length = utp_ntrs_control_encode_link_hello(message, sizeof(message), &hello);
    if (length == 0u || !utp_ntrs_peer_send(peer, message, length)) {
        return false;
    }
    return true;
}

static bool utp_ntrs_peer_matches(const utp_ntrs_peer_t* peer, const utp_ntrs_node_instance_t* remote)
{
    return (peer->hello_received && utp_ntrs_node_instance_equal(&peer->remote, remote)) ||
           (peer->outbound && utp_ntrs_node_instance_equal(&peer->expected_remote, remote));
}

static int32_t utp_ntrs_peer_key_compare(const utp_ntrs_peer_t* left, const utp_ntrs_peer_t* right)
{
    const int32_t node_id_result =
        memcmp(left->initiator_node_id, right->initiator_node_id, sizeof(left->initiator_node_id));

    return node_id_result != 0 ? node_id_result
                               : memcmp(left->initiator_nonce, right->initiator_nonce, sizeof(left->initiator_nonce));
}

static void utp_ntrs_peer_activate(utp_ntrs_peer_t* peer)
{
    utp_ntrs_peer_manager_t* const manager = peer->manager;
    utp_ntrs_peer_t*               current;

    for (current = manager->peers; current != NULL; current = current->next) {
        if (current == peer || !current->active || !utp_ntrs_node_instance_equal(&current->remote, &peer->remote)) {
            continue;
        }
        if (utp_ntrs_peer_key_compare(peer, current) >= 0) {
            peer->suppress_failure = true;
            (void)utp_ntrs_peer_send_header(peer, UTP_NTRS_CONTROL_NODE_LINK_DUPLICATE);
            (void)utp_ntrs_tls_stream_flush(peer->tls);
            utp_ntrs_peer_schedule_close(peer);
            return;
        }
        current->suppress_failure = true;
        (void)utp_ntrs_peer_send_header(current, UTP_NTRS_CONTROL_NODE_LINK_DUPLICATE);
        (void)utp_ntrs_tls_stream_flush(current->tls);
        utp_ntrs_peer_schedule_close(current);
    }
    peer->active = true;
    if (manager->on_active != NULL) {
        manager->on_active(manager->user_data, &peer->remote);
    }
}

static void utp_ntrs_peer_on_message(void* user_data, uint8_t type, const uint8_t* payload, uint32_t payload_length)
{
    utp_ntrs_peer_t* const peer = user_data;
    uint8_t                message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t                 length;

    if (!utp_ntrs_peer_message_from_payload(type, payload, payload_length, message, &length)) {
        utp_ntrs_peer_schedule_close(peer);
        return;
    }
    if (!peer->hello_received) {
        utp_ntrs_node_link_hello_t hello;

        if (type != UTP_NTRS_CONTROL_NODE_LINK_HELLO || !utp_ntrs_control_decode_link_hello(message, length, &hello) ||
            utp_ntrs_node_instance_equal(&hello.instance, &peer->manager->local) ||
            (peer->outbound && !utp_ntrs_node_instance_equal(&hello.instance, &peer->expected_remote)) ||
            (!peer->outbound &&
             memcmp(hello.initiator_node_id, hello.instance.node_id, sizeof(hello.initiator_node_id)) != 0) ||
            (peer->outbound &&
             memcmp(hello.initiator_node_id, peer->manager->local.node_id, sizeof(hello.initiator_node_id)) != 0)) {
            utp_ntrs_peer_schedule_close(peer);
            return;
        }
        peer->remote         = hello.instance;
        peer->hello_received = true;
        if (!peer->outbound) {
            (void)memcpy(peer->initiator_node_id, hello.initiator_node_id, sizeof(peer->initiator_node_id));
            (void)memcpy(peer->initiator_nonce, hello.initiator_nonce, sizeof(peer->initiator_nonce));
            if (!utp_ntrs_peer_send_hello(peer)) {
                return;
            }
        } else if (memcmp(peer->initiator_node_id, hello.initiator_node_id, sizeof(peer->initiator_node_id)) != 0 ||
                   memcmp(peer->initiator_nonce, hello.initiator_nonce, sizeof(peer->initiator_nonce)) != 0) {
            utp_ntrs_peer_schedule_close(peer);
            return;
        }
        utp_ntrs_peer_activate(peer);
        return;
    }
    if (type == UTP_NTRS_CONTROL_NODE_LINK_PING && payload_length == 0u) {
        (void)utp_ntrs_peer_send_header(peer, UTP_NTRS_CONTROL_NODE_LINK_PONG);
        return;
    }
    if (type == UTP_NTRS_CONTROL_NODE_LINK_PONG && payload_length == 0u) {
        peer->unanswered_pings = 0u;
        return;
    }
    if (type == UTP_NTRS_CONTROL_NODE_LINK_DUPLICATE && payload_length == 0u) {
        peer->suppress_failure = true;
        utp_ntrs_peer_schedule_close(peer);
        return;
    }
    if (type == UTP_NTRS_CONTROL_NAT_FORWARD_FILTER_RSP) {
        utp_ntrs_forward_filter_response_t forward;

        if (!peer->active || !utp_ntrs_control_decode_forward_filter_response(message, length, &forward) ||
            !utp_ntrs_node_instance_equal(&forward.target, &peer->manager->local) ||
            peer->manager->on_forward == NULL) {
            utp_ntrs_peer_schedule_close(peer);
            return;
        }
        peer->manager->on_forward(peer->manager->user_data, &forward);
        return;
    }
    utp_ntrs_peer_schedule_close(peer);
}

static void utp_ntrs_peer_on_plaintext(void* user_data, const uint8_t* data, size_t length)
{
    utp_ntrs_peer_t* const peer = user_data;

    if (!utp_ntrs_control_stream_feed(&peer->control, data, length, utp_ntrs_peer_on_message, peer)) {
        utp_ntrs_peer_schedule_close(peer);
    }
}

static void utp_ntrs_peer_on_closed(void* user_data) { utp_ntrs_peer_schedule_close(user_data); }

static void utp_ntrs_peer_on_ready(void* user_data)
{
    utp_ntrs_peer_t* const peer = user_data;

    if (peer->outbound && !utp_ntrs_peer_send_hello(peer)) {
        utp_ntrs_peer_schedule_close(peer);
    }
}

static void utp_ntrs_peer_destroy(utp_ntrs_peer_t* peer)
{
    utp_ntrs_peer_t**                     link          = &peer->manager->peers;
    const utp_ntrs_node_instance_t* const failed_remote = peer->active ? &peer->remote : &peer->expected_remote;

    while (*link != peer) {
        link = &(*link)->next;
    }
    *link = peer->next;
    if (!peer->suppress_failure && peer->manager->on_failed != NULL && (peer->active || peer->outbound) &&
        !utp_ntrs_peer_manager_has_active_remote(peer->manager, peer, failed_remote)) {
        peer->manager->on_failed(peer->manager->user_data, failed_remote);
    }
    utp_ntrs_tls_stream_free(peer->tls);
    event_free(peer->close_event);
    free(peer);
}

static utp_ntrs_peer_t* utp_ntrs_peer_new(utp_ntrs_peer_manager_t* manager, int32_t fd, bool outbound)
{
    utp_ntrs_peer_t* const         peer      = calloc(1u, sizeof(*peer));
    const utp_ntrs_tls_callbacks_t callbacks = {
        .ready     = utp_ntrs_peer_on_ready,
        .plaintext = utp_ntrs_peer_on_plaintext,
        .closed    = utp_ntrs_peer_on_closed,
    };

    if (peer == NULL) {
        if (fd >= 0) {
            evutil_closesocket(fd);
        }
        return NULL;
    }
    peer->manager  = manager;
    peer->outbound = outbound;
    utp_ntrs_control_stream_init(&peer->control);
    peer->tls         = utp_ntrs_tls_stream_new(manager->base, fd, outbound ? manager->client_tls : manager->server_tls,
                                                !outbound, &callbacks, peer);
    peer->close_event = event_new(manager->base, -1, EV_TIMEOUT, utp_ntrs_peer_close_deferred, peer);
    if (peer->tls == NULL || peer->close_event == NULL) {
        event_free(peer->close_event);
        utp_ntrs_tls_stream_free(peer->tls);
        free(peer);
        return NULL;
    }
    peer->next     = manager->peers;
    manager->peers = peer;
    return peer;
}

static void utp_ntrs_peer_on_accept(struct evconnlistener* listener, evutil_socket_t fd, struct sockaddr* address,
                                    int32_t address_length, void* user_data)
{
    utp_ntrs_peer_manager_t* const manager = user_data;

    (void)listener;
    (void)address;
    (void)address_length;
    (void)utp_ntrs_peer_new(manager, (int32_t)fd, false);
}

/** @brief 创建已绑定指定网卡的 Node control 监听 socket。 */
static struct evconnlistener* utp_ntrs_peer_listener_new(struct event_base* base, const utp_ntrs_endpoint_t* endpoint,
                                                         const struct sockaddr_storage* address,
                                                         socklen_t address_length, const char* interface_name,
                                                         void* user_data)
{
    const int32_t fd      = socket(endpoint->family, SOCK_STREAM, 0);
    const int32_t enabled = 1;

    if (fd < 0 || !utp_ntrs_socket_bind_interface(fd, interface_name) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        (endpoint->family == (uint8_t)AF_INET6 &&
         setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled)) != 0) ||
        bind(fd, (const struct sockaddr*)address, address_length) != 0 || evutil_make_socket_nonblocking(fd) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return NULL;
    }
    {
        struct evconnlistener* const listener = evconnlistener_new(base, utp_ntrs_peer_on_accept, user_data,
                                                                   LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, fd);

        if (listener == NULL) {
            (void)close(fd);
        }
        return listener;
    }
}

utp_ntrs_peer_manager_t* utp_ntrs_peer_manager_start(const utp_ntrs_peer_manager_options_t* options)
{
    utp_ntrs_peer_manager_t* manager = calloc(1u, sizeof(*manager));
    struct sockaddr_storage  address;
    socklen_t                address_length;

    if (manager == NULL || !utp_ntrs_endpoint_to_sockaddr(&options->listen, &address, &address_length)) {
        free(manager);
        return NULL;
    }
    manager->base           = options->base;
    manager->client_tls     = options->client_tls;
    manager->server_tls     = options->server_tls;
    manager->local          = options->local;
    manager->interface_name = options->interface_name;
    manager->on_active      = options->on_active;
    manager->on_failed      = options->on_failed;
    manager->on_forward     = options->on_forward;
    manager->user_data      = options->user_data;
    manager->listener       = utp_ntrs_peer_listener_new(manager->base, &options->listen, &address, address_length,
                                                         manager->interface_name, manager);
    if (manager->listener == NULL) {
        free(manager);
        return NULL;
    }
    return manager;
}

void utp_ntrs_peer_manager_stop(utp_ntrs_peer_manager_t* manager)
{
    if (manager != NULL) {
        while (manager->peers != NULL) {
            manager->peers->suppress_failure = true;
            (void)event_del(manager->peers->close_event);
            utp_ntrs_peer_destroy(manager->peers);
        }
        evconnlistener_free(manager->listener);
        free(manager);
    }
}

bool utp_ntrs_peer_manager_connect(utp_ntrs_peer_manager_t* manager, const utp_ntrs_node_instance_t* remote,
                                   const utp_ntrs_endpoint_t* endpoint)
{
    utp_ntrs_peer_t*        peer;
    struct sockaddr_storage address;
    socklen_t               address_length;
    int32_t                 fd;

    for (peer = manager->peers; peer != NULL; peer = peer->next) {
        if (!peer->close_scheduled && utp_ntrs_peer_matches(peer, remote)) {
            if (peer->active && manager->on_active != NULL) {
                manager->on_active(manager->user_data, &peer->remote);
            }
            return true;
        }
    }
    if (!utp_ntrs_endpoint_to_sockaddr(endpoint, &address, &address_length)) {
        return false;
    }
    fd = socket(endpoint->family, SOCK_STREAM, 0);
    if (fd < 0 || !utp_ntrs_socket_bind_interface(fd, manager->interface_name) ||
        evutil_make_socket_nonblocking(fd) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return false;
    }
    peer = utp_ntrs_peer_new(manager, fd, true);
    if (peer == NULL) {
        return false;
    }
    peer->expected_remote = *remote;
    (void)memcpy(peer->initiator_node_id, manager->local.node_id, sizeof(peer->initiator_node_id));
    if (getrandom(peer->initiator_nonce, sizeof(peer->initiator_nonce), 0u) != (ssize_t)sizeof(peer->initiator_nonce) ||
        bufferevent_socket_connect(utp_ntrs_tls_stream_transport(peer->tls), (const struct sockaddr*)&address,
                                   (int32_t)address_length) != 0) {
        utp_ntrs_peer_schedule_close(peer);
        return false;
    }
    return true;
}

bool utp_ntrs_peer_manager_send_forward(utp_ntrs_peer_manager_t* manager, const utp_ntrs_node_instance_t* target,
                                        const utp_ntrs_forward_filter_response_t* forward)
{
    utp_ntrs_peer_t* peer;
    uint8_t          message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t           length;

    for (peer = manager->peers; peer != NULL; peer = peer->next) {
        if (peer->active && !peer->close_scheduled && utp_ntrs_node_instance_equal(&peer->remote, target)) {
            length = utp_ntrs_control_encode_forward_filter_response(message, sizeof(message), forward);
            return length != 0u && utp_ntrs_peer_send(peer, message, length);
        }
    }
    return false;
}

void utp_ntrs_peer_manager_tick(utp_ntrs_peer_manager_t* manager)
{
    utp_ntrs_peer_t* peer;

    for (peer = manager->peers; peer != NULL; peer = peer->next) {
        if (!peer->active || peer->close_scheduled) {
            continue;
        }
        if (peer->unanswered_pings >= 3u) {
            utp_ntrs_peer_schedule_close(peer);
            continue;
        }
        ++peer->unanswered_pings;
        (void)utp_ntrs_peer_send_header(peer, UTP_NTRS_CONTROL_NODE_LINK_PING);
    }
}
