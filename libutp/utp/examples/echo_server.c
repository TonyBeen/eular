#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <utp/utp.h>

enum echo_server_phase {
    ECHO_SERVER_READ_HEADER,
    ECHO_SERVER_READ_PAYLOAD,
    ECHO_SERVER_FINISHED,
    ECHO_SERVER_FAILED,
};

struct echo_server_stream;

typedef struct echo_server_app {
    utp_context_t*             context;  // 由应用持有的服务端 Context
    struct echo_server_stream* streams;  // 尚未由流关闭回调释放的状态链表
    bool                       quiet;    // 是否关闭常规输出
} echo_server_app_t;

typedef struct echo_server_stream {
    echo_server_app_t*         app;              // 所属应用
    utp_connection_t*          connection;       // 借用的所属连接
    utp_stream_t*              stream;           // 借用的协议流
    struct echo_server_stream* next;             // 应用状态链表中的下一项
    enum echo_server_phase     phase;            // 应用协议解析阶段
    char                       header[128];      // 尚未完成的 UPLOAD 行
    size_t                     header_length;    // header 已接收长度
    uint64_t                   expected_bytes;   // UPLOAD 声明的总负载长度
    uint64_t                   received_bytes;   // 已校验的负载长度
    XXH3_state_t*              hash;             // 负载完整性校验状态
    char                       response[96];     // DONE 响应
    size_t                     response_length;  // 待写响应长度
    bool                       response_queued;  // 响应是否已经放入协议发送缓冲
    bool                       write_shutdown;   // 是否已发送本地写 FIN
    bool                       fin_received;     // 是否收到了上传流 FIN
} echo_server_stream_t;

static void echo_server_release_stream(echo_server_stream_t* state)
{
    echo_server_stream_t** link;

    if (state == NULL || state->app == NULL) {
        return;
    }
    link = &state->app->streams;
    while (*link != NULL && *link != state) {
        link = &(*link)->next;
    }
    if (*link != state) {
        return;
    }
    *link = state->next;
    XXH3_freeState(state->hash);
    free(state);
}

static void echo_server_release_connection_streams(echo_server_app_t* app, const utp_connection_t* connection)
{
    echo_server_stream_t** link;

    if (app == NULL) {
        return;
    }
    link = &app->streams;
    while (*link != NULL) {
        echo_server_stream_t* state = *link;

        if (connection != NULL && state->connection == connection) {
            *link = state->next;
            XXH3_freeState(state->hash);
            free(state);
            continue;
        }
        link = &state->next;
    }
}

static void echo_server_release_all_streams(echo_server_app_t* app)
{
    echo_server_stream_t* state;

    if (app == NULL) {
        return;
    }
    state        = app->streams;
    app->streams = NULL;
    while (state != NULL) {
        echo_server_stream_t* next = state->next;

        XXH3_freeState(state->hash);
        free(state);
        state = next;
    }
}

static void echo_server_usage(const char* program)
{
    fprintf(stderr, "Usage: %s [--bind-ip IP] [--bind-port PORT] [--quiet]\n", program);
}

static bool echo_parse_u16(const char* text, uint16_t* out_value)
{
    char*         end = NULL;
    unsigned long value;

    if (text == NULL || out_value == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT16_MAX) {
        return false;
    }
    *out_value = (uint16_t)value;
    return true;
}

static bool echo_parse_upload_header(const char* line, uint64_t* out_bytes)
{
    static const char  prefix[] = "UPLOAD ";
    char*              end      = NULL;
    unsigned long long value;

    if (line == NULL || out_bytes == NULL || strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }
    errno = 0;
    value = strtoull(line + sizeof(prefix) - 1u, &end, 10);
    if (errno != 0 || end == line + sizeof(prefix) - 1u || *end != '\0' || value == 0u) {
        return false;
    }
    *out_bytes = (uint64_t)value;
    return true;
}

static void echo_hash_hex(XXH3_state_t* hash, char output[33])
{
    static const char  hex[] = "0123456789abcdef";
    XXH128_canonical_t canonical;
    XXH128_hash_t      digest;
    size_t             index;

    digest = XXH3_128bits_digest(hash);
    XXH128_canonicalFromHash(&canonical, digest);
    for (index = 0u; index < sizeof(canonical.digest); ++index) {
        output[index * 2u]      = hex[canonical.digest[index] >> 4u];
        output[index * 2u + 1u] = hex[canonical.digest[index] & 0x0fu];
    }
    output[32] = '\0';
}

static void echo_server_stream_closed(utp_stream_t* stream, void* user_data)
{
    echo_server_stream_t* state = user_data;

    (void)stream;
    echo_server_release_stream(state);
}

static void echo_server_fail(echo_server_stream_t* state, const char* reason)
{
    utp_stream_t* stream;

    if (state == NULL || state->phase == ECHO_SERVER_FAILED) {
        return;
    }
    state->phase = ECHO_SERVER_FAILED;
    if (!state->app->quiet) {
        fprintf(stderr, "[server] stream %" PRIu32 " failed: %s\n", utp_stream_id(state->stream), reason);
    }
    stream = state->stream;
    (void)utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_READ);
    (void)utp_stream_reset(stream, 1u);
}

