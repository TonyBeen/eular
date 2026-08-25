#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <netinet/in.h>
#include <ntrs/service.h>
#include <sys/socket.h>
#include <utils/CLI11.hpp>

#include "app_log.h"
#include "service_util.h"
#include "tls_stream.h"

#define fprintf(stream, ...) UTP_NTRS_APP_LOG(stream, __VA_ARGS__)

typedef struct nat_detect_hub_service nat_detect_hub_service_t;

typedef struct nat_detect_hub_session {
    nat_detect_hub_service_t*      service;            // 所属 Hub
    struct nat_detect_hub_session* next;               // Hub 连接链表
    utp_ntrs_tls_stream_t*         tls;                // Hub TLS 控制连接
    utp_ntrs_control_stream_t      control;            // 控制消息重组状态
    utp_ntrs_endpoint_t            peer_endpoint;      // Hub 从 TCP 连接观察到的 Node 地址
    utp_ntrs_node_registration_t   registration;       // 首次注册后的 Node 信息
    bool                           registered;         // 是否已写入 Hub 成员表
    bool                           owns_registration;  // 是否持有当前成员表记录
    bool                           close_scheduled;
} nat_detect_hub_session_t;

struct nat_detect_hub_service {
    struct event_base*        base;      // 主 libevent loop
    struct evconnlistener*    listener;  // Hub TCP 监听器
    struct event*             sweep;     // 心跳清理定时器
    SSL_CTX*                  tls;       // Hub 服务端 TLS 上下文
    utp_ntrs_hub_t            hub;       // Node 成员与 assignment 状态
    nat_detect_hub_session_t* sessions;  // 全部已建立 TCP 连接
};

static void nat_detect_hub_session_destroy(nat_detect_hub_session_t* session);
static void nat_detect_hub_session_schedule_close(nat_detect_hub_session_t* session);

/** @brief 用控制连接的源地址覆盖 Node 自报的服务 IP，仅信任其监听端口。 */
static bool nat_detect_hub_registration_apply_peer_endpoint(utp_ntrs_node_registration_t* registration,
                                                            const utp_ntrs_endpoint_t*    peer_endpoint)
{
    utp_ntrs_node_family_t* families[] = {&registration->ipv4, &registration->ipv6};
    uint32_t                index;

    for (index = 0u; index < sizeof(families) / sizeof(families[0]); ++index) {
        utp_ntrs_node_family_t* const family = families[index];

        if (!family->valid) {
            continue;
        }
        if (family->family != peer_endpoint->family) {
            return false;
        }
        (void)memcpy(family->public_endpoint.address, peer_endpoint->address, sizeof(family->public_endpoint.address));
        (void)memcpy(family->probe_endpoint.address, peer_endpoint->address, sizeof(family->probe_endpoint.address));
        (void)memcpy(family->change_port_endpoint.address, peer_endpoint->address,
                     sizeof(family->change_port_endpoint.address));
        (void)memcpy(family->control_endpoint.address, peer_endpoint->address,
                     sizeof(family->control_endpoint.address));
    }
    return true;
}

static const char* nat_detect_hub_session_instance(const nat_detect_hub_session_t* session,
                                                   char buffer[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE])
{
    return session->registered ? utp_ntrs_node_instance_format(&session->registration.instance, buffer,
                                                               UTP_NTRS_NODE_INSTANCE_TEXT_SIZE)
                               : "unregistered";
}

static bool nat_detect_hub_session_same_node_id(const nat_detect_hub_session_t* left,
                                                const nat_detect_hub_session_t* right)
{
    return memcmp(left->registration.instance.node_id, right->registration.instance.node_id,
                  sizeof(left->registration.instance.node_id)) == 0;
}

