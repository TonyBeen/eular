#include <assert.h>
#include <string.h>

#include <event2/event.h>
#include <utp/context.h>

#include "connection/connection.h"
#include "context/context.h"
#include "proto/frame.h"

#if defined(__APPLE__)
#include <net/if.h>
#endif

typedef struct public_api_probe {
    int32_t           new_connection_count;
    int32_t           connected_count;
    int32_t           connect_error_count;
    int32_t           connection_error_count;
    uint32_t          last_local_cid;
    uint32_t          last_peer_cid;
    uint32_t          last_connect_timeout_ms;
    int8_t            last_connect_retries;
    utp_status_t      last_connect_status;
    utp_status_t      last_connection_status;
    uint16_t          last_peer_error_code;
    size_t            last_reason_length;
    bool              last_peer_initiated;
    utp_connection_t* connected_connection;
} public_api_probe_t;

static bool test_on_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    public_api_probe_t* probe = user_data;

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

static void test_on_connected(utp_connection_t* connection, void* user_data)
{
    public_api_probe_t* probe = user_data;

    assert(connection != NULL);
    ++probe->connected_count;
    probe->connected_connection = connection;
}

static void test_on_connection_error(utp_connection_t* connection, const utp_connection_error_info_t* info,
                                     void* user_data)
{
    public_api_probe_t* probe = user_data;

    assert(connection != NULL);
    assert(info != NULL);
    assert(info->reason != NULL || info->reason_length == 0u);
    ++probe->connection_error_count;
    probe->last_connection_status = info->status;
    probe->last_peer_error_code   = info->peer_error_code;
    probe->last_reason_length     = info->reason_length;
    probe->last_peer_initiated    = info->peer_initiated;
}

static void test_on_connect_error(utp_status_t status, const char* message, const utp_connect_attempt_info_t* attempt,
                                  void* user_data)
{
    public_api_probe_t* probe = user_data;

    assert(message != NULL);
    assert(attempt != NULL);
    ++probe->connect_error_count;
    probe->last_connect_status     = status;
    probe->last_connect_timeout_ms = attempt->timeout_ms;
    probe->last_connect_retries    = attempt->retries;
}