static void echo_server_flush_response(echo_server_stream_t* state)
{
    utp_status_t status;

    if (state == NULL || state->phase == ECHO_SERVER_FAILED || state->response_length == 0u || state->response_queued) {
        return;
    }
    status = utp_stream_write(state->stream, state->response, state->response_length);
    if (status == UTP_STATUS_WOULD_BLOCK) {
        return;
    }
    if (status != UTP_STATUS_OK) {
        echo_server_fail(state, "response_write_failed");
        return;
    }
    state->response_queued = true;
    status                 = utp_stream_shutdown(state->stream, UTP_STREAM_SHUTDOWN_WRITE);
    if (status != UTP_STATUS_OK && status != UTP_STATUS_CLOSED) {
        echo_server_fail(state, "response_shutdown_failed");
        return;
    }
    state->write_shutdown = true;
}

static void echo_server_finish_upload(echo_server_stream_t* state)
{
    char hash[33];
    int  count;

    if (state == NULL || state->phase != ECHO_SERVER_READ_PAYLOAD || !state->fin_received) {
        return;
    }
    if (state->received_bytes != state->expected_bytes) {
        echo_server_fail(state, "size_mismatch");
        return;
    }
    echo_hash_hex(state->hash, hash);
    count = snprintf(state->response, sizeof(state->response), "DONE bytes=%" PRIu64 " xxh128=%s\n",
                     state->received_bytes, hash);
    if (count <= 0 || (size_t)count >= sizeof(state->response)) {
        echo_server_fail(state, "response_format_failed");
        return;
    }
    state->response_length = (size_t)count;
    state->phase           = ECHO_SERVER_FINISHED;
    echo_server_flush_response(state);
}

static bool echo_server_consume(echo_server_stream_t* state, const uint8_t* data, size_t length)
{
    size_t index = 0u;

    while (index < length && state->phase != ECHO_SERVER_FAILED) {
        if (state->phase == ECHO_SERVER_READ_HEADER) {
            const uint8_t byte = data[index++];

            if (byte == (uint8_t)'\n') {
                if (state->header_length > 0u && state->header[state->header_length - 1u] == '\r') {
                    --state->header_length;
                }
                state->header[state->header_length] = '\0';
                if (!echo_parse_upload_header(state->header, &state->expected_bytes)) {
                    echo_server_fail(state, "bad_upload_header");
                    return false;
                }
                state->phase = ECHO_SERVER_READ_PAYLOAD;
                continue;
            }
            if (state->header_length + 1u >= sizeof(state->header)) {
                echo_server_fail(state, "upload_header_too_long");
                return false;
            }
            state->header[state->header_length++] = (char)byte;
            continue;
        }
        if (state->received_bytes > state->expected_bytes ||
            (uint64_t)(length - index) > state->expected_bytes - state->received_bytes) {
            echo_server_fail(state, "payload_overflow");
            return false;
        }
        if (XXH3_128bits_update(state->hash, data + index, length - index) != XXH_OK) {
            echo_server_fail(state, "xxh128_update_failed");
            return false;
        }
        state->received_bytes += (uint64_t)(length - index);
        index                  = length;
    }
    return state->phase != ECHO_SERVER_FAILED;
}

static void echo_server_stream_readable(utp_stream_t* stream, void* user_data)
{
    echo_server_stream_t* state = user_data;

    if (state == NULL || state->stream != stream || state->phase == ECHO_SERVER_FAILED) {
        return;
    }
    for (;;) {
        utp_stream_read_view_t view;
        utp_status_t           status = utp_stream_acquire_read_view(stream, &view);

        if (status == UTP_STATUS_WOULD_BLOCK) {
            break;
        }
        if (status == UTP_STATUS_CLOSED) {
            state->fin_received = true;
            if (state->phase == ECHO_SERVER_READ_PAYLOAD) {
                echo_server_finish_upload(state);
            } else {
                echo_server_fail(state, "missing_upload_header");
            }
            return;
        }
        if (status != UTP_STATUS_OK) {
            echo_server_fail(state, "read_view_failed");
            return;
        }
        if (!echo_server_consume(state, view.data, view.length)) {
            return;
        }
        status = utp_stream_commit_read_view(stream, view.offset, view.length);
        if (status != UTP_STATUS_OK) {
            echo_server_fail(state, "read_commit_failed");
            return;
        }
        if (view.fin) {
            state->fin_received = true;
            echo_server_finish_upload(state);
            return;
        }
    }
}

static void echo_server_stream_writable(utp_stream_t* stream, void* user_data)
{
    echo_server_stream_t* state = user_data;

    if (state != NULL && state->stream == stream && !state->write_shutdown) {
        echo_server_flush_response(state);
    }
}