static void nat_detect_hub_session_replace_owner(nat_detect_hub_session_t* session)
{
    nat_detect_hub_session_t* current;

    for (current = session->service->sessions; current != NULL; current = current->next) {
        if (current != session && current->registered && current->owns_registration &&
            nat_detect_hub_session_same_node_id(current, session)) {
            char previous[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
            char replacement[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

            (void)fprintf(stderr, "nat_detect_hub event=node_replaced previous=%s replacement=%s\n",
                          nat_detect_hub_session_instance(current, previous),
                          nat_detect_hub_session_instance(session, replacement));
            current->owns_registration = false;
            nat_detect_hub_session_schedule_close(current);
        }
    }
}

static void nat_detect_hub_session_close_deferred(evutil_socket_t fd, int16_t events, void* user_data)
{
    nat_detect_hub_session_t* const session = static_cast<nat_detect_hub_session_t*>(user_data);

    (void)fd;
    (void)events;
    nat_detect_hub_session_destroy(session);
}

static void nat_detect_hub_session_schedule_close(nat_detect_hub_session_t* session)
{
    if (!session->close_scheduled) {
        session->close_scheduled = true;
        if (event_base_once(session->service->base, -1, EV_TIMEOUT, nat_detect_hub_session_close_deferred, session,
                            NULL) != 0) {
            session->close_scheduled = false;
        }
    }
}

static bool nat_detect_hub_session_send(nat_detect_hub_session_t* session, const uint8_t* message, size_t length)
{
    if (!utp_ntrs_tls_stream_send(session->tls, message, length)) {
        char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_hub event=control_send_failed node=%s\n",
                      nat_detect_hub_session_instance(session, instance));
        nat_detect_hub_session_schedule_close(session);
        return false;
    }
    return true;
}

static bool nat_detect_hub_session_send_assignment(nat_detect_hub_session_t* session, uint8_t family)
{
    utp_ntrs_assignment_t assignment;
    uint8_t               message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    const bool            family_valid =
        family == (uint8_t)AF_INET ? session->registration.ipv4.valid : session->registration.ipv6.valid;
    const size_t length =
        utp_ntrs_hub_get_assignment(&session->service->hub, &session->registration.instance, family, &assignment)
            ? utp_ntrs_control_encode_assignment(message, sizeof(message), &assignment)
            : 0u;

    if (!family_valid) {
        return true;
    }
    if (length == 0u || !nat_detect_hub_session_send(session, message, length)) {
        return false;
    }
    {
        char node[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
        char primary[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
        char backup[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(
            stderr,
            "nat_detect_hub event=assignment_sent node=%s family=%u version=%llu "
            "primary=%s backup=%s\n",
            nat_detect_hub_session_instance(session, node), (uint32_t)family, (unsigned long long)assignment.version,
            assignment.has_primary
                ? utp_ntrs_node_instance_format(&assignment.primary, primary, UTP_NTRS_NODE_INSTANCE_TEXT_SIZE)
                : "none",
            assignment.has_backup
                ? utp_ntrs_node_instance_format(&assignment.backup, backup, UTP_NTRS_NODE_INSTANCE_TEXT_SIZE)
                : "none");
    }
    return true;
}

static void nat_detect_hub_broadcast_assignments(nat_detect_hub_service_t* service)
{
    nat_detect_hub_session_t* session;

    for (session = service->sessions; session != NULL; session = session->next) {
        if (session->registered && !session->close_scheduled) {
            (void)nat_detect_hub_session_send_assignment(session, (uint8_t)AF_INET);
            (void)nat_detect_hub_session_send_assignment(session, (uint8_t)AF_INET6);
        }
    }
}

static bool nat_detect_hub_message_from_payload(uint8_t type, const uint8_t* payload, uint32_t payload_length,
                                                uint8_t message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE], size_t* length)
{
    *length = utp_ntrs_control_encode_header(message, UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE, type, payload_length);
    if (*length == 0u) {
        return false;
    }
    (void)memcpy(message + UTP_NTRS_CONTROL_HEADER_SIZE, payload, payload_length);
    return true;
}

static void nat_detect_hub_session_on_message(void* user_data, uint8_t type, const uint8_t* payload,
                                              uint32_t payload_length)
{
    nat_detect_hub_session_t* const session = static_cast<nat_detect_hub_session_t*>(user_data);
    uint8_t                         message[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
    size_t                          length;

    if (!nat_detect_hub_message_from_payload(type, payload, payload_length, message, &length)) {
        (void)fprintf(stderr, "nat_detect_hub event=control_message_overflow type=%u\n", (uint32_t)type);
        nat_detect_hub_session_schedule_close(session);
        return;
    }
    if (!session->registered) {
        utp_ntrs_node_registration_t registration;
        bool                         replaced;

        if (type != UTP_NTRS_CONTROL_NODE_REGISTER ||
            !utp_ntrs_control_decode_registration(message, length, &registration) ||
            !nat_detect_hub_registration_apply_peer_endpoint(&registration, &session->peer_endpoint) ||
            !utp_ntrs_hub_register(&session->service->hub, &registration, utp_ntrs_now_ms(), &replaced)) {
            uint8_t reject[UTP_NTRS_CONTROL_HEADER_SIZE];

            if (utp_ntrs_control_encode_header(reject, sizeof(reject), UTP_NTRS_CONTROL_NODE_REGISTER_REJECT, 0u) !=
                0u) {
                (void)nat_detect_hub_session_send(session, reject, sizeof(reject));
            }
            (void)fprintf(stderr, "nat_detect_hub event=registration_rejected\n");
            nat_detect_hub_session_schedule_close(session);
            return;
        }
        session->registration = registration;
        session->registered   = true;
        nat_detect_hub_session_replace_owner(session);
        session->owns_registration = true;
        {
            char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];
            char peer[64];

            (void)fprintf(stderr,
                          "nat_detect_hub event=node_registered node=%s peer=%s "
                          "replaced=%u\n",
                          nat_detect_hub_session_instance(session, instance),
                          utp_ntrs_endpoint_format(&session->peer_endpoint, peer, sizeof(peer)), replaced ? 1u : 0u);
        }
        {
            uint8_t                          accepted[UTP_NTRS_CONTROL_HEADER_SIZE + 19u];
            const utp_ntrs_registration_ok_t registration_ok = {
                .public_endpoint = session->registration.ipv4.valid ? session->registration.ipv4.public_endpoint
                                                                    : session->registration.ipv6.public_endpoint,
            };
            const size_t accepted_length =
                utp_ntrs_control_encode_registration_ok(accepted, sizeof(accepted), &registration_ok);

            if (accepted_length == 0u || !nat_detect_hub_session_send(session, accepted, accepted_length)) {
                nat_detect_hub_session_schedule_close(session);
                return;
            }
        }
        (void)replaced;
        nat_detect_hub_broadcast_assignments(session->service);
        return;
    }
    if (!session->owns_registration) {
        char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_hub event=stale_control_message node=%s type=%u\n",
                      nat_detect_hub_session_instance(session, instance), (uint32_t)type);
        nat_detect_hub_session_schedule_close(session);
        return;
    }
    if (type == UTP_NTRS_CONTROL_NODE_HEARTBEAT) {
        utp_ntrs_node_heartbeat_t heartbeat;

        if (!utp_ntrs_control_decode_heartbeat(message, length, &heartbeat) ||
            !utp_ntrs_node_instance_equal(&heartbeat.instance, &session->registration.instance) ||
            !utp_ntrs_hub_heartbeat(&session->service->hub, &heartbeat.instance, heartbeat.load, utp_ntrs_now_ms())) {
            char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

            (void)fprintf(stderr, "nat_detect_hub event=heartbeat_rejected node=%s\n",
                          nat_detect_hub_session_instance(session, instance));
            nat_detect_hub_session_schedule_close(session);
        }
        return;
    }
    if (type == UTP_NTRS_CONTROL_NODE_ASSIGNMENT_REQUEST) {
        utp_ntrs_assignment_request_t request;
        utp_ntrs_assignment_t         assignment;
        uint8_t                       response[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];
        size_t                        response_length;

        if (!utp_ntrs_control_decode_assignment_request(message, length, &request) ||
            !utp_ntrs_node_instance_equal(&request.instance, &session->registration.instance) ||
            !utp_ntrs_hub_request_assignment(&session->service->hub, &request, utp_ntrs_now_ms(), &assignment)) {
            char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

            (void)fprintf(stderr, "nat_detect_hub event=assignment_request_rejected node=%s\n",
                          nat_detect_hub_session_instance(session, instance));
            nat_detect_hub_session_schedule_close(session);
            return;
        }
        response_length = utp_ntrs_control_encode_assignment(response, sizeof(response), &assignment);
        if (response_length == 0u || !nat_detect_hub_session_send(session, response, response_length)) {
            char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

            (void)fprintf(stderr, "nat_detect_hub event=assignment_response_failed node=%s\n",
                          nat_detect_hub_session_instance(session, instance));
            nat_detect_hub_session_schedule_close(session);
        }
        return;
    }
    {
        char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_hub event=unexpected_control_message node=%s type=%u\n",
                      nat_detect_hub_session_instance(session, instance), (uint32_t)type);
    }
    nat_detect_hub_session_schedule_close(session);
}

static void nat_detect_hub_session_on_plaintext(void* user_data, const uint8_t* data, size_t length)
{
    nat_detect_hub_session_t* const session = static_cast<nat_detect_hub_session_t*>(user_data);

    if (!utp_ntrs_control_stream_feed(&session->control, data, length, nat_detect_hub_session_on_message, session)) {
        char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_hub event=invalid_control_stream node=%s\n",
                      nat_detect_hub_session_instance(session, instance));
        nat_detect_hub_session_schedule_close(session);
    }
}

