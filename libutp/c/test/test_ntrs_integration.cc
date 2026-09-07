#define CATCH_CONFIG_MAIN

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cerrno>

#include <arpa/inet.h>
#include <catch2/catch.hpp>
#include <event2/event.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utp/context.h>

typedef struct integration_probe {
    utp_context_t*    context;
    utp_connection_t* connection;
    utp_stream_t*     incoming_stream;
    int32_t           registered_count;
    int32_t           connected_count;
    int32_t           connect_error_count;
} integration_probe_t;

typedef struct session_token_probe {
    std::array<uint8_t, 256u> token;
    size_t                    length;
    int32_t                   ready_count;
} session_token_probe_t;

static void     test_on_incoming_stream(utp_connection_t* connection, utp_stream_t* stream, void* user_data);

static uint16_t test_find_udp_port(void)
{
    struct sockaddr_in address        = {};
    socklen_t          address_length = sizeof(address);
    const int          fd             = socket(AF_INET, SOCK_DGRAM, 0);
    uint16_t           port;

    REQUIRE(fd >= 0);
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    REQUIRE(bind(fd, (const struct sockaddr*)&address, sizeof(address)) == 0);
    REQUIRE(getsockname(fd, (struct sockaddr*)&address, &address_length) == 0);
    REQUIRE(address_length == sizeof(address));
    port = ntohs(address.sin_port);
    REQUIRE(port != 0u);
    REQUIRE(close(fd) == 0);
    return port;
}

static void test_on_connected(utp_connection_t* connection, void* user_data)
{
    integration_probe_t* probe = static_cast<integration_probe_t*>(user_data);

    REQUIRE(connection != NULL);
    REQUIRE(probe != NULL);
    ++probe->connected_count;
    probe->connection = connection;
    utp_connection_set_on_incoming_stream(connection, test_on_incoming_stream, probe);
}

static void test_on_connect_error(utp_status_t status, const char* message, const utp_connect_attempt_info_t* attempt,
                                  void* user_data)
{
    integration_probe_t* probe = static_cast<integration_probe_t*>(user_data);

    REQUIRE(status != UTP_STATUS_OK);
    REQUIRE(message != NULL);
    REQUIRE(attempt != NULL);
    REQUIRE(probe != NULL);
    ++probe->connect_error_count;
}

static bool test_on_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    integration_probe_t* probe = static_cast<integration_probe_t*>(user_data);

    REQUIRE(info != NULL);
    REQUIRE(probe != NULL);
    return utp_context_accept(probe->context) == UTP_STATUS_OK;
}

static void test_on_incoming_stream(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    integration_probe_t* probe = static_cast<integration_probe_t*>(user_data);

    REQUIRE(connection != NULL);
    REQUIRE(stream != NULL);
    REQUIRE(probe != NULL);
    probe->incoming_stream = stream;
}

static void test_on_session_token_ready(utp_connection_t* connection, void* user_data)
{
    session_token_probe_t* probe = static_cast<session_token_probe_t*>(user_data);

    REQUIRE(connection != NULL);
    REQUIRE(probe != NULL);
    ++probe->ready_count;
    REQUIRE(utp_connection_export_session_token(connection, probe->token.data(), probe->token.size(), &probe->length) ==
            UTP_STATUS_OK);
}

static void test_on_ntrs_registered(utp_context_t* context, const utp_ntrs_registered_info_t* info, void* user_data)
{
    integration_probe_t* probe = static_cast<integration_probe_t*>(user_data);

    REQUIRE(context != NULL);
    REQUIRE(info != NULL);
    REQUIRE(info->peer_id != NULL);
    REQUIRE(strcmp(info->peer_id, "node-b") == 0);
    REQUIRE(probe != NULL);
    ++probe->registered_count;
}

template <typename Predicate>
static void test_pump_until(struct event_base* event_base, uint32_t timeout_ms, Predicate complete)
{
    const struct timeval delay = {0, 1000};
    uint32_t             elapsed_ms;

    for (elapsed_ms = 0u; elapsed_ms < timeout_ms; ++elapsed_ms) {
        if (complete()) {
            return;
        }
        REQUIRE(event_base_loopexit(event_base, &delay) == 0);
        REQUIRE(event_base_loop(event_base, EVLOOP_ONCE) == 0);
    }
    REQUIRE(complete());
}

static pid_t test_start_ntrs(const char* executable, uint16_t port)
{
    char  port_text[6u];
    pid_t pid;

    REQUIRE(executable != NULL);
    REQUIRE(snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port) > 0);
    pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        execl(executable, executable, "-a", "127.0.0.1", "-e", "127.0.0.1", "-p", port_text, "-t", "3000", "-k", "1000",
              (char*)NULL);
        _exit(127);
    }
    return pid;
}