static void echo_server_incoming_stream(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    echo_server_app_t*    app   = user_data;
    echo_server_stream_t* state = calloc(1u, sizeof(*state));

    if (state == NULL) {
        (void)utp_stream_reset(stream, 1u);
        return;
    }
    state->app        = app;
    state->connection = connection;
    state->stream     = stream;
    state->phase      = ECHO_SERVER_READ_HEADER;
    state->hash       = XXH3_createState();
    if (state->hash == NULL || XXH3_128bits_reset(state->hash) != XXH_OK) {
        XXH3_freeState(state->hash);
        free(state);
        (void)utp_stream_reset(stream, 1u);
        return;
    }
    state->next  = app->streams;
    app->streams = state;
    utp_stream_set_on_closed(stream, echo_server_stream_closed, state);
    utp_stream_set_on_writable(stream, echo_server_stream_writable, state);
    utp_stream_set_on_readable(stream, echo_server_stream_readable, state);
    if (!app->quiet) {
        fprintf(stdout, "[server] incoming stream id=%" PRIu32 "\n", utp_stream_id(stream));
    }
}

static void echo_server_connected(utp_connection_t* connection, void* user_data)
{
    echo_server_app_t*           app = user_data;
    utp_connection_description_t description;

    if (utp_connection_get_description(connection, &description) != UTP_STATUS_OK) {
        return;
    }
    utp_connection_set_on_incoming_stream(connection, echo_server_incoming_stream, app);
    if (!app->quiet) {
        fprintf(stdout, "[server] connected scid=%" PRIu32 " dcid=%" PRIu32 " peer=%s:%" PRIu16 "\n",
                description.local_cid, description.peer_cid, description.remote_host, description.remote_port);
    }
}

static bool echo_server_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    echo_server_app_t* app = user_data;
    utp_status_t       status;

    (void)info;
    status = utp_context_accept(app->context);
    if (status != UTP_STATUS_OK && !app->quiet) {
        fprintf(stderr, "[server] accept failed: %s\n", utp_status_string(status));
    }
    return status == UTP_STATUS_OK;
}

static void echo_server_connection_error(utp_connection_t* connection, const utp_connection_error_info_t* info,
                                         void* user_data)
{
    echo_server_app_t* app = user_data;

    echo_server_release_connection_streams(app, connection);
    if (!app->quiet && info != NULL && info->status != UTP_STATUS_OK) {
        fprintf(stderr, "[server] connection error: %s\n", utp_status_string(info->status));
    }
}

int main(int argc, char** argv)
{
    const char*           bind_ip   = "0.0.0.0";
    uint16_t              bind_port = 9000u;
    bool                  quiet     = false;
    struct event_base*    event_base;
    utp_context_options_t options     = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_t*        context     = NULL;
    echo_server_app_t     app         = {0};
    uint16_t              actual_port = 0u;
    int                   index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--bind-ip") == 0 && index + 1 < argc) {
            bind_ip = argv[++index];
        } else if (strcmp(argv[index], "--bind-port") == 0 && index + 1 < argc) {
            if (!echo_parse_u16(argv[++index], &bind_port) || bind_port == 0u) {
                echo_server_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--quiet") == 0 || strcmp(argv[index], "--silent") == 0) {
            quiet = true;
        } else {
            echo_server_usage(argv[0]);
            return 2;
        }
    }
    event_base = event_base_new();
    if (event_base == NULL) {
        fprintf(stderr, "[server] event_base_new failed\n");
        return 1;
    }
    options.event_base                          = event_base;
    options.peer_id                             = "echo-server";
    options.context_id                          = 1u;
    options.enable_keepalive                    = true;
    options.enable_dplpmtud                     = false;
    options.mtu_min                             = 1400u;
    options.mtu_base                            = 1400u;
    options.mtu_max                             = 1400u;
    options.ack_every_n_packets                 = 30u;
    options.handshake_timeout                   = 3000u;
    options.initial_max_stream_data_bidi_local  = UINT64_C(1024) * 1024u;
    options.initial_max_stream_data_bidi_remote = UINT64_C(1024) * 1024u;
    if (utp_context_create(&options, &context) != UTP_STATUS_OK ||
        utp_context_bind(context, bind_ip, bind_port, NULL, &actual_port) != UTP_STATUS_OK) {
        fprintf(stderr, "[server] context setup failed\n");
        utp_context_destroy(context);
        event_base_free(event_base);
        return 1;
    }
    app.context = context;
    app.quiet   = quiet;
    utp_context_set_on_new_connection(context, echo_server_new_connection, &app);
    utp_context_set_on_connected(context, echo_server_connected, &app);
    utp_context_set_on_connection_error(context, echo_server_connection_error, &app);
    if (!quiet) {
        fprintf(stdout, "[server] listening %s:%" PRIu16 "\n", bind_ip, actual_port);
    }
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    event_base_dispatch(event_base);
    utp_context_destroy(context);
    echo_server_release_all_streams(&app);
    event_base_free(event_base);
    return 0;
}