static void nat_detect_hub_session_on_closed(void* user_data)
{
    nat_detect_hub_session_t* const session = static_cast<nat_detect_hub_session_t*>(user_data);
    char                            instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

    (void)fprintf(stderr, "nat_detect_hub event=control_closed node=%s\n",
                  nat_detect_hub_session_instance(session, instance));
    nat_detect_hub_session_schedule_close(static_cast<nat_detect_hub_session_t*>(user_data));
}

static void nat_detect_hub_session_destroy(nat_detect_hub_session_t* session)
{
    nat_detect_hub_session_t** link = &session->service->sessions;

    while (*link != session) {
        link = &(*link)->next;
    }
    *link = session->next;
    if (session->registered && session->owns_registration) {
        char instance[UTP_NTRS_NODE_INSTANCE_TEXT_SIZE];

        (void)fprintf(stderr, "nat_detect_hub event=node_disconnected node=%s\n",
                      nat_detect_hub_session_instance(session, instance));
        (void)utp_ntrs_hub_remove(&session->service->hub, &session->registration.instance, utp_ntrs_now_ms());
    }
    utp_ntrs_tls_stream_free(session->tls);
    free(session);
}

static void nat_detect_hub_on_accept(struct evconnlistener* listener, evutil_socket_t fd, struct sockaddr* address,
                                     int32_t address_length, void* user_data)
{
    nat_detect_hub_service_t* const service   = static_cast<nat_detect_hub_service_t*>(user_data);
    nat_detect_hub_session_t* const session   = static_cast<nat_detect_hub_session_t*>(calloc(1u, sizeof(*session)));
    const utp_ntrs_tls_callbacks_t  callbacks = {
        .ready     = NULL,
        .plaintext = nat_detect_hub_session_on_plaintext,
        .closed    = nat_detect_hub_session_on_closed,
    };

    (void)listener;
    if (session == NULL ||
        !utp_ntrs_endpoint_from_sockaddr(&session->peer_endpoint, address, (socklen_t)address_length)) {
        evutil_closesocket(fd);
        free(session);
        return;
    }
    session->service = service;
    utp_ntrs_control_stream_init(&session->control);
    session->tls = utp_ntrs_tls_stream_new(service->base, (int32_t)fd, service->tls, true, &callbacks, session);
    if (session->tls == NULL) {
        free(session);
        return;
    }
    session->next     = service->sessions;
    service->sessions = session;
    (void)fprintf(stderr, "nat_detect_hub event=control_accepted\n");
}

