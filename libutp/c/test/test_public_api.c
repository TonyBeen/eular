#include <assert.h>
#include <stdio.h>
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
    int32_t               new_connection_count;
    int32_t               connected_count;
    int32_t               connect_error_count;
    int32_t               connection_error_count;
    uint32_t              last_local_cid;
    uint32_t              last_peer_cid;
    uint32_t              last_connect_timeout_ms;
    int8_t                last_connect_retries;
    utp_status_t          last_connect_status;
    utp_status_t          last_connection_status;
    uint16_t              last_peer_error_code;
    size_t                last_reason_length;
    bool                  last_peer_initiated;
    utp_encryption_mode_t expected_encryption;
    utp_context_t*        context;
    utp_connection_t*     connected_connection;
} public_api_probe_t;

static int32_t         test_log_count;
static utp_log_level_t test_log_level;
static char            test_log_message[UTP_LOG_MESSAGE_MAX_LENGTH + 1u];

static void            test_log_sink(utp_log_level_t level, const char* message)
{
    assert(message != NULL);
    ++test_log_count;
    test_log_level = level;
    (void)snprintf(test_log_message, sizeof(test_log_message), "%s", message);
}

static bool test_on_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    public_api_probe_t* probe = user_data;

    assert(info != NULL);
    assert(info->remote.family == 4u);
    assert(info->remote.port != 0u);
    assert(info->local_cid != 0u);
    assert(info->peer_cid != 0u);
    assert(info->encryption == probe->expected_encryption);
    ++probe->new_connection_count;
    probe->last_local_cid = info->local_cid;
    probe->last_peer_cid  = info->peer_cid;
    assert(probe->context != NULL);
    return utp_context_accept(probe->context) == UTP_STATUS_OK;
}

