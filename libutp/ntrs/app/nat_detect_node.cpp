#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include <arpa/inet.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>
#include <ntrs/service.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <utils/CLI11.hpp>

#include "app_log.h"
#include "peer_manager.h"
#include "service_util.h"
#include "tls_stream.h"

#define fprintf(stream, ...) UTP_NTRS_APP_LOG(stream, __VA_ARGS__)

#define UTP_NTRS_FORWARD_DEDUP_CAPACITY    1024u
#define UTP_NTRS_FORWARD_DEDUP_LIFETIME_MS 30000u

typedef struct nat_detect_node_forward_dedup {
    uint64_t forward_id;     // 已处理的转发标识
    uint64_t expires_at_ms;  // 去重记录失效时刻
} nat_detect_node_forward_dedup_t;

typedef struct nat_detect_node {
    struct event_base*              base;                     // Node 主 libevent loop
    struct event*                   reconnect_event;          // Hub 断线重连定时器
    struct event*                   heartbeat_event;          // Hub 心跳定时器
    struct event*                   peer_tick_event;          // Node 间链路保活定时器
    SSL_CTX*                        hub_tls_context;          // Hub 客户端 TLS 上下文，空指针表示明文 TCP
    SSL_CTX*                        peer_server_tls_context;  // Node 入站 TLS 上下文，空指针表示明文 TCP
    utp_ntrs_tls_stream_t*          hub_stream;               // 当前 Hub 控制连接
    utp_ntrs_control_stream_t       control;                  // Hub 控制消息重组状态
    utp_ntrs_node_registration_t    registration;             // 本 Node 的固定注册信息
    utp_ntrs_endpoint_t             hub_endpoint;             // Hub TCP endpoint
    const char*                     interface_name;           // 指定的 Linux 出口网卡
    const char*                     node_name;                // 操作员可读的 Node 名称
    utp_ntrs_udp_server_t*          udp_server;               // UDP NAT 探测 worker
    utp_ntrs_peer_manager_t*        peers;                    // Node 间控制链路管理器
    utp_ntrs_assignment_t           assignments[2];           // IPv4、IPv6 当前 assignment
    nat_detect_node_forward_dedup_t forward_dedup[UTP_NTRS_FORWARD_DEDUP_CAPACITY];  // 协同回包短期去重表
    uint64_t                        assignment_versions[2];  // IPv4、IPv6 已接受 assignment 版本
    bool                            registered : 1;          // Hub 已接受注册
    bool                            close_scheduled : 1;
} nat_detect_node_t;

typedef struct nat_detect_node_options {
    const char* hub;             // Hub TCP endpoint
    const char* node_id;         // 稳定 Node 名称
    const char* probe;           // UDP probe 监听 endpoint
    const char* change_port;     // UDP 备用端口监听 endpoint
    const char* control;         // Node 间控制监听 endpoint
    const char* interface_name;  // Linux 网卡绑定
    const char* certificate;     // 可选 TLS 证书
    const char* private_key;     // 可选 TLS 私钥
    const char* boot_id;         // 可选实例启动标识
    uint32_t    load;            // 当前负载
    uint32_t    heartbeat_ms;    // Hub 心跳周期
    uint32_t    workers;         // UDP worker 数
    uint32_t    source_rate;     // 单源请求限速
    uint32_t    source_burst;    // 单源突发上限
    bool        use_ipv6;        // 是否强制 IPv6 服务实例
} nat_detect_node_options_t;

static void        nat_detect_node_disconnect(nat_detect_node_t* node);
static void        nat_detect_node_schedule_disconnect(nat_detect_node_t* node);
static bool        nat_detect_node_send(nat_detect_node_t* node, const uint8_t* message, size_t length);
static void        nat_detect_node_on_udp_forward(void* user_data, const utp_ntrs_forward_filter_response_t* forward);
static void        nat_detect_node_on_peer_forward(void* user_data, const utp_ntrs_forward_filter_response_t* forward);

static const char* nat_detect_node_instance(const utp_ntrs_node_instance_t* instance,
                                            char                            text[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE])
{
    return utp_ntrs_node_instance_format(instance, text, UTP_NTRS_NODE_INSTANCE_TEXT_SIZE);
}

static uint8_t nat_detect_node_family_slot(uint8_t family)
{
    return family == (uint8_t)AF_INET ? (uint8_t)0u : (uint8_t)1u;
}

