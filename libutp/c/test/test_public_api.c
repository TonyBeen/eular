#include <assert.h>
#include <event2/event.h>
#include <string.h>
#include <utp/utp.h>

#if defined(__APPLE__)
#include <net/if.h>
#endif

typedef struct public_api_probe {
    int32_t  new_connection_count;
    int32_t  connected_count;
    uint32_t last_local_cid;
    uint32_t last_peer_cid;
} public_api_probe_t;

static bool test_on_new_connection(const utp_new_connection_info_t *info, void *user_data) {
    public_api_probe_t *probe = user_data;

    assert(info != NULL);
    assert(info->remote.family == 4u);
    assert(info->remote.port != 0u);
    assert(info->local_cid != 0u);
    assert(info->peer_cid != 0u);
    assert(info->encryption == UTP_ENCRYPTION_NONE);
    ++probe->new_connection_count;
    probe->last_local_cid = info->local_cid;
    probe->last_peer_cid  = info->peer_cid;
    return true;
}

static void test_on_connected(utp_connection_t *connection, void *user_data) {
    public_api_probe_t *probe = user_data;

    assert(connection != NULL);
    ++probe->connected_count;
}

static void pump_event_loop(struct event_base *event_base, int32_t iterations) {
    int32_t index;

    for (index = 0; index < iterations; ++index) {
        (void)event_base_loop(event_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
    }
}

int main(void) {
    struct event_base    *event_base = event_base_new();
    utp_context_options_t options    = {event_base, NULL, 7u};
    utp_context_t        *context    = NULL;

    assert(event_base != NULL);
    assert(utp_context_create(&options, &context) == UTP_STATUS_OK);
    assert(context != NULL);
    {
        uint16_t local_port = 0u;

        assert(utp_context_bind(context, "127.0.0.1", 0u, NULL, &local_port) == UTP_STATUS_OK);
        assert(local_port != 0u);
        assert(utp_context_bind(context, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_SOCKET_OPEN);
    }
    utp_context_destroy(context);
#if defined(__APPLE__)
    if (if_nametoindex("lo0") != 0u) {
        utp_context_options_t interface_options = {event_base, NULL, 8u};
        utp_context_t        *interface_context = NULL;
        uint16_t              interface_port    = 0u;

        assert(utp_context_create(&interface_options, &interface_context) == UTP_STATUS_OK);
        assert(utp_context_bind(interface_context, "127.0.0.1", 0u, "lo0", &interface_port) == UTP_STATUS_OK);
        assert(interface_port != 0u);
        utp_context_destroy(interface_context);
    }
#endif
    {
        utp_context_options_t client_options = {event_base, NULL, 11u};
        utp_context_options_t server_options = {event_base, NULL, 22u};
        utp_context_t        *client         = NULL;
        utp_context_t        *server         = NULL;
        uint16_t              client_port    = 0u;
        uint16_t              server_port    = 0u;
        public_api_probe_t    client_probe   = {0, 0, 0u, 0u};
        public_api_probe_t    server_probe   = {0, 0, 0u, 0u};
        utp_connect_options_t connect_options;

        assert(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
        assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
        assert(utp_context_accept(server) == UTP_STATUS_SOCKET_NOT_BOUND);
        assert(utp_context_bind(client, "127.0.0.1", 0u, NULL, &client_port) == UTP_STATUS_OK);
        assert(utp_context_bind(server, "127.0.0.1", 0u, "", &server_port) == UTP_STATUS_OK);
        assert(client_port != 0u);
        assert(server_port != 0u);
        assert(utp_context_accept(server) == UTP_STATUS_WOULD_BLOCK);
        utp_context_set_on_connected(client, test_on_connected, &client_probe);
        utp_context_set_on_connected(server, test_on_connected, &server_probe);
        utp_context_set_on_new_connection(server, test_on_new_connection, &server_probe);

        memset(&connect_options, 0, sizeof(connect_options));
        connect_options.address    = "127.0.0.1";
        connect_options.port       = server_port;
        connect_options.timeout_ms = 3000u;
        connect_options.encryption = UTP_ENCRYPTION_NONE;
        assert(utp_context_connect(client, &connect_options) == UTP_STATUS_OK);
        pump_event_loop(event_base, 8);
        assert(server_probe.new_connection_count == 1);
        assert(server_probe.last_local_cid != 0u);
        assert(server_probe.last_peer_cid != 0u);
        assert(utp_context_accept(server) == UTP_STATUS_OK);
        pump_event_loop(event_base, 16);
        assert(client_probe.connected_count == 1);
        assert(server_probe.connected_count == 1);

        connect_options.encryption = UTP_ENCRYPTION_AES_GCM_128;
        assert(utp_context_connect(client, &connect_options) == UTP_STATUS_UNSUPPORTED);
        utp_context_destroy(server);
        utp_context_destroy(client);
    }
    event_base_free(event_base);
    assert(strcmp(utp_version(), UTP_VERSION_STRING) == 0);
    assert(strcmp(utp_status_string(UTP_STATUS_PROTOCOL), "protocol") == 0);
    return 0;
}