static bool test_on_new_connection_without_accept(const utp_new_connection_info_t* info, void* user_data)
{
    public_api_probe_t* probe = user_data;

    assert(info != NULL);
    ++probe->new_connection_count;
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

static void test_encrypted_connection(struct event_base* event_base, utp_encryption_mode_t encryption,
                                      uint64_t client_context_id, uint64_t server_context_id)
{
    utp_context_options_t client_options         = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t server_options         = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_t*        client                 = NULL;
    utp_context_t*        server                 = NULL;
    uint16_t              client_port            = 0u;
    uint16_t              server_port            = 0u;
    public_api_probe_t    client_probe           = {0};
    public_api_probe_t    server_probe           = {0};
    utp_connect_options_t connect_options        = UTP_CONNECT_OPTIONS_INIT;
    const uint8_t         data[]                 = "encrypted stream";
    uint8_t               resumption_state[166u] = {0u};
    uint8_t               received[32]           = {0u};
    uint32_t              stream_id              = UINT32_MAX;
    utp_stream_t*         stream;
    size_t                received_length = 0u;

    client_options.event_base        = event_base;
    client_options.context_id        = client_context_id;
    server_options.event_base        = event_base;
    server_options.context_id        = server_context_id;
    server_probe.expected_encryption = encryption;
    assert(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
    assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
    server_probe.context = server;
    assert(utp_context_bind(client, "127.0.0.1", 0u, NULL, &client_port) == UTP_STATUS_OK);
    assert(utp_context_bind(server, "127.0.0.1", 0u, NULL, &server_port) == UTP_STATUS_OK);
    utp_context_set_on_connected(client, test_on_connected, &client_probe);
    utp_context_set_on_connected(server, test_on_connected, &server_probe);
    utp_context_set_on_new_connection(server, test_on_new_connection, &server_probe);
    utp_context_set_on_connect_error(client, test_on_connect_error, &client_probe);
    utp_context_set_on_connection_error(client, test_on_connection_error, &client_probe);
    utp_context_set_on_connection_error(server, test_on_connection_error, &server_probe);

    connect_options.address    = "127.0.0.1";
    connect_options.port       = server_port;
    connect_options.encryption = encryption;
    assert(utp_context_connect(client, &connect_options) == UTP_STATUS_OK);
    pump_event_loop(event_base, 8);
    assert(server_probe.new_connection_count == 1);
    pump_event_loop(event_base, 16);
    assert(client_probe.connect_error_count == 0);
    assert(client_probe.connected_count == 1);
    assert(server_probe.connected_count == 1);
    assert(client_probe.connected_connection->crypto_ready);
    assert(server_probe.connected_connection->crypto_ready);
    assert(server_probe.connected_connection->send_control.current_packet_number >= 2u);
    assert(utp_connection_export_session_token(client_probe.connected_connection, resumption_state,
                                               sizeof(resumption_state), &received_length) == UTP_STATUS_OK);
    assert(received_length == sizeof(resumption_state));
    assert(utp_connection_create_stream(client_probe.connected_connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
           UTP_STATUS_OK);
    stream = utp_connection_get_stream(client_probe.connected_connection, stream_id);
    assert(stream != NULL);
    assert(utp_stream_write(stream, data, sizeof(data) - 1u) == UTP_STATUS_OK);
    utp_stream_close(stream);
    pump_event_loop(event_base, 12);
    stream = utp_connection_get_stream(server_probe.connected_connection, stream_id);
    assert(stream != NULL);
    assert(utp_stream_read(stream, received, sizeof(received), &received_length) == UTP_STATUS_OK);
    assert(received_length == sizeof(data) - 1u);
    assert(memcmp(received, data, received_length) == 0);
    assert(utp_stream_read(stream, received, sizeof(received), &received_length) == UTP_STATUS_CLOSED);
    utp_context_destroy(client);
    pump_event_loop(event_base, 16);
    assert(server_probe.connection_error_count == 1);
    assert(server_probe.last_connection_status == UTP_STATUS_OK);
    assert(server_probe.last_peer_initiated);
    utp_context_destroy(server);
}

static void test_plaintext_zero_rtt(struct event_base* event_base)
{
    utp_context_options_t      server_options  = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t      first_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t      early_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_options_t      connect         = UTP_CONNECT_OPTIONS_INIT;
    utp_connect_0rtt_options_t early           = UTP_CONNECT_0RTT_OPTIONS_INIT;
    utp_context_t*             server          = NULL;
    utp_context_t*             first_client    = NULL;
    utp_context_t*             early_client    = NULL;
    utp_context_t*             replay_client   = NULL;
    public_api_probe_t         server_probe    = {0};
    public_api_probe_t         first_probe     = {0};
    public_api_probe_t         early_probe     = {0};
    uint8_t                    token[166u]     = {0u};
    uint8_t                    received[16u]   = {0u};
    const uint8_t              early_data[]    = "early";
    uint16_t                   port            = 0u;
    size_t                     token_length    = 0u;
    size_t                     received_length = 0u;
    utp_stream_t*              stream;

    server_options.event_base                     = event_base;
    server_options.context_id                     = 201u;
    server_options.zero_rtt_replay_cache_capacity = 1u;
    first_options.event_base                      = event_base;
    first_options.context_id                      = 202u;
    early_options.event_base                      = event_base;
    early_options.context_id                      = 203u;
    server_probe.expected_encryption              = UTP_ENCRYPTION_NONE;
    assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
    assert(utp_context_create(&first_options, &first_client) == UTP_STATUS_OK);
    server_probe.context = server;
    assert(utp_context_bind(server, "127.0.0.1", 0u, NULL, &port) == UTP_STATUS_OK);
    assert(utp_context_bind(first_client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    utp_context_set_on_new_connection(server, test_on_new_connection, &server_probe);
    utp_context_set_on_connected(server, test_on_connected, &server_probe);
    utp_context_set_on_connected(first_client, test_on_connected, &first_probe);
    connect.address = "127.0.0.1";
    connect.port    = port;
    assert(utp_context_connect(first_client, &connect) == UTP_STATUS_OK);
    pump_event_loop(event_base, 8);
    pump_event_loop(event_base, 20);
    assert(first_probe.connected_connection != NULL);
    assert(utp_connection_export_session_token(first_probe.connected_connection, token, sizeof(token), &token_length) ==
           UTP_STATUS_OK);
    assert(token_length == sizeof(token));

    assert(utp_context_create(&early_options, &early_client) == UTP_STATUS_OK);
    assert(utp_context_bind(early_client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    utp_context_set_on_connected(early_client, test_on_connected, &early_probe);
    early.address            = "127.0.0.1";
    early.port               = port;
    early.session_token      = token;
    early.session_token_size = token_length;
    early.early_data         = early_data;
    early.early_data_size    = sizeof(early_data) - 1u;
    early.early_fin          = true;
    assert(utp_context_connect_0rtt(early_client, &early) == UTP_STATUS_OK);
    pump_event_loop(event_base, 24);
    assert(early_probe.connected_connection != NULL);
    assert(utp_connection_get_stream(early_probe.connected_connection, 0u) != NULL);
    assert(server_probe.new_connection_count == 2);
    stream = utp_connection_get_stream(server_probe.connected_connection, 0u);
    assert(stream != NULL);
    assert(utp_stream_read(stream, received, sizeof(received), &received_length) == UTP_STATUS_OK);
    assert(received_length == sizeof(early_data) - 1u);
    assert(memcmp(received, early_data, received_length) == 0);
    assert(utp_stream_read(stream, received, sizeof(received), &received_length) == UTP_STATUS_CLOSED);
    assert(utp_context_create(&early_options, &replay_client) == UTP_STATUS_OK);
    assert(utp_context_bind(replay_client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    assert(utp_context_connect_0rtt(replay_client, &early) == UTP_STATUS_OK);
    pump_event_loop(event_base, 8);
    assert(server_probe.new_connection_count == 2);
    {
        uint8_t replacement_root[32u] = {1u};

        utp_context_set_resumption_key(first_client, replacement_root);
        assert(utp_connection_export_session_token(first_probe.connected_connection, token, sizeof(token),
                                                   &token_length) == UTP_STATUS_CONNECTION_SESSION_TOKEN_UNAVAILABLE);
    }
    utp_context_destroy(replay_client);
    utp_context_destroy(early_client);
    utp_context_destroy(first_client);
    utp_context_destroy(server);
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
    options.log_sink   = test_log_sink;
    options.log_level  = UTP_LOG_LEVEL_INFO;
    test_log_count     = 0;
    assert(utp_context_create(&options, &context) == UTP_STATUS_OK);
    assert(context != NULL);
    assert(test_log_count == 1);
    assert(test_log_level == UTP_LOG_LEVEL_INFO);
    assert(strstr(test_log_message, "context created") != NULL);
    {
        uint16_t local_port = 0u;

        assert(utp_context_bind(context, "127.0.0.1", 0u, NULL, &local_port) == UTP_STATUS_OK);
        assert(local_port != 0u);
        assert(test_log_count == 2);
        assert(strstr(test_log_message, "udp socket bound") != NULL);
        assert(utp_context_bind(context, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_SOCKET_OPEN);
    }
    utp_context_destroy(context);
    assert(test_log_count == 3);
    assert(strstr(test_log_message, "context destroy started") != NULL);
    {
        utp_context_options_t quiet_options = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_t*        quiet_context = NULL;

        quiet_options.event_base                     = event_base;
        quiet_options.context_id                     = 71u;
        quiet_options.log_sink                       = test_log_sink;
        quiet_options.log_level                      = UTP_LOG_LEVEL_WARNING;
        quiet_options.zero_rtt_replay_cache_capacity = 8192u;
        test_log_count                               = 0;
        assert(utp_context_create(&quiet_options, &quiet_context) == UTP_STATUS_OK);
        assert(quiet_context->zero_rtt_replay_cache_capacity == 8192u);
        assert(quiet_context->zero_rtt_replay.max_entries == 8192u);
        utp_context_destroy(quiet_context);
        assert(test_log_count == 0);
        quiet_options.log_level = (utp_log_level_t)99;
        assert(utp_context_create(&quiet_options, &quiet_context) == UTP_STATUS_INVALID_ARGUMENT);
    }
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
        server_probe.context = server;
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
    test_encrypted_connection(event_base, UTP_ENCRYPTION_AES_GCM_128, 101u, 102u);
    test_encrypted_connection(event_base, UTP_ENCRYPTION_AES_GCM_256, 103u, 104u);
    test_plaintext_zero_rtt(event_base);
    {
        utp_context_options_t client_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_context_options_t server_options  = UTP_CONTEXT_OPTIONS_INIT;
        utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;
        utp_context_t*        client          = NULL;
        utp_context_t*        server          = NULL;
        public_api_probe_t    server_probe    = {0};
        uint16_t              server_port     = 0u;

        client_options.event_base = event_base;
        client_options.context_id = 301u;
        server_options.event_base = event_base;
        server_options.context_id = 302u;
        assert(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
        assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
        assert(utp_context_bind(client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
        assert(utp_context_bind(server, "127.0.0.1", 0u, NULL, &server_port) == UTP_STATUS_OK);
        utp_context_set_on_new_connection(server, test_on_new_connection_without_accept, &server_probe);
        connect_options.address = "127.0.0.1";
        connect_options.port    = server_port;
        assert(utp_context_connect(client, &connect_options) == UTP_STATUS_OK);
        pump_event_loop(event_base, 8);
        assert(server_probe.new_connection_count == 1);
        assert(utp_context_accept(server) == UTP_STATUS_WOULD_BLOCK);
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
        public_api_probe_t    server_probe    = {0};
        utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;

        client_options.event_base = event_base;
        client_options.context_id = 23u;
        server_options.event_base = event_base;
        server_options.context_id = 24u;
        assert(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
        assert(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
        server_probe.context = server;
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