static bool nat_detect_node_send_assignment_request(nat_detect_node_t* node, uint8_t slot, uint8_t failed_roles)
{
    const utp_ntrs_assignment_t* const assignment = &node->assignments[slot];
    utp_ntrs_assignment_request_t      request    = {};
    uint8_t                            message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t                             length;

    if (!node->registered || assignment->version == 0u || failed_roles == 0u) {
        return false;
    }
    request.instance           = node->registration.instance;
    request.assignment_version = assignment->version;
    request.family             = assignment->family;
    request.failed_roles       = failed_roles;
    if ((failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) != 0u) {
        request.failed_primary = assignment->primary;
    }
    if ((failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) != 0u) {
        request.failed_backup = assignment->backup;
    }
    length = utp_ntrs_control_encode_assignment_request(message, sizeof(message), &request);
    if (length == 0u || !nat_detect_node_send(node, message, length)) {
        return false;
    }
    {
        char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr,
                      "nat_detect_node event=assignment_replacement_requested "
                      "node=%s family=%u version=%llu roles=%u\n",
                      nat_detect_node_instance(&node->registration.instance, local), (uint32_t)assignment->family,
                      (unsigned long long)assignment->version, (uint32_t)failed_roles);
    }
    return true;
}

static void nat_detect_node_request_replacement(nat_detect_node_t* node, const utp_ntrs_node_instance_t* remote)
{
    uint8_t slot;

    for (slot = 0u; slot < 2u; ++slot) {
        const utp_ntrs_assignment_t* const assignment   = &node->assignments[slot];
        uint8_t                            failed_roles = 0u;

        if (assignment->has_primary && utp_ntrs_node_instance_equal(&assignment->primary, remote)) {
            failed_roles |= UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY;
        }
        if (assignment->has_backup && utp_ntrs_node_instance_equal(&assignment->backup, remote)) {
            failed_roles |= UTP_NTRS_ASSIGNMENT_ROLE_BACKUP;
        }
        if (failed_roles != 0u && !nat_detect_node_send_assignment_request(node, slot, failed_roles)) {
            nat_detect_node_schedule_disconnect(node);
        }
    }
}

static void nat_detect_node_on_peer_failed(void* user_data, const utp_ntrs_node_instance_t* remote)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);
    uint8_t                  slot;

    {
        char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
        char peer[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_node event=peer_link_failed node=%s peer=%s\n",
                      nat_detect_node_instance(&node->registration.instance, local),
                      nat_detect_node_instance(remote, peer));
    }
    for (slot = 0u; slot < 2u; ++slot) {
        const utp_ntrs_assignment_t* const assignment = &node->assignments[slot];

        if (assignment->has_primary && utp_ntrs_node_instance_equal(&assignment->primary, remote)) {
            utp_ntrs_udp_server_set_primary(node->udp_server, NULL, NULL);
            (void)fprintf(stderr, "nat_detect_node event=alternate_probe_disabled family=%u\n",
                          (uint32_t)assignment->family);
            break;
        }
    }
    nat_detect_node_request_replacement(node, remote);
}

static void nat_detect_node_on_peer_active(void* user_data, const utp_ntrs_node_instance_t* remote)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);
    uint8_t                  slot;

    for (slot = 0u; slot < 2u; ++slot) {
        const utp_ntrs_assignment_t* const assignment = &node->assignments[slot];

        if (assignment->has_primary && utp_ntrs_node_instance_equal(&assignment->primary, remote)) {
            char endpoint[64];
            char peer[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

            utp_ntrs_udp_server_set_primary(node->udp_server, remote, &assignment->primary_probe);
            (void)fprintf(stderr,
                          "nat_detect_node event=primary_link_active peer=%s "
                          "family=%u alternate_probe=%s\n",
                          nat_detect_node_instance(remote, peer), (uint32_t)assignment->family,
                          utp_ntrs_endpoint_format(&assignment->primary_probe, endpoint, sizeof(endpoint)));
            return;
        }
    }
}

