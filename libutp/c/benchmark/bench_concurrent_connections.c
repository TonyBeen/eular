#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <utp/context.h>

typedef struct concurrent_connection_test concurrent_connection_test_t;

typedef struct concurrent_connection_client {
    concurrent_connection_test_t* test;        // 所属测试状态，不拥有
    utp_context_t*                context;     // 客户端 Context 所有者
    utp_connection_t*             connection;  // 已连接的 Connection，Context 借用
    struct event*                 hold_event;  // 建连后的保持定时器
    bool                          connected;   // 是否已完成握手
} concurrent_connection_client_t;

struct concurrent_connection_test {
    struct event_base*              event_base;         // 测试使用的单个事件循环
    utp_context_t*                  server;             // 服务端 Context 所有者
    struct event*                   timeout_event;      // 全局超时定时器
    concurrent_connection_client_t* clients;            // 客户端数组所有者
    uint32_t                        connection_count;   // 目标并发连接数
    uint32_t                        hold_ms;            // 建连后保持时间
    uint32_t                        accepted_count;     // 服务端已接受连接数
    uint32_t                        server_connected;   // 服务端已完成握手数
    uint32_t                        client_connected;   // 客户端已完成握手数
    uint32_t                        held_count;         // 已完成保持的客户端数
    uint32_t                        connect_errors;     // 主动建连错误数
    uint32_t                        connection_errors;  // 已建立连接错误数
    bool                            timed_out;          // 是否触发总超时
    utp_encryption_mode_t           encryption;         // 本轮握手加密方式
    uint64_t                        started_ms;         // 测试开始时刻
};