static void nat_detect_hub_on_sweep(evutil_socket_t fd, int16_t events, void* user_data)
{
    nat_detect_hub_service_t* const service = static_cast<nat_detect_hub_service_t*>(user_data);
    nat_detect_hub_session_t*       session;

    (void)fd;
    (void)events;
    const size_t expired = utp_ntrs_hub_sweep_expired(&service->hub, utp_ntrs_now_ms());

    if (expired != 0u) {
        (void)fprintf(stderr, "nat_detect_hub event=heartbeat_expired count=%zu\n", expired);
    }
    for (session = service->sessions; session != NULL; session = session->next) {
        utp_ntrs_assignment_t assignment;

        if (session->registered && session->owns_registration &&
            !utp_ntrs_hub_get_assignment(&service->hub, &session->registration.instance, (uint8_t)AF_INET,
                                         &assignment)) {
            nat_detect_hub_session_schedule_close(session);
        }
    }
}

static void nat_detect_hub_on_signal(evutil_socket_t fd, int16_t events, void* user_data)
{
    (void)fd;
    (void)events;
    (void)event_base_loopbreak(static_cast<struct event_base*>(user_data));
}

static struct evconnlistener* nat_detect_hub_listener_new(struct event_base* base, const utp_ntrs_endpoint_t* endpoint,
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
        struct evconnlistener* const listener = evconnlistener_new(base, nat_detect_hub_on_accept, user_data,
                                                                   LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, fd);

        if (listener == NULL) {
            (void)close(fd);
        }
        return listener;
    }
}

