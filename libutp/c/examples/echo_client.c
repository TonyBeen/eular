#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <event2/util.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <utp/utp.h>

static uint8_t echo_client_payload[16384u];  // 固定测试负载，最大长度受命令行校验约束

typedef struct echo_client_app {
    struct event_base* event_base;          // 调用方拥有的事件循环
    struct event*      timeout_event;       // 整个上传的保护超时
    utp_connection_t*  connection;          // Context 借用的活动连接
    utp_stream_t*      stream;              // Connection 借用的上传流
    XXH3_state_t*      hash;                // 本地发送负载校验状态
    uint32_t           payload_length;      // 单次生成的负载长度
    size_t             payload_offset;      // 下次写入固定负载的起始位置
    uint64_t           target_bytes;        // 本次上传总字节数
    uint64_t           sent_bytes;          // 已提交到协议发送缓冲的负载字节数
    uint64_t           done_bytes;          // 服务端 DONE 中声明的字节数
    uint64_t           started_ms;          // 发起连接时间
    uint64_t           connected_ms;        // 完成握手时间
    uint64_t           upload_done_ms;      // 本地写 FIN 时间
    uint64_t           done_ms;             // 收到 DONE 时间
    char               header[64];          // UPLOAD 请求行
    size_t             header_length;       // 请求行长度
    bool               header_sent;         // 请求行是否已提交
    bool               write_shutdown;      // 本地写方向是否已经关闭
    bool               writing;             // 防止 writable 回调重入
    bool               finished;            // 结果是否已经输出
    bool               passed;              // 最终校验结果
    bool               quiet;               // 是否关闭过程输出
    char               local_hash[33];      // 发送端 XXH128
    char               server_hash[33];     // 服务端返回的 XXH128
    char               response_line[128];  // 未完成的响应行
    size_t             response_length;     // response_line 已接收长度
} echo_client_app_t;

static void echo_client_usage(const char* program)
{
    fprintf(stderr,
            "Usage: %s [--server-ip IP] [--server-port PORT] [--bind-ip IP] [--bind-port PORT]\n"
            "          [--count N] [--length N] [--total-bytes N] [--encryption MODE] [--quiet]\n",
            program);
}

static uint64_t echo_now_ms(void)
{
    struct timeval time_value;

    if (evutil_gettimeofday(&time_value, NULL) != 0) {
        return 0u;
    }
    return (uint64_t)time_value.tv_sec * UINT64_C(1000) + (uint64_t)time_value.tv_usec / UINT64_C(1000);
}