static bool nat_detect_node_forward_seen(nat_detect_node_t* node, uint64_t forward_id)
{
    const uint64_t                   now_ms = utp_ntrs_now_ms();
    nat_detect_node_forward_dedup_t* slot   = NULL;
    uint32_t                         index;

    for (index = 0u; index < UTP_NTRS_FORWARD_DEDUP_CAPACITY; ++index) {
        nat_detect_node_forward_dedup_t* const current = &node->forward_dedup[index];

        if (current->expires_at_ms > now_ms && current->forward_id == forward_id) {
            return true;
        }
        if (current->expires_at_ms <= now_ms) {
            slot = current;
            break;
        }
        if (slot == NULL || current->expires_at_ms < slot->expires_at_ms) {
            slot = current;
        }
    }
    *slot = nat_detect_node_forward_dedup_t{
        .forward_id    = forward_id,
        .expires_at_ms = now_ms + UTP_NTRS_FORWARD_DEDUP_LIFETIME_MS,
    };
    return false;
}

static void nat_detect_node_on_udp_forward(void* user_data, const utp_ntrs_forward_filter_response_t* forward)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);

    if (!utp_ntrs_peer_manager_send_forward(node->peers, &forward->target, forward)) {
        char target[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr,
                      "nat_detect_node event=filter_forward_dropped target=%s "
                      "packet_number=%llu reason=peer_unavailable\n",
                      nat_detect_node_instance(&forward->target, target), (unsigned long long)forward->packet_number);
        return;
    }
    {
        char target[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_node event=filter_forwarded target=%s packet_number=%llu\n",
                      nat_detect_node_instance(&forward->target, target), (unsigned long long)forward->packet_number);
    }
}

static void nat_detect_node_on_peer_forward(void* user_data, const utp_ntrs_forward_filter_response_t* forward)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);

    if (nat_detect_node_forward_seen(node, forward->forward_id)) {
        (void)fprintf(stderr, "nat_detect_node event=filter_forward_duplicate packet_number=%llu\n",
                      (unsigned long long)forward->packet_number);
        return;
    }
    if (!utp_ntrs_udp_server_send_filter_response(node->udp_server, forward)) {
        (void)fprintf(stderr, "nat_detect_node event=filter_response_failed packet_number=%llu\n",
                      (unsigned long long)forward->packet_number);
        return;
    }
    (void)fprintf(stderr, "nat_detect_node event=filter_response_sent packet_number=%llu\n",
                  (unsigned long long)forward->packet_number);
}

static bool nat_detect_node_message_from_payload(uint8_t type, const uint8_t* payload, uint32_t payload_length,
                                                 uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE], size_t* length)
{
    *length = utp_ntrs_control_encode_header(message, UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE, type, payload_length);
    if (*length == 0u) {
        return false;
    }
    (void)memcpy(message + UTP_NTRS_CONTROL_HEADER_SIZE, payload, payload_length);
    return true;
}

static void nat_detect_node_disconnect_deferred(evutil_socket_t fd, int16_t events, void* user_data)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);

    (void)fd;
    (void)events;
    node->close_scheduled = false;
    nat_detect_node_disconnect(node);
}

static void nat_detect_node_schedule_disconnect(nat_detect_node_t* node)
{
    if (!node->close_scheduled) {
        node->close_scheduled = true;
        if (event_base_once(node->base, -1, EV_TIMEOUT, nat_detect_node_disconnect_deferred, node, NULL) != 0) {
            node->close_scheduled = false;
        }
    }
}

static void nat_detect_node_disconnect(nat_detect_node_t* node)
{
    if (node->hub_stream != NULL) {
        char local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_node event=hub_disconnected node=%s registered=%u\n",
                      nat_detect_node_instance(&node->registration.instance, local), node->registered ? 1u : 0u);
    }
    if (node->hub_stream != NULL) {
        utp_ntrs_tls_stream_free(node->hub_stream);
        node->hub_stream = NULL;
    }
    node->registered             = false;
    node->assignment_versions[0] = 0u;
    node->assignment_versions[1] = 0u;
    node->assignments[0]         = {};
    node->assignments[1]         = {};
    utp_ntrs_control_stream_init(&node->control);
    if (node->udp_server != NULL) {
        utp_ntrs_udp_server_set_primary(node->udp_server, NULL, NULL);
    }
}

static bool nat_detect_node_send(nat_detect_node_t* node, const uint8_t* message, size_t length)
{
    if (node->hub_stream == NULL || !utp_ntrs_tls_stream_send(node->hub_stream, message, length)) {
        (void)fprintf(stderr, "nat_detect_node event=hub_send_failed\n");
        nat_detect_node_schedule_disconnect(node);
        return false;
    }
    return true;
}