static uint64_t concurrent_connection_now_ms(void)
{
    struct timeval now;

    if (evutil_gettimeofday(&now, NULL) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_usec / UINT64_C(1000);
}

static bool concurrent_connection_parse_u32(const char* text, uint32_t* value)
{
    char*              end = NULL;
    unsigned long long parsed;

    if (text == NULL || value == NULL || *text == '\0') {
        return false;
    }
    errno  = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool concurrent_connection_parse_encryption(const char* text, utp_encryption_mode_t* encryption)
{
    if (strcmp(text, "none") == 0) {
        *encryption = UTP_ENCRYPTION_NONE;
        return true;
    }
    if (strcmp(text, "aes128") == 0) {
        *encryption = UTP_ENCRYPTION_AES_GCM_128;
        return true;
    }
    if (strcmp(text, "aes256") == 0) {
        *encryption = UTP_ENCRYPTION_AES_GCM_256;
        return true;
    }
    return false;
}

static void concurrent_connection_stop(concurrent_connection_test_t* test)
{
    if (test != NULL && test->event_base != NULL) {
        event_base_loopbreak(test->event_base);
    }
}

static bool concurrent_connection_on_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    concurrent_connection_test_t* test = user_data;

    if (test == NULL || info == NULL || utp_context_accept(test->server) != UTP_STATUS_OK) {
        if (test != NULL) {
            ++test->connect_errors;
            concurrent_connection_stop(test);
        }
        return false;
    }
    ++test->accepted_count;
    return true;
}

static void concurrent_connection_on_server_connected(utp_connection_t* connection, void* user_data)
{
    concurrent_connection_test_t* test = user_data;

    (void)connection;
    if (test != NULL) {
        ++test->server_connected;
    }
}

static void concurrent_connection_on_hold(evutil_socket_t socket, short events, void* user_data)
{
    concurrent_connection_client_t* client = user_data;
    concurrent_connection_test_t*   test;

    (void)socket;
    (void)events;
    if (client == NULL || client->test == NULL || !client->connected) {
        return;
    }
    test = client->test;
    utp_connection_close(client->connection);
    ++test->held_count;
    if (test->held_count == test->connection_count) {
        concurrent_connection_stop(test);
    }
}

static void concurrent_connection_on_client_connected(utp_connection_t* connection, void* user_data)
{
    concurrent_connection_client_t* client = user_data;
    concurrent_connection_test_t*   test;
    struct timeval                  hold;
    int32_t                         hold_seconds;
    int32_t                         hold_microseconds;

    if (client == NULL || client->test == NULL || client->connected) {
        return;
    }
    test               = client->test;
    client->connection = connection;
    client->connected  = true;
    ++test->client_connected;
    hold_seconds      = (int32_t)(test->hold_ms / 1000u);
    hold_microseconds = (int32_t)((test->hold_ms % 1000u) * 1000u);
    hold.tv_sec       = hold_seconds;
    hold.tv_usec      = hold_microseconds;
    if (event_add(client->hold_event, &hold) != 0) {
        ++test->connect_errors;
        concurrent_connection_stop(test);
    }
}

static void concurrent_connection_on_connect_error(utp_status_t status, const char* message,
                                                   const utp_connect_attempt_info_t* attempt, void* user_data)
{
    concurrent_connection_client_t* client = user_data;

    (void)status;
    (void)message;
    (void)attempt;
    if (client != NULL && client->test != NULL) {
        ++client->test->connect_errors;
        concurrent_connection_stop(client->test);
    }
}

static void concurrent_connection_on_connection_error(utp_connection_t*                  connection,
                                                      const utp_connection_error_info_t* info, void* user_data)
{
    concurrent_connection_client_t* client = user_data;

    (void)connection;
    (void)info;
    if (client != NULL && client->test != NULL) {
        ++client->test->connection_errors;
        concurrent_connection_stop(client->test);
    }
}

static void concurrent_connection_on_server_connection_error(utp_connection_t*                  connection,
                                                             const utp_connection_error_info_t* info, void* user_data)
{
    concurrent_connection_test_t* test = user_data;

    (void)connection;
    if (test != NULL && info != NULL && info->status != UTP_STATUS_OK) {
        ++test->connection_errors;
        concurrent_connection_stop(test);
    }
}

static void concurrent_connection_on_timeout(evutil_socket_t socket, short events, void* user_data)
{
    concurrent_connection_test_t* test = user_data;

    (void)socket;
    (void)events;
    if (test != NULL) {
        test->timed_out = true;
        concurrent_connection_stop(test);
    }
}

static void concurrent_connection_usage(const char* program)
{
    fprintf(stderr, "Usage: %s [--connections N] [--hold-ms N] [--encryption none|aes128|aes256]\n", program);
}

int main(int argc, char** argv)
{
    concurrent_connection_test_t test           = {0};
    utp_context_options_t        server_options = UTP_CONTEXT_OPTIONS_INIT;
    server_options.peer_id                      = "test";
    uint16_t server_port                        = 0u;
    uint32_t index;
    int32_t  result = 1;

    test.connection_count = 10u;
    test.hold_ms          = 5000u;
    test.encryption       = UTP_ENCRYPTION_AES_GCM_128;
    for (index = 1u; index < (uint32_t)argc; ++index) {
        if (strcmp(argv[index], "--connections") == 0 && index + 1u < (uint32_t)argc) {
            if (!concurrent_connection_parse_u32(argv[++index], &test.connection_count) ||
                test.connection_count == 0u) {
                concurrent_connection_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--hold-ms") == 0 && index + 1u < (uint32_t)argc) {
            if (!concurrent_connection_parse_u32(argv[++index], &test.hold_ms) || test.hold_ms == 0u) {
                concurrent_connection_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--encryption") == 0 && index + 1u < (uint32_t)argc) {
            if (!concurrent_connection_parse_encryption(argv[++index], &test.encryption)) {
                concurrent_connection_usage(argv[0]);
                return 2;
            }
        } else {
            concurrent_connection_usage(argv[0]);
            return 2;
        }
    }
    test.event_base = event_base_new();
    test.clients    = calloc(test.connection_count, sizeof(*test.clients));
    if (test.event_base == NULL || test.clients == NULL) {
        fprintf(stderr, "concurrent connection test setup failed\n");
        goto cleanup;
    }
    server_options.event_base       = test.event_base;
    server_options.context_id       = 1u;
    server_options.enable_keepalive = false;
    if (utp_context_create(&server_options, &test.server) != UTP_STATUS_OK ||
        utp_context_bind(test.server, "127.0.0.1", 0u, NULL, &server_port) != UTP_STATUS_OK) {
        fprintf(stderr, "server setup failed\n");
        goto cleanup;
    }
    utp_context_set_on_new_connection(test.server, concurrent_connection_on_new_connection, &test);
    utp_context_set_on_connected(test.server, concurrent_connection_on_server_connected, &test);
    utp_context_set_on_connection_error(test.server, concurrent_connection_on_server_connection_error, &test);
    test.timeout_event = evtimer_new(test.event_base, concurrent_connection_on_timeout, &test);
    if (test.timeout_event == NULL) {
        fprintf(stderr, "timeout event setup failed\n");
        goto cleanup;
    }
    for (index = 0u; index < test.connection_count; ++index) {
        concurrent_connection_client_t* client         = &test.clients[index];
        utp_context_options_t           client_options = UTP_CONTEXT_OPTIONS_INIT;
        client_options.peer_id                         = "test";
        utp_connect_options_t connect_options          = UTP_CONNECT_OPTIONS_INIT;

        client->test                    = &test;
        client_options.event_base       = test.event_base;
        client_options.context_id       = (uint64_t)index + 2u;
        client_options.enable_keepalive = false;
        if (utp_context_create(&client_options, &client->context) != UTP_STATUS_OK ||
            utp_context_bind(client->context, "127.0.0.1", 0u, NULL, NULL) != UTP_STATUS_OK) {
            fprintf(stderr, "client %" PRIu32 " setup failed\n", index);
            goto cleanup;
        }
        client->hold_event = evtimer_new(test.event_base, concurrent_connection_on_hold, client);
        if (client->hold_event == NULL) {
            fprintf(stderr, "client %" PRIu32 " hold event setup failed\n", index);
            goto cleanup;
        }
        utp_context_set_on_connected(client->context, concurrent_connection_on_client_connected, client);
        utp_context_set_on_connect_error(client->context, concurrent_connection_on_connect_error, client);
        utp_context_set_on_connection_error(client->context, concurrent_connection_on_connection_error, client);
        connect_options.address        = "127.0.0.1";
        connect_options.target_peer_id = "test";
        connect_options.port           = server_port;
        connect_options.timeout_ms     = 3000u;
        connect_options.retries        = 5;
        connect_options.encryption     = test.encryption;
        if (utp_context_connect(client->context, &connect_options) != UTP_STATUS_OK) {
            fprintf(stderr, "client %" PRIu32 " connect start failed\n", index);
            goto cleanup;
        }
    }
    {
        struct timeval timeout;

        timeout.tv_sec  = 30;
        timeout.tv_usec = 0;
        (void)event_add(test.timeout_event, &timeout);
    }
    test.started_ms = concurrent_connection_now_ms();
    (void)event_base_dispatch(test.event_base);
    if (!test.timed_out && test.accepted_count == test.connection_count &&
        test.server_connected == test.connection_count && test.client_connected == test.connection_count &&
        test.held_count == test.connection_count && test.connect_errors == 0u && test.connection_errors == 0u) {
        result = 0;
    }
    printf("result=%s connections=%" PRIu32 " client_connected=%" PRIu32 " server_accepted=%" PRIu32
           " server_connected=%" PRIu32 " held=%" PRIu32 " connect_errors=%" PRIu32 " connection_errors=%" PRIu32
           " elapsed_ms=%" PRIu64 "\n",
           result == 0 ? "PASS" : "FAIL", test.connection_count, test.client_connected, test.accepted_count,
           test.server_connected, test.held_count, test.connect_errors, test.connection_errors,
           concurrent_connection_now_ms() - test.started_ms);

cleanup:
    if (test.clients != NULL) {
        for (index = 0u; index < test.connection_count; ++index) {
            event_free(test.clients[index].hold_event);
            utp_context_destroy(test.clients[index].context);
        }
    }
    event_free(test.timeout_event);
    utp_context_destroy(test.server);
    free(test.clients);
    event_base_free(test.event_base);
    return result;
}