static void pump_event_loop(struct event_base* event_base, int32_t iterations)
{
    int32_t index;

    for (index = 0; index < iterations; ++index) {
        (void)event_base_loop(event_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
    }
}

static void pump_event_loop_blocking(struct event_base* event_base, int32_t iterations)
{
    int32_t index;

    for (index = 0; index < iterations; ++index) {
        assert(event_base_loop(event_base, EVLOOP_ONCE) == 0);
    }
}

int main(void)
{
    struct event_base*    event_base = event_base_new();
    utp_context_options_t options    = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_t*        context    = NULL;

    assert(event_base != NULL);
    assert(options.mtu_probe_retries == 1u);
    options.event_base = event_base;
    options.context_id = 7u;
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
        utp_context_options_t interface_options = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_t*        interface_context = NULL;
        uint16_t              interface_port    = 0u;

        interface_options.event_base = event_base;
        interface_options.context_id = 8u;
        assert(utp_context_create(&interface_options, &interface_context) == UTP_STATUS_OK);
        assert(utp_context_bind(interface_context, "127.0.0.1", 0u, "lo0", &interface_port) == UTP_STATUS_OK);
        assert(interface_port != 0u);
        utp_context_destroy(interface_context);
    }
#endif
    {
        utp_context_options_t client_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_options_t server_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_t*        client          = NULL;
        utp_context_t*        server          = NULL;
        uint16_t              client_port     = 0u;
        uint16_t              server_port     = 0u;
        public_api_probe_t    client_probe    = {0};
        public_api_probe_t    server_probe    = {0};
        utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;

        client_options.event_base = event_base;
        client_options.context_id = 11u;
        server_options.event_base = event_base;
        server_options.context_id = 22u;
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
        utp_context_set_on_connection_error(client, test_on_connection_error, &client_probe);
        utp_context_set_on_connection_error(server, test_on_connection_error, &server_probe);

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
        assert(client_probe.connected_connection != NULL);
        assert(server_probe.connected_connection != NULL);

        {
            const uint8_t data[]      = "api";
            uint8_t       received[8] = {0u};
            uint32_t      stream_id   = UINT32_MAX;
            utp_stream_t* stream;
            size_t        received_length = 0u;

            assert(utp_connection_create_stream(client_probe.connected_connection, UTP_STREAM_TYPE_BIDIRECTIONAL,
                                                &stream_id) == UTP_STATUS_OK);
            stream = utp_connection_get_stream(client_probe.connected_connection, stream_id);
            assert(stream != NULL);
            assert(utp_stream_id(stream) == stream_id);
            assert(utp_stream_write(stream, data, sizeof(data) - 1u) == UTP_STATUS_OK);
            assert(utp_stream_set_priority(stream, UTP_STREAM_PRIORITY_HIGHEST) == UTP_STATUS_OK);
            assert(utp_stream_priority(stream) == UTP_STREAM_PRIORITY_HIGHEST);
            utp_stream_close(stream);
            pump_event_loop(event_base, 8);
            stream = utp_connection_get_stream(server_probe.connected_connection, stream_id);
            assert(stream != NULL);
            assert(utp_stream_read(stream, received, sizeof(received), &received_length) == UTP_STATUS_OK);
            assert(received_length == sizeof(data) - 1u);
            assert(memcmp(received, data, received_length) == 0);
            assert(utp_stream_read(stream, received, sizeof(received), &received_length) == UTP_STATUS_CLOSED);
            assert(received_length == 0u);
        }
        connect_options.encryption = UTP_ENCRYPTION_AES_GCM_128;
        assert(utp_context_connect(client, &connect_options) == UTP_STATUS_UNSUPPORTED);
        utp_context_destroy(client);
        client = NULL;
        pump_event_loop(event_base, 16);
        assert(server_probe.connection_error_count == 1);
        assert(server_probe.last_connection_status == UTP_STATUS_OK);
        assert(server_probe.last_peer_error_code == 0u);
        assert(server_probe.last_reason_length == 0u);
        assert(server_probe.last_peer_initiated);
        utp_context_destroy(server);
    }
    {
        utp_context_options_t client_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_options_t server_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_t*        client          = NULL;
        utp_context_t*        server          = NULL;
        uint16_t              client_port     = 0u;
        uint16_t              server_port     = 0u;
        public_api_probe_t    client_probe    = {0};
        public_api_probe_t    server_probe    = {0};
        utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;

        client_options.event_base = event_base;
        client_options.context_id = 23u;
        server_options.event_base = event_base;
        server_options.context_id = 24u;
        assert(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
        assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
        assert(utp_context_bind(client, "127.0.0.1", 0u, NULL, &client_port) == UTP_STATUS_OK);
        assert(utp_context_bind(server, "127.0.0.1", 0u, NULL, &server_port) == UTP_STATUS_OK);
        utp_context_set_on_connected(client, test_on_connected, &client_probe);
        utp_context_set_on_connected(server, test_on_connected, &server_probe);
        utp_context_set_on_new_connection(server, test_on_new_connection, &server_probe);
        utp_context_set_on_connection_error(server, test_on_connection_error, &server_probe);

        connect_options.address = "127.0.0.1";
        connect_options.port    = server_port;
        assert(utp_context_connect(client, &connect_options) == UTP_STATUS_OK);
        pump_event_loop(event_base, 8);
        assert(utp_context_accept(server) == UTP_STATUS_OK);
        pump_event_loop(event_base, 16);
        assert(client_probe.connected_connection != NULL);
        assert(server_probe.connected_connection != NULL);

        {
            const uint8_t      data[]                                               = {'x'};
            uint8_t            payload[UTP_FRAME_STREAM_HEADER_SIZE + sizeof(data)] = {0u};
            uint32_t           stream_id                                            = UINT32_MAX;
            utp_stream_t*      stream;
            utp_frame_stream_t invalid;

            assert(utp_connection_create_stream(server_probe.connected_connection, UTP_STREAM_TYPE_UNIDIRECTIONAL,
                                                &stream_id) == UTP_STATUS_OK);
            stream = utp_connection_get_stream(server_probe.connected_connection, stream_id);
            assert(stream != NULL);
            assert(utp_stream_write(stream, data, sizeof(data)) == UTP_STATUS_OK);
            utp_stream_close(stream);
            pump_event_loop(event_base, 8);
            assert(utp_connection_get_stream(client_probe.connected_connection, stream_id) != NULL);

            invalid.flags       = UTP_STREAM_FLAG_NONE;
            invalid.stream_id   = stream_id;
            invalid.offset      = 0u;
            invalid.data        = data;
            invalid.data_length = (uint16_t)sizeof(data);
            assert(utp_frame_stream_encode(payload, sizeof(payload), &invalid) == UTP_INTERNAL_ERROR_OK);
            assert(utp_connection_queue_packet(client_probe.connected_connection, UTP_PACKET_TYPE_CTRL, payload,
                                               sizeof(payload), false) == UTP_INTERNAL_ERROR_OK);
            assert(utp_context_flush_public_connection(client, client_probe.connected_connection) ==
                   UTP_INTERNAL_ERROR_OK);
        }
        pump_event_loop(event_base, 16);
        assert(server_probe.connection_error_count == 1);
        assert(server_probe.last_connection_status == UTP_STATUS_PROTOCOL);
        assert(server_probe.last_peer_error_code == 0u);
        assert(server_probe.last_reason_length != 0u);
        assert(!server_probe.last_peer_initiated);

        utp_context_destroy(client);
        utp_context_destroy(server);
    }
    {
        utp_context_options_t client_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_options_t server_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_t*        client          = NULL;
        utp_context_t*        server          = NULL;
        uint16_t              client_port     = 0u;
        uint16_t              server_port     = 0u;
        public_api_probe_t    client_probe    = {0};
        utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;

        client_options.event_base = event_base;
        client_options.context_id = 31u;
        server_options.event_base = event_base;
        server_options.context_id = 32u;
        assert(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
        assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
        assert(utp_context_bind(client, "127.0.0.1", 0u, NULL, &client_port) == UTP_STATUS_OK);
        assert(utp_context_bind(server, "127.0.0.1", 0u, NULL, &server_port) == UTP_STATUS_OK);
        assert(client_port != 0u);
        assert(server_port != 0u);
        utp_context_set_on_connect_error(client, test_on_connect_error, &client_probe);

        connect_options.address    = "127.0.0.1";
        connect_options.port       = server_port;
        connect_options.timeout_ms = 1u;
        connect_options.retries    = 1;
        assert(utp_context_connect(client, &connect_options) == UTP_STATUS_OK);
        pump_event_loop_blocking(event_base, 4);
        assert(client_probe.connect_error_count == 1);
        assert(client_probe.last_connect_status == UTP_STATUS_TIMEOUT);
        assert(client_probe.last_connect_timeout_ms == 1u);
        assert(client_probe.last_connect_retries == 1);
        utp_context_destroy(server);
        utp_context_destroy(client);
    }
    event_base_free(event_base);
    assert(strcmp(utp_version(), UTP_VERSION_STRING) == 0);
    assert(strcmp(utp_status_string(UTP_STATUS_PROTOCOL), "protocol") == 0);
    return 0;
}