static void nat_detect_node_on_ready(void* user_data)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);
    uint8_t                  message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    const size_t length = utp_ntrs_control_encode_registration(message, sizeof(message), &node->registration);

    if (length == 0u || !nat_detect_node_send(node, message, length)) {
        nat_detect_node_schedule_disconnect(node);
        return;
    }
    (void)fprintf(stderr, "nat_detect_node event=hub_control_ready registration_sent\n");
}

static void nat_detect_node_on_message(void* user_data, uint8_t type, const uint8_t* payload, uint32_t payload_length)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);
    uint8_t                  message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t                   length;

    if (!nat_detect_node_message_from_payload(type, payload, payload_length, message, &length)) {
        (void)fprintf(stderr, "nat_detect_node event=hub_message_overflow type=%u\n", (uint32_t)type);
        nat_detect_node_schedule_disconnect(node);
        return;
    }
    if (!node->registered) {
        utp_ntrs_registration_ok_t registration_ok;

        if (type != UTP_NTRS_CONTROL_NODE_REGISTER_OK ||
            !utp_ntrs_control_decode_registration_ok(message, length, &registration_ok)) {
            (void)fprintf(stderr, "nat_detect_node event=hub_registration_rejected type=%u\n", (uint32_t)type);
            nat_detect_node_schedule_disconnect(node);
            return;
        }
        {
            utp_ntrs_node_family_t* const family =
                node->registration.ipv4.valid ? &node->registration.ipv4 : &node->registration.ipv6;
            char public_endpoint[64];

            if (registration_ok.public_endpoint.family != family->family) {
                (void)fprintf(stderr, "nat_detect_node event=hub_registration_family_mismatch\n");
                nat_detect_node_schedule_disconnect(node);
                return;
            }
            (void)memcpy(family->public_endpoint.address, registration_ok.public_endpoint.address,
                         sizeof(family->public_endpoint.address));
            (void)memcpy(family->probe_endpoint.address, registration_ok.public_endpoint.address,
                         sizeof(family->probe_endpoint.address));
            (void)memcpy(family->change_port_endpoint.address, registration_ok.public_endpoint.address,
                         sizeof(family->change_port_endpoint.address));
            (void)memcpy(family->control_endpoint.address, registration_ok.public_endpoint.address,
                         sizeof(family->control_endpoint.address));
            utp_ntrs_udp_server_set_public_endpoints(node->udp_server, &family->probe_endpoint,
                                                     &family->change_port_endpoint);
            (void)fprintf(stderr, "nat_detect_node event=public_endpoint_confirmed endpoint=%s\n",
                          utp_ntrs_endpoint_format(&family->public_endpoint, public_endpoint, sizeof(public_endpoint)));
        }
        node->registered = true;
        (void)fprintf(stderr, "nat_detect_node event=hub_registered\n");
        return;
    }
    if (type == UTP_NTRS_CONTROL_NODE_ASSIGNMENT) {
        utp_ntrs_assignment_t assignment;
        uint8_t               slot;

        if (!utp_ntrs_control_decode_assignment(message, length, &assignment)) {
            (void)fprintf(stderr, "nat_detect_node event=assignment_invalid\n");
            nat_detect_node_schedule_disconnect(node);
            return;
        }
        if ((assignment.family == (uint8_t)AF_INET && !node->registration.ipv4.valid) ||
            (assignment.family == (uint8_t)AF_INET6 && !node->registration.ipv6.valid)) {
            (void)fprintf(stderr, "nat_detect_node event=assignment_family_mismatch family=%u\n",
                          (uint32_t)assignment.family);
            nat_detect_node_schedule_disconnect(node);
            return;
        }
        slot = nat_detect_node_family_slot(assignment.family);
        if (assignment.version > node->assignment_versions[slot]) {
            char primary[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
            char backup[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

            node->assignment_versions[slot] = assignment.version;
            node->assignments[slot]         = assignment;
            utp_ntrs_udp_server_set_primary(node->udp_server, NULL, NULL);
            if (assignment.has_primary &&
                !utp_ntrs_peer_manager_connect(node->peers, &assignment.primary, &assignment.primary_control)) {
                (void)fprintf(stderr, "nat_detect_node event=primary_link_connect_failed family=%u\n",
                              (uint32_t)assignment.family);
                nat_detect_node_request_replacement(node, &assignment.primary);
            }
            if (assignment.has_backup &&
                !utp_ntrs_peer_manager_connect(node->peers, &assignment.backup, &assignment.backup_control)) {
                (void)fprintf(stderr, "nat_detect_node event=backup_link_connect_failed family=%u\n",
                              (uint32_t)assignment.family);
                nat_detect_node_request_replacement(node, &assignment.backup);
            }
            (void)fprintf(stderr,
                          "nat_detect_node event=assignment_updated family=%u "
                          "version=%llu primary=%s backup=%s\n",
                          (uint32_t)assignment.family, (unsigned long long)assignment.version,
                          assignment.has_primary ? nat_detect_node_instance(&assignment.primary, primary) : "none",
                          assignment.has_backup ? nat_detect_node_instance(&assignment.backup, backup) : "none");
        }
        return;
    }
    (void)fprintf(stderr, "nat_detect_node event=unexpected_hub_message type=%u\n", (uint32_t)type);
    nat_detect_node_schedule_disconnect(node);
}

static void nat_detect_node_on_plaintext(void* user_data, const uint8_t* data, size_t length)
{
    nat_detect_node_t* const node = static_cast<nat_detect_node_t*>(user_data);

    if (!utp_ntrs_control_stream_feed(&node->control, data, length, nat_detect_node_on_message, node)) {
        (void)fprintf(stderr, "nat_detect_node event=invalid_hub_control_stream\n");
        nat_detect_node_schedule_disconnect(node);
    }
}

static void nat_detect_node_on_closed(void* user_data)
{
    (void)fprintf(stderr, "nat_detect_node event=hub_control_closed\n");
    nat_detect_node_schedule_disconnect(static_cast<nat_detect_node_t*>(user_data));
}

static void nat_detect_node_connect(nat_detect_node_t* node)
{
    struct sockaddr_storage        address;
    socklen_t                      address_length;
    int32_t                        fd;
    const utp_ntrs_tls_callbacks_t callbacks = {
        .ready     = nat_detect_node_on_ready,
        .plaintext = nat_detect_node_on_plaintext,
        .closed    = nat_detect_node_on_closed,
    };

    if (node->hub_stream != NULL || !utp_ntrs_endpoint_to_sockaddr(&node->hub_endpoint, &address, &address_length)) {
        return;
    }
    fd = socket(node->hub_endpoint.family, SOCK_STREAM, 0);
    if (fd < 0 || !utp_ntrs_socket_bind_interface(fd, node->interface_name) ||
        evutil_make_socket_nonblocking(fd) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        (void)fprintf(stderr, "nat_detect_node event=hub_socket_create_failed\n");
        return;
    }
    {
        char endpoint[64];

        (void)fprintf(stderr, "nat_detect_node event=hub_connecting endpoint=%s\n",
                      utp_ntrs_endpoint_format(&node->hub_endpoint, endpoint, sizeof(endpoint)));
    }
    node->hub_stream = utp_ntrs_tls_stream_new(node->base, fd, node->hub_tls_context, false, &callbacks, node);
    if (node->hub_stream == NULL ||
        bufferevent_socket_connect(utp_ntrs_tls_stream_transport(node->hub_stream), (const struct sockaddr*)&address,
                                   (int32_t)address_length) != 0) {
        (void)fprintf(stderr, "nat_detect_node event=hub_connect_failed\n");
        nat_detect_node_disconnect(node);
    }
}

static void nat_detect_node_on_reconnect(evutil_socket_t fd, int16_t events, void* user_data)
{
    (void)fd;
    (void)events;
    nat_detect_node_connect(static_cast<nat_detect_node_t*>(user_data));
}

static void nat_detect_node_on_heartbeat(evutil_socket_t fd, int16_t events, void* user_data)
{
    nat_detect_node_t* const        node      = static_cast<nat_detect_node_t*>(user_data);
    const utp_ntrs_node_heartbeat_t heartbeat = {
        .instance = node->registration.instance,
        .load     = node->registration.load,
    };
    uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t  length;

    (void)fd;
    (void)events;
    if (!node->registered) {
        return;
    }
    length = utp_ntrs_control_encode_heartbeat(message, sizeof(message), &heartbeat);
    if (length == 0u || !nat_detect_node_send(node, message, length)) {
        (void)fprintf(stderr, "nat_detect_node event=heartbeat_send_failed\n");
        nat_detect_node_schedule_disconnect(node);
    }
}

static void nat_detect_node_on_peer_tick(evutil_socket_t fd, int16_t events, void* user_data)
{
    (void)fd;
    (void)events;
    utp_ntrs_peer_manager_tick(static_cast<nat_detect_node_t*>(user_data)->peers);
}

static void nat_detect_node_on_signal(evutil_socket_t fd, int16_t events, void* user_data)
{
    (void)fd;
    (void)events;
    (void)fprintf(stderr, "nat_detect_node event=stopping\n");
    (void)event_base_loopbreak(static_cast<struct event_base*>(user_data));
}

static int32_t nat_detect_node_run(const nat_detect_node_options_t* options)
{
    nat_detect_node_t               node             = {};
    utp_ntrs_udp_server_options_t   udp_options      = {};
    struct event*                   signal_int       = NULL;
    struct event*                   signal_term      = NULL;
    struct timeval                  reconnect_period = {.tv_sec = 1, .tv_usec = 0};
    struct timeval                  heartbeat_period;
    const struct timeval            peer_tick_period = {.tv_sec = 10, .tv_usec = 0};
    utp_ntrs_peer_manager_options_t peer_options     = {};
    const char* const               node_id          = options->node_id;
    const char* const               boot_id          = options->boot_id;
    const char* const               hub              = options->hub;
    const char* const               probe            = options->probe;
    const char* const               change_port      = options->change_port;
    const char* const               control          = options->control;
    const char* const               interface_name   = options->interface_name;
    const char* const               certificate      = options->certificate;
    const char* const               private_key      = options->private_key;
    const uint32_t                  workers          = options->workers;
    const uint32_t                  source_rate      = options->source_rate;
    const uint32_t                  source_burst     = options->source_burst;
    const int32_t                   address_family   = options->use_ipv6 ? AF_INET6 : AF_INET;
    int32_t                         result           = EXIT_FAILURE;

    node.registration.load         = options->load;
    node.registration.heartbeat_ms = options->heartbeat_ms;
    if (hub == NULL || node_id == NULL || node_id[0] == '\0' || strlen(node_id) > UTP_NTRS_NODE_ID_SIZE ||
        workers == 0u || workers > UINT16_MAX || node.registration.heartbeat_ms == 0u ||
        ((certificate == NULL) != (private_key == NULL)) ||
        (boot_id != NULL && !utp_ntrs_hex_decode(boot_id, node.registration.instance.boot_id, UTP_NTRS_BOOT_ID_SIZE)) ||
        (boot_id == NULL &&
         getrandom(node.registration.instance.boot_id, UTP_NTRS_BOOT_ID_SIZE, 0u) != (ssize_t)UTP_NTRS_BOOT_ID_SIZE) ||
        !utp_ntrs_endpoint_resolve_for_family(hub, address_family, &node.hub_endpoint) ||
        !utp_ntrs_endpoint_parse(probe, &udp_options.probe_endpoint) ||
        !utp_ntrs_endpoint_parse(change_port, &udp_options.change_port_endpoint) ||
        !utp_ntrs_endpoint_parse(control, &node.registration.ipv4.control_endpoint) ||
        node.hub_endpoint.family != (uint8_t)address_family ||
        udp_options.probe_endpoint.family != (uint8_t)address_family ||
        node.hub_endpoint.family != udp_options.probe_endpoint.family ||
        udp_options.probe_endpoint.family != udp_options.change_port_endpoint.family ||
        udp_options.probe_endpoint.family != node.registration.ipv4.control_endpoint.family) {
        return EXIT_FAILURE;
    }
    (void)memcpy(node.registration.instance.node_id, node_id, strlen(node_id));
    node.interface_name                     = interface_name;
    node.node_name                          = node_id;
    udp_options.worker_count                = (uint16_t)workers;
    udp_options.source_rate_per_second      = source_rate;
    udp_options.source_burst                = source_burst;
    udp_options.interface_name              = interface_name;
    udp_options.public_probe_endpoint       = udp_options.probe_endpoint;
    udp_options.public_change_port_endpoint = udp_options.change_port_endpoint;
    node.registration.ipv4                  = utp_ntrs_node_family_t{
        .public_endpoint      = udp_options.probe_endpoint,
        .probe_endpoint       = udp_options.probe_endpoint,
        .change_port_endpoint = udp_options.change_port_endpoint,
        .control_endpoint     = node.registration.ipv4.control_endpoint,
        .family               = udp_options.probe_endpoint.family,
        .valid                = true,
    };
    node.registration.ipv4.public_endpoint.port = 0u;
    if (node.registration.ipv4.family != (uint8_t)AF_INET) {
        node.registration.ipv6        = node.registration.ipv4;
        node.registration.ipv6.family = (uint8_t)AF_INET6;
        node.registration.ipv4        = {};
    }
    if ((node.base = event_base_new()) == NULL) {
        goto cleanup;
    }
    if (certificate != NULL &&
        ((node.hub_tls_context = utp_ntrs_tls_client_context_new()) == NULL ||
         (node.peer_server_tls_context = utp_ntrs_tls_server_context_new(certificate, private_key)) == NULL)) {
        goto cleanup;
    }
    udp_options.main_base       = node.base;
    udp_options.on_forward      = nat_detect_node_on_udp_forward;
    udp_options.user_data       = &node;
    peer_options.base           = node.base;
    peer_options.client_tls     = node.hub_tls_context;
    peer_options.server_tls     = node.peer_server_tls_context;
    peer_options.local          = node.registration.instance;
    peer_options.listen         = node.registration.ipv4.valid ? node.registration.ipv4.control_endpoint
                                                               : node.registration.ipv6.control_endpoint;
    peer_options.interface_name = node.interface_name;
    peer_options.on_active      = nat_detect_node_on_peer_active;
    peer_options.on_failed      = nat_detect_node_on_peer_failed;
    peer_options.on_forward     = nat_detect_node_on_peer_forward;
    peer_options.user_data      = &node;
    if ((node.udp_server = utp_ntrs_udp_server_start(&udp_options)) == NULL ||
        (node.peers = utp_ntrs_peer_manager_start(&peer_options)) == NULL) {
        goto cleanup;
    }
    {
        char                                local[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
        char                                hub_endpoint[64];
        char                                probe_endpoint[64];
        char                                control_endpoint[64];
        const utp_ntrs_node_family_t* const family =
            node.registration.ipv4.valid ? &node.registration.ipv4 : &node.registration.ipv6;

        (void)fprintf(stderr,
                      "nat_detect_node event=starting node_name=%s node=%s hub=%s probe=%s "
                      "change_port=%u "
                      "control=%s workers=%u transport=%s interface=%s\n",
                      node.node_name, nat_detect_node_instance(&node.registration.instance, local),
                      utp_ntrs_endpoint_format(&node.hub_endpoint, hub_endpoint, sizeof(hub_endpoint)),
                      utp_ntrs_endpoint_format(&family->probe_endpoint, probe_endpoint, sizeof(probe_endpoint)),
                      (uint32_t)family->change_port_endpoint.port,
                      utp_ntrs_endpoint_format(&family->control_endpoint, control_endpoint, sizeof(control_endpoint)),
                      (uint32_t)workers, node.hub_tls_context != NULL ? "tls" : "tcp",
                      node.interface_name != NULL ? node.interface_name : "any");
    }
    utp_ntrs_control_stream_init(&node.control);
    heartbeat_period.tv_sec  = (int32_t)(node.registration.heartbeat_ms / 1000u);
    heartbeat_period.tv_usec = (int32_t)((node.registration.heartbeat_ms % 1000u) * 1000u);
    node.reconnect_event     = event_new(node.base, -1, EV_PERSIST, nat_detect_node_on_reconnect, &node);
    node.heartbeat_event     = event_new(node.base, -1, EV_PERSIST, nat_detect_node_on_heartbeat, &node);
    node.peer_tick_event     = event_new(node.base, -1, EV_PERSIST, nat_detect_node_on_peer_tick, &node);
    signal_int               = evsignal_new(node.base, SIGINT, nat_detect_node_on_signal, node.base);
    signal_term              = evsignal_new(node.base, SIGTERM, nat_detect_node_on_signal, node.base);
    if (node.reconnect_event == NULL || node.heartbeat_event == NULL || node.peer_tick_event == NULL ||
        signal_int == NULL || signal_term == NULL || event_add(node.reconnect_event, &reconnect_period) != 0 ||
        event_add(node.heartbeat_event, &heartbeat_period) != 0 || event_add(signal_int, NULL) != 0 ||
        event_add(signal_term, NULL) != 0 || event_add(node.peer_tick_event, &peer_tick_period) != 0) {
        goto cleanup;
    }
    nat_detect_node_connect(&node);
    (void)event_base_dispatch(node.base);
    result = EXIT_SUCCESS;

cleanup:
    if (signal_int != NULL) {
        event_free(signal_int);
    }
    if (signal_term != NULL) {
        event_free(signal_term);
    }
    if (node.reconnect_event != NULL) {
        event_free(node.reconnect_event);
    }
    if (node.heartbeat_event != NULL) {
        event_free(node.heartbeat_event);
    }
    if (node.peer_tick_event != NULL) {
        event_free(node.peer_tick_event);
    }
    nat_detect_node_disconnect(&node);
    if (node.peers != NULL) {
        utp_ntrs_peer_manager_stop(node.peers);
    }
    if (node.udp_server != NULL) {
        utp_ntrs_udp_server_stop(node.udp_server);
    }
    if (node.peer_server_tls_context != NULL) {
        utp_ntrs_tls_context_free(node.peer_server_tls_context);
    }
    if (node.hub_tls_context != NULL) {
        utp_ntrs_tls_context_free(node.hub_tls_context);
    }
    if (node.base != NULL) {
        event_base_free(node.base);
    }
    return result;
}

int main(int argc, char** argv)
{
    CLI::App    cli{"NTRS NAT detection node"};
    std::string hub;
    std::string node_id;
    std::string probe;
    std::string change_port;
    std::string control;
    std::string interface_name;
    std::string certificate;
    std::string private_key;
    std::string boot_id;
    uint32_t    load         = 0u;
    uint32_t    heartbeat_ms = 5000u;
    uint32_t    workers      = 1u;
    uint32_t    source_rate  = 0u;
    uint32_t    source_burst = 0u;
    bool        use_ipv6     = false;

    cli.add_option("-H,--hub", hub, "Hub endpoint")->required();
    cli.add_option("-n,--node-id", node_id, "Stable node ID (1-128 bytes)")->required();
    cli.add_option("-p,--probe", probe, "UDP probe listen endpoint");
    cli.add_option("-q,--change-port", change_port, "UDP alternate-port listen endpoint");
    cli.add_option("-C,--control", control, "Node control listen endpoint");
    cli.add_option("-i,--interface", interface_name, "Bind all service sockets to this interface");
    cli.add_option("-c,--cert", certificate, "TLS certificate file");
    cli.add_option("-k,--key", private_key, "TLS private key file");
    cli.add_option("-b,--boot-id", boot_id, "Optional 16-byte hexadecimal boot ID");
    cli.add_option("-l,--load", load, "Current node load");
    cli.add_option("-t,--heartbeat-ms", heartbeat_ms, "Hub heartbeat interval in milliseconds");
    cli.add_option("-w,--workers", workers, "UDP SO_REUSEPORT worker count");
    cli.add_option("-r,--source-rate", source_rate, "Per-source request rate limit");
    cli.add_option("-B,--source-burst", source_burst, "Per-source request burst limit");
    cli.add_flag("-6", use_ipv6, "Use IPv6 only and resolve the Hub with AAAA records");
    CLI11_PARSE(cli, argc, argv);

    if (probe.empty()) {
        probe = use_ipv6 ? "[::]:24001" : "0.0.0.0:24001";
    }
    if (change_port.empty()) {
        change_port = use_ipv6 ? "[::]:24002" : "0.0.0.0:24002";
    }
    if (control.empty()) {
        control = use_ipv6 ? "[::]:24003" : "0.0.0.0:24003";
    }

    const nat_detect_node_options_t options = {
        .hub            = hub.c_str(),
        .node_id        = node_id.c_str(),
        .probe          = probe.c_str(),
        .change_port    = change_port.c_str(),
        .control        = control.c_str(),
        .interface_name = interface_name.empty() ? NULL : interface_name.c_str(),
        .certificate    = certificate.empty() ? NULL : certificate.c_str(),
        .private_key    = private_key.empty() ? NULL : private_key.c_str(),
        .boot_id        = boot_id.empty() ? NULL : boot_id.c_str(),
        .load           = load,
        .heartbeat_ms   = heartbeat_ms,
        .workers        = workers,
        .source_rate    = source_rate,
        .source_burst   = source_burst,
        .use_ipv6       = use_ipv6,
    };

    utp_ntrs_app_log_init("nat_detect_node");
    return nat_detect_node_run(&options);
}