class ntrs_process
{
public:
    ntrs_process(const char* executable, uint16_t port) : pid_(test_start_ntrs(executable, port)) {}

    ~ntrs_process()
    {
        int status;

        if (pid_ <= 0) {
            return;
        }
        (void)kill(pid_, SIGTERM);
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }

    void stop()
    {
        int status = 0;

        REQUIRE(pid_ > 0);
        REQUIRE(kill(pid_, SIGTERM) == 0);
        REQUIRE(waitpid(pid_, &status, 0) == pid_);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
        pid_ = -1;
    }

private:
    pid_t pid_;
};

TEST_CASE("NTRS process completes Context rendezvous", "[ntrs][integration]")
{
    const uint16_t              ntrs_port = test_find_udp_port();
    const char*                 ntrs_executable;
    struct event_base*          event_base;
    utp_context_options_t       client_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t       server_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_ntrs_register_options_t register_options = UTP_NTRS_REGISTER_OPTIONS_INIT;
    utp_connect_options_t       connect_options  = UTP_CONNECT_OPTIONS_INIT;
    utp_context_t*              client           = NULL;
    utp_context_t*              server           = NULL;
    integration_probe_t         client_probe     = {};
    integration_probe_t         server_probe     = {};

    ntrs_executable = getenv("UTP_C_NTRS_EXECUTABLE");
    REQUIRE(ntrs_executable != NULL);
    REQUIRE(access(ntrs_executable, X_OK) == 0);
    event_base = event_base_new();
    REQUIRE(event_base != NULL);
    client_options.event_base = event_base;
    client_options.context_id = 701u;
    client_options.peer_id    = "node-a";
    server_options.event_base = event_base;
    server_options.context_id = 702u;
    server_options.peer_id    = "node-b";
    REQUIRE(utp_context_create(&client_options, &client) == UTP_STATUS_OK);
    REQUIRE(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(server, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    client_probe.context = client;
    server_probe.context = server;
    utp_context_set_on_connected(client, test_on_connected, &client_probe);
    utp_context_set_on_connected(server, test_on_connected, &server_probe);
    utp_context_set_on_connect_error(client, test_on_connect_error, &client_probe);
    utp_context_set_on_new_connection(server, test_on_new_connection, &server_probe);

    ntrs_process ntrs(ntrs_executable, ntrs_port);
    register_options.ntrs_address          = "127.0.0.1";
    register_options.ntrs_port             = ntrs_port;
    register_options.timeout_ms            = 200u;
    register_options.retries               = 10u;
    register_options.keepalive_interval_ms = 1000u;
    register_options.keepalive_timeout_ms  = 500u;
    REQUIRE(utp_context_register_ntrs(server, &register_options, test_on_ntrs_registered, &server_probe) ==
            UTP_STATUS_OK);
    test_pump_until(event_base, 2000u, [&server_probe]() { return server_probe.registered_count == 1; });
    REQUIRE(server_probe.registered_count == 1);

    connect_options.address        = "127.0.0.1";
    connect_options.target_peer_id = "node-b";
    connect_options.port           = ntrs_port;
    connect_options.timeout_ms     = 1000u;
    connect_options.retries        = 2;
    connect_options.encryption     = UTP_ENCRYPTION_NONE;
    REQUIRE(utp_context_connect(client, &connect_options) == UTP_STATUS_OK);
    test_pump_until(event_base, 3000u, [&client_probe, &server_probe]() {
        return client_probe.connected_count == 1 && server_probe.connected_count == 1;
    });
    REQUIRE(client_probe.connect_error_count == 0);
    REQUIRE(client_probe.connected_count == 1);
    REQUIRE(server_probe.connected_count == 1);

    utp_context_destroy(client);
    utp_context_destroy(server);
    event_base_free(event_base);
    ntrs.stop();
}

TEST_CASE("NTRS process carries 0-RTT early data through rendezvous", "[ntrs][integration][0rtt]")
{
    const uint16_t                         ntrs_port = test_find_udp_port();
    const char*                            ntrs_executable;
    const std::array<uint8_t, 15u>         early_data = {'n', 't', 'r', 's', '-', 'e', 'a', 'r',
                                                         'l', 'y', '-', 'd', 'a', 't', 'a'};
    struct event_base*                     event_base;
    utp_context_options_t                  first_options    = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t                  early_options    = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t                  server_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_ntrs_register_options_t            register_options = UTP_NTRS_REGISTER_OPTIONS_INIT;
    utp_connect_options_t                  first_connect    = UTP_CONNECT_OPTIONS_INIT;
    utp_connect_0rtt_options_t             early_connect    = UTP_CONNECT_0RTT_OPTIONS_INIT;
    utp_context_t*                         first_client     = NULL;
    utp_context_t*                         early_client     = NULL;
    utp_context_t*                         server           = NULL;
    integration_probe_t                    first_probe      = {};
    integration_probe_t                    early_probe      = {};
    integration_probe_t                    server_probe     = {};
    session_token_probe_t                  token_probe      = {};
    uint16_t                               server_port      = 0u;
    std::array<uint8_t, early_data.size()> received         = {};
    size_t                                 received_length  = 0u;

    ntrs_executable = getenv("UTP_C_NTRS_EXECUTABLE");
    REQUIRE(ntrs_executable != NULL);
    REQUIRE(access(ntrs_executable, X_OK) == 0);
    event_base = event_base_new();
    REQUIRE(event_base != NULL);
    first_options.event_base  = event_base;
    first_options.context_id  = 711u;
    first_options.peer_id     = "node-a";
    early_options.event_base  = event_base;
    early_options.context_id  = 712u;
    early_options.peer_id     = "node-a";
    server_options.event_base = event_base;
    server_options.context_id = 713u;
    server_options.peer_id    = "node-b";
    REQUIRE(utp_context_create(&first_options, &first_client) == UTP_STATUS_OK);
    REQUIRE(utp_context_create(&server_options, &server) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(first_client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(server, "127.0.0.1", 0u, NULL, &server_port) == UTP_STATUS_OK);
    first_probe.context  = first_client;
    server_probe.context = server;
    utp_context_set_on_connected(first_client, test_on_connected, &first_probe);
    utp_context_set_on_connected(server, test_on_connected, &server_probe);
    utp_context_set_on_connect_error(first_client, test_on_connect_error, &first_probe);
    utp_context_set_on_new_connection(server, test_on_new_connection, &server_probe);

    ntrs_process ntrs(ntrs_executable, ntrs_port);
    register_options.ntrs_address          = "127.0.0.1";
    register_options.ntrs_port             = ntrs_port;
    register_options.timeout_ms            = 200u;
    register_options.retries               = 10u;
    register_options.keepalive_interval_ms = 1000u;
    register_options.keepalive_timeout_ms  = 500u;
    REQUIRE(utp_context_register_ntrs(server, &register_options, test_on_ntrs_registered, &server_probe) ==
            UTP_STATUS_OK);
    test_pump_until(event_base, 2000u, [&server_probe]() { return server_probe.registered_count == 1; });

    first_connect.address        = "127.0.0.1";
    first_connect.target_peer_id = "node-b";
    first_connect.port           = server_port;
    first_connect.timeout_ms     = 1000u;
    first_connect.retries        = 2;
    first_connect.encryption     = UTP_ENCRYPTION_NONE;
    REQUIRE(utp_context_connect(first_client, &first_connect) == UTP_STATUS_OK);
    test_pump_until(event_base, 2000u, [&first_probe, &server_probe]() {
        return first_probe.connected_count == 1 && server_probe.connected_count == 1;
    });
    utp_connection_set_on_session_token_ready(first_probe.connection, test_on_session_token_ready, &token_probe);
    test_pump_until(event_base, 2000u, [&token_probe]() { return token_probe.ready_count == 1; });
    REQUIRE(token_probe.length > 0u);

    REQUIRE(utp_context_create(&early_options, &early_client) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(early_client, "127.0.0.1", 0u, NULL, NULL) == UTP_STATUS_OK);
    early_probe.context = early_client;
    utp_context_set_on_connected(early_client, test_on_connected, &early_probe);
    utp_context_set_on_connect_error(early_client, test_on_connect_error, &early_probe);
    early_connect.address            = "127.0.0.1";
    early_connect.target_peer_id     = "node-b";
    early_connect.port               = ntrs_port;
    early_connect.timeout_ms         = 1000u;
    early_connect.retries            = 2;
    early_connect.session_token      = token_probe.token.data();
    early_connect.session_token_size = token_probe.length;
    early_connect.early_data         = early_data.data();
    early_connect.early_data_size    = early_data.size();
    early_connect.early_fin          = true;
    REQUIRE(utp_context_connect_0rtt(early_client, &early_connect) == UTP_STATUS_OK);
    test_pump_until(event_base, 3000u, [&early_probe, &server_probe]() {
        return early_probe.connected_count == 1 && server_probe.connected_count == 2;
    });
    test_pump_until(event_base, 2000u, [&server_probe, &early_data]() {
        return server_probe.incoming_stream != NULL &&
               utp_stream_readable_bytes(server_probe.incoming_stream) == early_data.size();
    });
    REQUIRE(utp_stream_read(server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == early_data.size());
    REQUIRE(received == early_data);
    REQUIRE(early_probe.connect_error_count == 0);

    utp_context_destroy(early_client);
    utp_context_destroy(first_client);
    utp_context_destroy(server);
    event_base_free(event_base);
    ntrs.stop();
}