static int32_t nat_detect_hub_run(const char* listen, const char* interface_name, const char* certificate,
                                  const char* private_key, bool use_ipv6)
{
    nat_detect_hub_service_t service = {};
    utp_ntrs_endpoint_t      endpoint;
    struct sockaddr_storage  address;
    socklen_t                address_length;
    struct event*            signal_int;
    struct event*            signal_term;
    struct timeval           sweep_interval = {1, 0};
    int32_t                  result         = EXIT_FAILURE;

    if (((certificate == NULL) != (private_key == NULL)) ||
        !utp_ntrs_endpoint_resolve_for_family(listen, use_ipv6 ? AF_INET6 : AF_INET, &endpoint) ||
        !utp_ntrs_endpoint_to_sockaddr(&endpoint, &address, &address_length) ||
        (service.base = event_base_new()) == NULL) {
        goto cleanup;
    }
    if (certificate != NULL && (service.tls = utp_ntrs_tls_server_context_new(certificate, private_key)) == NULL) {
        goto cleanup;
    }
    utp_ntrs_hub_init(&service.hub);
    {
        char endpoint_text[INET6_ADDRSTRLEN + 8u];

        (void)fprintf(stderr, "nat_detect_hub event=starting listen=%s transport=%s interface=%s\n",
                      utp_ntrs_endpoint_format(&endpoint, endpoint_text, sizeof(endpoint_text)),
                      service.tls != NULL ? "tls" : "tcp", interface_name != NULL ? interface_name : "any");
    }
    service.listener =
        nat_detect_hub_listener_new(service.base, &endpoint, &address, address_length, interface_name, &service);
    service.sweep = event_new(service.base, -1, EV_PERSIST, nat_detect_hub_on_sweep, &service);
    signal_int    = evsignal_new(service.base, SIGINT, nat_detect_hub_on_signal, service.base);
    signal_term   = evsignal_new(service.base, SIGTERM, nat_detect_hub_on_signal, service.base);
    if (service.listener == NULL || service.sweep == NULL || signal_int == NULL || signal_term == NULL ||
        event_add(service.sweep, &sweep_interval) != 0 || event_add(signal_int, NULL) != 0 ||
        event_add(signal_term, NULL) != 0) {
        event_free(signal_int);
        event_free(signal_term);
        goto cleanup;
    }
    (void)event_base_dispatch(service.base);
    (void)fprintf(stderr, "nat_detect_hub event=stopping\n");
    event_free(signal_int);
    event_free(signal_term);
    result = EXIT_SUCCESS;

cleanup:
    while (service.sessions != NULL) {
        nat_detect_hub_session_destroy(service.sessions);
    }
    if (service.sweep != NULL) {
        event_free(service.sweep);
    }
    if (service.listener != NULL) {
        evconnlistener_free(service.listener);
    }
    utp_ntrs_hub_destroy(&service.hub);
    if (service.tls != NULL) {
        utp_ntrs_tls_context_free(service.tls);
    }
    if (service.base != NULL) {
        event_base_free(service.base);
    }
    return result;
}

int main(int argc, char** argv)
{
    CLI::App    cli{"NTRS NAT detection hub"};
    std::string listen;
    std::string interface_name;
    std::string certificate;
    std::string private_key;
    bool        use_ipv6 = false;

    cli.add_option("-l,--listen", listen, "TCP listen endpoint");
    cli.add_option("-i,--interface", interface_name, "Bind all service sockets to this interface");
    cli.add_option("-c,--cert", certificate, "TLS certificate file");
    cli.add_option("-k,--key", private_key, "TLS private key file");
    cli.add_flag("-6", use_ipv6, "Use IPv6 only and resolve hostnames with AAAA records");
    CLI11_PARSE(cli, argc, argv);

    if (listen.empty()) {
        listen = use_ipv6 ? "[::]:24000" : "0.0.0.0:24000";
    }

    utp_ntrs_app_log_init("nat_detect_hub");
    return nat_detect_hub_run(listen.c_str(), interface_name.empty() ? NULL : interface_name.c_str(),
                              certificate.empty() ? NULL : certificate.c_str(),
                              private_key.empty() ? NULL : private_key.c_str(), use_ipv6);
}