static bool echo_parse_u64(const char* text, uint64_t* out_value)
{
    char*              end = NULL;
    unsigned long long value;

    if (text == NULL || out_value == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

static bool echo_parse_u16(const char* text, uint16_t* out_value)
{
    uint64_t value;

    if (!echo_parse_u64(text, &value) || value > UINT16_MAX) {
        return false;
    }
    *out_value = (uint16_t)value;
    return true;
}

static bool echo_parse_u32(const char* text, uint32_t* out_value)
{
    uint64_t value;

    if (!echo_parse_u64(text, &value) || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool echo_parse_encryption(const char* text, utp_encryption_mode_t* out_mode)
{
    if (strcmp(text, "none") == 0) {
        *out_mode = UTP_ENCRYPTION_NONE;
        return true;
    }
    if (strcmp(text, "aes128") == 0) {
        *out_mode = UTP_ENCRYPTION_AES_GCM_128;
        return true;
    }
    if (strcmp(text, "aes256") == 0) {
        *out_mode = UTP_ENCRYPTION_AES_GCM_256;
        return true;
    }
    return false;
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

static void echo_client_print_result(echo_client_app_t* app, const char* reason)
{
    const uint64_t now_ms = echo_now_ms();
    bool           hash_match;
    bool           byte_match;

    if (app == NULL || app->finished) {
        return;
    }
    echo_hash_hex(app->hash, app->local_hash);
    byte_match    = app->sent_bytes == app->target_bytes && app->done_bytes == app->sent_bytes;
    hash_match    = app->server_hash[0] != '\0' && strcmp(app->local_hash, app->server_hash) == 0;
    app->passed   = byte_match && hash_match;
    app->finished = true;
    fprintf(app->passed ? stdout : stderr,
            "[client] result=%s reason=%s sent_bytes=%" PRIu64 " done_bytes=%" PRIu64 " connect_ms=%" PRIu64
            " upload_ms=%" PRIu64 " done_ms=%" PRIu64 " total_ms=%" PRIu64 " local_xxh128=%s server_xxh128=%s\n",
            app->passed ? "PASS" : "FAIL", reason, app->sent_bytes, app->done_bytes,
            app->connected_ms == 0u ? 0u : app->connected_ms - app->started_ms,
            app->upload_done_ms == 0u ? 0u : app->upload_done_ms - app->started_ms,
            app->done_ms == 0u ? 0u : app->done_ms - app->started_ms, now_ms - app->started_ms,
            app->local_hash[0] == '\0' ? "<pending>" : app->local_hash,
            app->server_hash[0] == '\0' ? "<pending>" : app->server_hash);
}

static void echo_client_finish(echo_client_app_t* app, const char* reason)
{
    if (app == NULL || app->finished) {
        return;
    }
    echo_client_print_result(app, reason);
    if (app->timeout_event != NULL) {
        (void)event_del(app->timeout_event);
    }
    if (app->connection != NULL) {
        utp_connection_close(app->connection);
    }
    event_base_loopbreak(app->event_base);
}

static void echo_client_fail(echo_client_app_t* app, const char* reason)
{
    if (app != NULL && !app->finished) {
        echo_client_finish(app, reason);
    }
}

static void echo_client_copy_payload(uint8_t* destination, size_t length, const uint8_t* payload, size_t payload_length,
                                     size_t* payload_offset)
{
    size_t copied = 0u;

    while (copied < length) {
        const size_t available = payload_length - *payload_offset;
        const size_t part      = available < length - copied ? available : length - copied;

        memcpy(destination + copied, payload + *payload_offset, part);
        copied          += part;
        *payload_offset  = (*payload_offset + part) % payload_length;
    }
}

static void echo_client_try_write(echo_client_app_t* app)
{
    if (app == NULL || app->stream == NULL || app->writing || app->finished) {
        return;
    }
    app->writing = true;
    while (!app->finished) {
        utp_status_t status;

        if (!app->header_sent) {
            status = utp_stream_write(app->stream, app->header, app->header_length);
            if (status == UTP_STATUS_WOULD_BLOCK) {
                break;
            }
            if (status != UTP_STATUS_OK) {
                echo_client_fail(app, "header_write_failed");
                break;
            }
            app->header_sent = true;
            continue;
        }
        if (app->sent_bytes == app->target_bytes) {
            if (!app->write_shutdown) {
                status = utp_stream_shutdown(app->stream, UTP_STREAM_SHUTDOWN_WRITE);
                if (status != UTP_STATUS_OK && status != UTP_STATUS_CLOSED) {
                    echo_client_fail(app, "write_shutdown_failed");
                    break;
                }
                app->write_shutdown = true;
                app->upload_done_ms = echo_now_ms();
            }
            break;
        }
        {
            utp_stream_write_view_t views[2];
            size_t                  view_count = 0u;
            size_t                  writable   = 0u;
            size_t                  to_write;
            size_t                  commit_length;
            size_t                  remaining;
            size_t                  index;

            status = utp_stream_acquire_write_views(app->stream, views, 2u, &view_count, &writable);
            if (status == UTP_STATUS_WOULD_BLOCK) {
                break;
            }
            if (status != UTP_STATUS_OK || writable == 0u) {
                echo_client_fail(app, "write_view_failed");
                break;
            }
            remaining = (size_t)((app->target_bytes - app->sent_bytes) > (uint64_t)SIZE_MAX
                                     ? SIZE_MAX
                                     : app->target_bytes - app->sent_bytes);
            to_write  = writable < remaining ? writable : remaining;
            if (to_write > app->payload_length) {
                to_write = app->payload_length;
            }
            commit_length = to_write;
            for (index = 0u; index < view_count && to_write > 0u; ++index) {
                const size_t part = views[index].length < to_write ? views[index].length : to_write;

                echo_client_copy_payload(views[index].data, part, echo_client_payload, app->payload_length,
                                         &app->payload_offset);
                if (XXH3_128bits_update(app->hash, views[index].data, part) != XXH_OK) {
                    echo_client_fail(app, "xxh128_update_failed");
                    break;
                }
                app->sent_bytes += (uint64_t)part;
                to_write        -= part;
            }
            if (app->finished) {
                break;
            }
            status = utp_stream_commit_write_views(app->stream, commit_length);
            if (status != UTP_STATUS_OK) {
                echo_client_fail(app, "write_commit_failed");
                break;
            }
        }
    }
    app->writing = false;
}

static bool echo_client_parse_done(echo_client_app_t* app, const char* line)
{
    static const char  prefix[] = "DONE bytes=";
    const char*        hash_mark;
    char*              end = NULL;
    unsigned long long bytes;

    if (strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }
    hash_mark = strstr(line, " xxh128=");
    if (hash_mark == NULL || strlen(hash_mark + 8u) != 32u) {
        return false;
    }
    errno = 0;
    bytes = strtoull(line + sizeof(prefix) - 1u, &end, 10);
    if (errno != 0 || end != hash_mark) {
        return false;
    }
    app->done_bytes = (uint64_t)bytes;
    memcpy(app->server_hash, hash_mark + 8u, 33u);
    return true;
}

static void echo_client_stream_readable(utp_stream_t* stream, void* user_data)
{
    echo_client_app_t* app = user_data;

    if (app == NULL || app->stream != stream || app->finished) {
        return;
    }
    for (;;) {
        utp_stream_read_view_t view;
        utp_status_t           status = utp_stream_acquire_read_view(stream, &view);
        size_t                 index;

        if (status == UTP_STATUS_WOULD_BLOCK) {
            return;
        }
        if (status == UTP_STATUS_CLOSED) {
            echo_client_fail(app, "server_closed_before_done");
            return;
        }
        if (status != UTP_STATUS_OK) {
            echo_client_fail(app, "response_read_failed");
            return;
        }
        for (index = 0u; index < view.length; ++index) {
            const uint8_t byte = view.data[index];

            if (byte == (uint8_t)'\n') {
                app->response_line[app->response_length] = '\0';
                if (!echo_client_parse_done(app, app->response_line)) {
                    (void)utp_stream_commit_read_view(stream, view.offset, view.length);
                    echo_client_fail(app, "bad_done_response");
                    return;
                }
                app->done_ms = echo_now_ms();
                status       = utp_stream_commit_read_view(stream, view.offset, view.length);
                if (status != UTP_STATUS_OK) {
                    echo_client_fail(app, "response_commit_failed");
                    return;
                }
                echo_client_finish(app, "done");
                return;
            }
            if (app->response_length + 1u >= sizeof(app->response_line)) {
                (void)utp_stream_commit_read_view(stream, view.offset, view.length);
                echo_client_fail(app, "response_too_long");
                return;
            }
            app->response_line[app->response_length++] = (char)byte;
        }
        status = utp_stream_commit_read_view(stream, view.offset, view.length);
        if (status != UTP_STATUS_OK) {
            echo_client_fail(app, "response_commit_failed");
            return;
        }
        if (view.fin) {
            echo_client_fail(app, "server_closed_before_done");
            return;
        }
    }
}

static void echo_client_stream_writable(utp_stream_t* stream, void* user_data)
{
    echo_client_app_t* app = user_data;

    if (app != NULL && app->stream == stream) {
        echo_client_try_write(app);
    }
}

static void echo_client_connected(utp_connection_t* connection, void* user_data)
{
    echo_client_app_t* app = user_data;
    uint32_t           stream_id;
    utp_status_t       status;

    if (app == NULL || app->finished) {
        return;
    }
    app->connection   = connection;
    app->connected_ms = echo_now_ms();
    status            = utp_connection_create_stream(connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id);
    if (status != UTP_STATUS_OK) {
        echo_client_fail(app, "stream_create_failed");
        return;
    }
    app->stream = utp_connection_get_stream(connection, stream_id);
    if (app->stream == NULL) {
        echo_client_fail(app, "stream_lookup_failed");
        return;
    }
    utp_stream_set_on_readable(app->stream, echo_client_stream_readable, app);
    utp_stream_set_on_writable(app->stream, echo_client_stream_writable, app);
    echo_client_try_write(app);
}

static void echo_client_connect_error(utp_status_t status, const char* message,
                                      const utp_connect_attempt_info_t* attempt, void* user_data)
{
    echo_client_app_t* app = user_data;

    (void)status;
    (void)message;
    (void)attempt;
    echo_client_fail(app, "connect_error");
}

static void echo_client_connection_error(utp_connection_t* connection, const utp_connection_error_info_t* info,
                                         void* user_data)
{
    echo_client_app_t* app = user_data;

    (void)connection;
    (void)info;
    if (app != NULL && !app->finished) {
        echo_client_fail(app, "connection_error");
    }
}

static void echo_client_timeout(evutil_socket_t socket, short events, void* user_data)
{
    echo_client_app_t* app = user_data;

    (void)socket;
    (void)events;
    echo_client_fail(app, "timeout");
}

int main(int argc, char** argv)
{
    const char*           server_ip   = "127.0.0.1";
    const char*           bind_ip     = "0.0.0.0";
    uint16_t              server_port = 9000u;
    uint16_t              bind_port   = 0u;
    uint32_t              count       = 5u;
    uint32_t              length      = 16u;
    uint64_t              total_bytes = 0u;
    utp_encryption_mode_t encryption  = UTP_ENCRYPTION_NONE;
    bool                  quiet       = false;
    struct event_base*    event_base;
    utp_context_options_t options         = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;
    utp_context_t*        context         = NULL;
    echo_client_app_t     app             = {0};
    uint64_t              target_bytes;
    uint64_t              product;
    uint32_t              payload_index;
    int                   header_count;
    int                   index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--server-ip") == 0 && index + 1 < argc) {
            server_ip = argv[++index];
        } else if (strcmp(argv[index], "--server-port") == 0 && index + 1 < argc) {
            if (!echo_parse_u16(argv[++index], &server_port) || server_port == 0u) {
                echo_client_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--bind-ip") == 0 && index + 1 < argc) {
            bind_ip = argv[++index];
        } else if (strcmp(argv[index], "--bind-port") == 0 && index + 1 < argc) {
            if (!echo_parse_u16(argv[++index], &bind_port)) {
                echo_client_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--count") == 0 && index + 1 < argc) {
            if (!echo_parse_u32(argv[++index], &count) || count == 0u || count > 200000u) {
                echo_client_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--length") == 0 && index + 1 < argc) {
            if (!echo_parse_u32(argv[++index], &length) || length < 16u || length > 16384u) {
                echo_client_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--total-bytes") == 0 && index + 1 < argc) {
            if (!echo_parse_u64(argv[++index], &total_bytes) || total_bytes > UINT64_C(4294967296)) {
                echo_client_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--encryption") == 0 && index + 1 < argc) {
            if (!echo_parse_encryption(argv[++index], &encryption)) {
                echo_client_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--quiet") == 0 || strcmp(argv[index], "--silent") == 0) {
            quiet = true;
        } else {
            echo_client_usage(argv[0]);
            return 2;
        }
    }
    product      = (uint64_t)count * (uint64_t)length;
    target_bytes = total_bytes == 0u ? product : total_bytes;
    if (target_bytes == 0u) {
        fprintf(stderr, "[client] target byte count must be non-zero\n");
        return 2;
    }
    event_base = event_base_new();
    if (event_base == NULL) {
        fprintf(stderr, "[client] event_base_new failed\n");
        return 1;
    }
    options.event_base                          = event_base;
    options.context_id                          = 2u;
    options.enable_keepalive                    = false;
    options.enable_dplpmtud                     = false;
    options.mtu_min                             = 1400u;
    options.mtu_base                            = 1400u;
    options.mtu_max                             = 1400u;
    options.ack_every_n_packets                 = 30u;
    options.handshake_timeout                   = 3000u;
    options.initial_max_stream_data_bidi_local  = UINT64_C(1024) * 1024u;
    options.initial_max_stream_data_bidi_remote = UINT64_C(1024) * 1024u;
    if (utp_context_create(&options, &context) != UTP_STATUS_OK ||
        utp_context_bind(context, bind_ip, bind_port, NULL, NULL) != UTP_STATUS_OK) {
        fprintf(stderr, "[client] context setup failed\n");
        utp_context_destroy(context);
        event_base_free(event_base);
        return 1;
    }
    app.event_base     = event_base;
    app.payload_length = length;
    app.target_bytes   = target_bytes;
    app.started_ms     = echo_now_ms();
    app.quiet          = quiet;
    {
        static const uint8_t alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        for (payload_index = 0u; payload_index < length; ++payload_index) {
            echo_client_payload[payload_index] = alphabet[payload_index % (sizeof(alphabet) - 1u)];
        }
    }
    app.hash = XXH3_createState();
    if (app.hash == NULL || XXH3_128bits_reset(app.hash) != XXH_OK) {
        fprintf(stderr, "[client] xxh128 initialization failed\n");
        XXH3_freeState(app.hash);
        utp_context_destroy(context);
        event_base_free(event_base);
        return 1;
    }
    header_count = snprintf(app.header, sizeof(app.header), "UPLOAD %" PRIu64 "\n", target_bytes);
    if (header_count <= 0 || (size_t)header_count >= sizeof(app.header)) {
        fprintf(stderr, "[client] upload header format failed\n");
        XXH3_freeState(app.hash);
        utp_context_destroy(context);
        event_base_free(event_base);
        return 1;
    }
    app.header_length = (size_t)header_count;
    app.timeout_event = evtimer_new(event_base, echo_client_timeout, &app);
    if (app.timeout_event == NULL) {
        fprintf(stderr, "[client] timeout event allocation failed\n");
        XXH3_freeState(app.hash);
        utp_context_destroy(context);
        event_base_free(event_base);
        return 1;
    }
    {
        const struct timeval timeout = {180, 0};

        (void)event_add(app.timeout_event, &timeout);
    }
    utp_context_set_on_connected(context, echo_client_connected, &app);
    utp_context_set_on_connect_error(context, echo_client_connect_error, &app);
    utp_context_set_on_connection_error(context, echo_client_connection_error, &app);
    connect_options.address    = server_ip;
    connect_options.port       = server_port;
    connect_options.timeout_ms = 3000u;
    connect_options.retries    = 0;
    connect_options.encryption = encryption;
    if (utp_context_connect(context, &connect_options) != UTP_STATUS_OK) {
        echo_client_fail(&app, "connect_start_failed");
    } else {
        if (!quiet) {
            fprintf(stdout, "[client] connecting to %s:%" PRIu16 " total_bytes=%" PRIu64 " length=%" PRIu32 "\n",
                    server_ip, server_port, target_bytes, length);
        }
        event_base_dispatch(event_base);
    }
    if (!app.finished) {
        echo_client_fail(&app, "loop_exit");
    }
    event_free(app.timeout_event);
    XXH3_freeState(app.hash);
    utp_context_destroy(context);
    event_base_free(event_base);
    return app.passed ? 0 : 1;
}
