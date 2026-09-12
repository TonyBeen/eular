#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>

#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/util.h>
#include <utp/utp.h>

static const size_t kPayloadSize = 16384u;

#define LOG(...)                      \
    do {                              \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n");        \
        fflush(stdout);               \
    } while (0)

struct NatPunchApp {
    event_base*                 base;
    event*                      timeout_event;
    event*                      signal_int;
    event*                      signal_term;
    utp_context_t*              context;
    utp_connection_t*           connection;
    utp_stream_t*               stream;
    utp_ntrs_register_options_t register_options;
    utp_connect_options_t       connect_options;
    const char*                 peer_id;
    const char*                 ntrs_address;
    const char*                 target_peer_id;
    uint16_t                    ntrs_port;
    uint64_t                    send_bytes;
    uint64_t                    sent_bytes;
    uint64_t                    received_bytes;
    uint64_t                    expected_bytes;
    uint64_t                    start_ms;
    char                        request[64];
    size_t                      request_length;
    char                        response[64];
    size_t                      response_length;
    char                        input_header[64];
    size_t                      input_header_length;
    uint8_t                     payload[kPayloadSize];
    size_t                      payload_offset;
    bool                        register_requested;
    bool                        listen_requested;
    bool                        rendezvous_connect;
    bool                        initiator;
    bool                        nat_finished;
    bool                        register_started;
    bool                        connect_started;
    bool                        connected;
    bool                        request_sent;
    bool                        write_shutdown;
    bool                        response_pending;
    bool                        response_sent;
    bool                        response_received;
    bool                        input_header_finished;
    bool                        input_fin_received;
    bool                        writing;
    bool                        finished;
    bool                        failed;
};

static void        nat_punch_fail(NatPunchApp* app, const char* reason);
static void        nat_punch_try_write(NatPunchApp* app);
static void        nat_punch_flush_response(NatPunchApp* app);
static void        nat_punch_incoming_stream(utp_connection_t* connection, utp_stream_t* stream, void* user_data);

static const char* nat_punch_nat_name(utp_nat_class_t nat_class)
{
    switch (nat_class) {
    case UTP_NAT_CLASS_OPEN_PUBLIC:
        return "OPEN_PUBLIC";
    case UTP_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL:
        return "OPEN_PUBLIC_WITH_FIREWALL";
    case UTP_NAT_CLASS_FULL_CONE:
        return "FULL_CONE";
    case UTP_NAT_CLASS_IP_RESTRICTED:
        return "IP_RESTRICTED";
    case UTP_NAT_CLASS_PORT_RESTRICTED:
        return "PORT_RESTRICTED";
    case UTP_NAT_CLASS_SYMMETRIC:
        return "SYMMETRIC";
    case UTP_NAT_CLASS_SYMMETRIC_MULTI_LINE:
        return "SYMMETRIC_MULTI_LINE";
    case UTP_NAT_CLASS_UDP_BLOCKED:
        return "UDP_BLOCKED";
    default:
        return "UNKNOWN";
    }
}

static const char* nat_punch_encryption_name(utp_encryption_mode_t encryption)
{
    switch (encryption) {
    case UTP_ENCRYPTION_NONE:
        return "none";
    case UTP_ENCRYPTION_AES_GCM_128:
        return "aes128";
    case UTP_ENCRYPTION_AES_GCM_256:
        return "aes256";
    default:
        return "unknown";
    }
}

static const char* nat_punch_endpoint_format(const utp_endpoint_t* endpoint, char output[80])
{
    char address[INET6_ADDRSTRLEN];

    if ((endpoint->family == 4u || endpoint->family == static_cast<uint8_t>(AF_INET)) &&
        inet_ntop(AF_INET, endpoint->address, address, sizeof(address)) != NULL) {
        snprintf(output, 80u, "%s:%" PRIu16, address, endpoint->port);
        return output;
    }
    if ((endpoint->family == 6u || endpoint->family == static_cast<uint8_t>(AF_INET6)) &&
        inet_ntop(AF_INET6, endpoint->address, address, sizeof(address)) != NULL) {
        if (endpoint->scope_id != 0u)
            snprintf(output, 80u, "[%s%%%" PRIu32 "]:%" PRIu16, address, endpoint->scope_id, endpoint->port);
        else
            snprintf(output, 80u, "[%s]:%" PRIu16, address, endpoint->port);
        return output;
    }
    snprintf(output, 80u, "<none>");
    return output;
}

static bool nat_punch_parse_u64(const char* text, uint64_t* value)
{
    char*              end = NULL;
    unsigned long long parsed;

    if (text == NULL || value == NULL || *text == '\0') return false;
    errno  = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

static bool nat_punch_parse_u16(const char* text, uint16_t* value)
{
    uint64_t parsed;

    if (!nat_punch_parse_u64(text, &parsed) || parsed == 0u || parsed > UINT16_MAX) return false;
    *value = static_cast<uint16_t>(parsed);
    return true;
}

static bool nat_punch_parse_encryption(const char* text, utp_encryption_mode_t* mode)
{
    if (strcmp(text, "none") == 0) {
        *mode = UTP_ENCRYPTION_NONE;
        return true;
    }
    if (strcmp(text, "aes128") == 0) {
        *mode = UTP_ENCRYPTION_AES_GCM_128;
        return true;
    }
    if (strcmp(text, "aes256") == 0) {
        *mode = UTP_ENCRYPTION_AES_GCM_256;
        return true;
    }
    return false;
}

static bool nat_punch_resolve(const std::string& input, int family, std::string* output)
{
    addrinfo  hints  = {};
    addrinfo* result = NULL;
    char      address[INET6_ADDRSTRLEN];

    hints.ai_family   = family;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(input.c_str(), NULL, &hints, &result) != 0) return false;
    for (addrinfo* current = result; current != NULL; current = current->ai_next) {
        const void* source = NULL;
        if (current->ai_family == AF_INET) {
            source = &reinterpret_cast<const sockaddr_in*>(current->ai_addr)->sin_addr;
        } else if (current->ai_family == AF_INET6) {
            source = &reinterpret_cast<const sockaddr_in6*>(current->ai_addr)->sin6_addr;
        }
        if (source != NULL && inet_ntop(current->ai_family, source, address, sizeof(address)) != NULL) {
            *output = address;
            freeaddrinfo(result);
            return true;
        }
    }
    freeaddrinfo(result);
    return false;
}

static uint64_t nat_punch_now_ms()
{
    timeval value;

    if (evutil_gettimeofday(&value, NULL) != 0) return 0u;
    return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1000) + static_cast<uint64_t>(value.tv_usec) / UINT64_C(1000);
}

static void nat_punch_usage(const char* program)
{
    LOG("Usage: %s -i ID -n IP [-N PORT]", program);
    LOG("          [-s IP [-S PORT] -r]");
    LOG("          [-l | -t ID (-a IP -p PORT | -s IP [-S PORT])] [-b IP -P PORT]");
    LOG("          [-I NAME] [-d N] [-T N] [-M N]");
    LOG("          [-e none|aes128|aes256] [-6]");
    LOG("");
    LOG("  -i, --peer-id ID             Local Context peer id");
    LOG("  -n, --nat-address IP         NAT probe service address");
    LOG("  -N, --nat-port PORT          NAT probe service port (default: 24001)");
    LOG("  -s, --ntrs-address IP        NTRS address; required by -r or NTRS rendezvous");
    LOG("  -S, --ntrs-port PORT         NTRS port (default: 24000)");
    LOG("  -r, --register               Register this Context at NTRS");
    LOG("  -l, --listen                 Wait for one direct incoming connection");
    LOG("  -t, --target-peer-id ID      Peer id to connect to");
    LOG("  -a, --peer-address IP        Direct peer address");
    LOG("  -p, --peer-port PORT         Direct peer port");
    LOG("  -b, --bind-ip IP             Local bind address");
    LOG("  -P, --bind-port PORT         Local bind port");
    LOG("  -I, --bind-interface NAME    Local network interface");
    LOG("  -d, --send-bytes N            Payload bytes to send");
    LOG("  -T, --timeout-ms N           Direct connection and transfer timeout");
    LOG("  -M, --nat-timeout-ms N       NAT probe phase timeout");
    LOG("  -e, --encryption MODE        none, aes128, or aes256");
    LOG("  -6, --ipv6                   Use IPv6");
}

static bool nat_punch_parse_header(const char* header, const char* prefix, uint64_t* value)
{
    const size_t       prefix_length = strlen(prefix);
    char*              end           = NULL;
    unsigned long long parsed;

    if (strncmp(header, prefix, prefix_length) != 0) return false;
    errno  = 0;
    parsed = strtoull(header + prefix_length, &end, 10);
    if (errno != 0 || end == header + prefix_length || (*end != '\0' && *end != '\n')) return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

static void nat_punch_break(NatPunchApp* app, bool failed, const char* reason)
{
    if (app->finished) return;
    app->finished = true;
    app->failed   = failed;
    LOG("nat_punch result=%s reason=%s sent_bytes=%" PRIu64 " received_bytes=%" PRIu64 " elapsed_ms=%" PRIu64,
        failed ? "FAIL" : "PASS", reason, app->sent_bytes, app->received_bytes, nat_punch_now_ms() - app->start_ms);
    if (app->timeout_event != NULL) event_del(app->timeout_event);
    if (app->connection != NULL && failed) utp_connection_close(app->connection);
    event_base_loopbreak(app->base);
}

static void nat_punch_fail(NatPunchApp* app, const char* reason) { nat_punch_break(app, true, reason); }

static void nat_punch_finish(NatPunchApp* app, const char* reason) { nat_punch_break(app, false, reason); }

static void nat_punch_fill(uint8_t* destination, size_t length, size_t* offset)
{
    static const uint8_t alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (size_t index = 0u; index < length; ++index) {
        destination[index] = alphabet[*offset % (sizeof(alphabet) - 1u)];
        *offset            = (*offset + 1u) % (sizeof(alphabet) - 1u);
    }
}

static void nat_punch_try_write(NatPunchApp* app)
{
    const uint64_t sent_before = app->sent_bytes;

    if (app->stream == NULL || app->writing || app->finished || !app->initiator) return;
    app->writing = true;
    while (!app->finished) {
        utp_status_t status;

        if (!app->request_sent) {
            status = utp_stream_write(app->stream, app->request, app->request_length);
            if (status == UTP_STATUS_WOULD_BLOCK) break;
            if (status != UTP_STATUS_OK) {
                nat_punch_fail(app, "request_write_failed");
                break;
            }
            app->request_sent = true;
            LOG("nat_punch peer=%s -> Peer [StreamWrite] id=%" PRIu32 " header=\"DATA %" PRIu64 "\\n\"", app->peer_id,
                utp_stream_id(app->stream), app->send_bytes);
            continue;
        }
        if (app->sent_bytes == app->send_bytes) {
            if (!app->write_shutdown) {
                status = utp_stream_shutdown(app->stream, UTP_STREAM_SHUTDOWN_WRITE);
                if (status != UTP_STATUS_OK && status != UTP_STATUS_CLOSED) {
                    nat_punch_fail(app, "write_shutdown_failed");
                    break;
                }
                app->write_shutdown = true;
                LOG("nat_punch peer=%s -> Peer [StreamFin] id=%" PRIu32 " sent=%" PRIu64, app->peer_id,
                    utp_stream_id(app->stream), app->sent_bytes);
            }
            break;
        }
        utp_stream_write_view_t views[2];
        size_t                  view_count = 0u;
        size_t                  writable   = 0u;
        status = utp_stream_acquire_write_views(app->stream, views, 2u, &view_count, &writable);
        if (status == UTP_STATUS_WOULD_BLOCK) break;
        if (status != UTP_STATUS_OK || writable == 0u) {
            nat_punch_fail(app, "write_view_failed");
            break;
        }
        size_t to_write =
            static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(writable), app->send_bytes - app->sent_bytes));
        size_t remaining = to_write;
        for (size_t index = 0u; index < view_count && remaining != 0u; ++index) {
            const size_t part = std::min(views[index].length, remaining);
            nat_punch_fill(views[index].data, part, &app->payload_offset);
            remaining -= part;
        }
        status = utp_stream_commit_write_views(app->stream, to_write);
        if (status != UTP_STATUS_OK) {
            nat_punch_fail(app, "write_commit_failed");
            break;
        }
        app->sent_bytes += static_cast<uint64_t>(to_write);
    }
    app->writing = false;
    if (app->sent_bytes != sent_before)
        LOG("nat_punch peer=%s -> Peer [StreamWrite] id=%" PRIu32 " bytes=%" PRIu64 " total=%" PRIu64 "/%" PRIu64,
            app->peer_id, utp_stream_id(app->stream), app->sent_bytes - sent_before, app->sent_bytes, app->send_bytes);
}

static void nat_punch_flush_response(NatPunchApp* app)
{
    utp_status_t status;

    if (app->stream == NULL || !app->response_pending || app->finished) return;
    status = utp_stream_write(app->stream, app->response, app->response_length);
    if (status == UTP_STATUS_WOULD_BLOCK) return;
    if (status != UTP_STATUS_OK) {
        nat_punch_fail(app, "response_write_failed");
        return;
    }
    app->response_pending = false;
    app->response_sent    = true;
    LOG("nat_punch peer=%s -> Peer [StreamWrite] id=%" PRIu32 " response=\"DONE %" PRIu64 "\\n\"", app->peer_id,
        utp_stream_id(app->stream), app->received_bytes);
    status = utp_stream_shutdown(app->stream, UTP_STREAM_SHUTDOWN_WRITE);
    if (status != UTP_STATUS_OK && status != UTP_STATUS_CLOSED) {
        nat_punch_fail(app, "response_shutdown_failed");
        return;
    }
}

static void nat_punch_finish_input(NatPunchApp* app)
{
    int count;

    if (!app->input_header_finished || !app->input_fin_received || app->response_pending || app->response_sent) return;
    if (app->received_bytes != app->expected_bytes) {
        nat_punch_fail(app, "received_size_mismatch");
        return;
    }
    count = snprintf(app->response, sizeof(app->response), "DONE %" PRIu64 "\n", app->received_bytes);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(app->response)) {
        nat_punch_fail(app, "response_format_failed");
        return;
    }
    app->response_length  = static_cast<size_t>(count);
    app->response_pending = true;
    LOG("nat_punch peer=%s [PayloadReceived] id=%" PRIu32 " bytes=%" PRIu64 "/%" PRIu64, app->peer_id,
        utp_stream_id(app->stream), app->received_bytes, app->expected_bytes);
    nat_punch_flush_response(app);
}

static void nat_punch_consume_input(NatPunchApp* app, const uint8_t* data, size_t length)
{
    size_t offset = 0u;

    while (offset < length && !app->finished) {
        if (!app->input_header_finished) {
            const uint8_t byte = data[offset++];
            if (byte == static_cast<uint8_t>('\n')) {
                uint64_t expected                           = 0u;
                app->input_header[app->input_header_length] = '\0';
                if (app->input_header_length == 0u || !nat_punch_parse_header(app->input_header, "DATA ", &expected)) {
                    nat_punch_fail(app, "bad_data_header");
                    return;
                }
                app->expected_bytes        = expected;
                app->input_header_finished = true;
                LOG("nat_punch peer=%s <- Peer [DataHeader] id=%" PRIu32 " expected_bytes=%" PRIu64, app->peer_id,
                    utp_stream_id(app->stream), app->expected_bytes);
                continue;
            }
            if (app->input_header_length + 1u >= sizeof(app->input_header)) {
                nat_punch_fail(app, "data_header_too_long");
                return;
            }
            app->input_header[app->input_header_length++] = static_cast<char>(byte);
            continue;
        }
        if (app->received_bytes > app->expected_bytes ||
            static_cast<uint64_t>(length - offset) > app->expected_bytes - app->received_bytes) {
            nat_punch_fail(app, "received_payload_overflow");
            return;
        }
        app->received_bytes += static_cast<uint64_t>(length - offset);
        offset               = length;
    }
}

static void nat_punch_consume_response(NatPunchApp* app, const uint8_t* data, size_t length)
{
    if (app->response_received) return;
    if (app->response_length + length >= sizeof(app->response)) {
        nat_punch_fail(app, "response_too_long");
        return;
    }
    memcpy(app->response + app->response_length, data, length);
    app->response_length                += length;
    app->response[app->response_length]  = '\0';
    if (app->response_length == 0u || app->response[app->response_length - 1u] != '\n') return;
    uint64_t done_bytes = 0u;
    if (!nat_punch_parse_header(app->response, "DONE ", &done_bytes) || done_bytes != app->send_bytes ||
        app->sent_bytes != app->send_bytes) {
        nat_punch_fail(app, "done_size_mismatch");
        return;
    }
    app->response_received = true;
    LOG("nat_punch peer=%s <- Peer [TransferComplete] id=%" PRIu32 " bytes=%" PRIu64, app->peer_id,
        utp_stream_id(app->stream), done_bytes);
    nat_punch_finish(app, "transfer_complete");
}

static void nat_punch_stream_readable(utp_stream_t* stream, void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);

    if (app->stream != stream || app->finished) return;
    for (;;) {
        utp_stream_read_view_t view;
        const utp_status_t     status = utp_stream_acquire_read_view(stream, &view);
        if (status == UTP_STATUS_WOULD_BLOCK) return;
        if (status == UTP_STATUS_CLOSED) {
            app->input_fin_received = true;
            LOG("nat_punch peer=%s <- Peer [StreamFin] id=%" PRIu32, app->peer_id, utp_stream_id(stream));
            if (!app->initiator) nat_punch_finish_input(app);
            return;
        }
        if (status != UTP_STATUS_OK) {
            nat_punch_fail(app, "read_view_failed");
            return;
        }
        LOG("nat_punch peer=%s <- Peer [StreamRead] id=%" PRIu32 " bytes=%zu offset=%" PRIu64 " fin=%s", app->peer_id,
            utp_stream_id(stream), view.length, view.offset, view.fin ? "true" : "false");
        if (app->initiator)
            nat_punch_consume_response(app, view.data, view.length);
        else
            nat_punch_consume_input(app, view.data, view.length);
        if (app->finished) return;
        if (utp_stream_commit_read_view(stream, view.offset, view.length) != UTP_STATUS_OK) {
            nat_punch_fail(app, "read_commit_failed");
            return;
        }
        if (view.fin) {
            app->input_fin_received = true;
            LOG("nat_punch peer=%s <- Peer [StreamFin] id=%" PRIu32, app->peer_id, utp_stream_id(stream));
            if (!app->initiator) nat_punch_finish_input(app);
            return;
        }
    }
}

static void nat_punch_stream_writable(utp_stream_t* stream, void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);

    if (app->stream != stream || app->finished) return;
    if (app->initiator)
        nat_punch_try_write(app);
    else
        nat_punch_flush_response(app);
}

static void nat_punch_stream_closed(utp_stream_t* stream, void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);

    if (app->stream != stream || app->finished) return;
    LOG("nat_punch peer=%s [StreamClosed] id=%" PRIu32, app->peer_id, utp_stream_id(stream));
    if (!app->initiator && app->response_pending)
        nat_punch_fail(app, "stream_closed_before_response");
    else if (!app->initiator)
        nat_punch_finish(app, "transfer_complete");
}

static void nat_punch_connected(utp_connection_t* connection, void* user_data)
{
    NatPunchApp*                 app       = static_cast<NatPunchApp*>(user_data);
    uint32_t                     stream_id = 0u;
    utp_connection_description_t description;

    app->connection = connection;
    app->connected  = true;
    if (utp_connection_get_description(connection, &description) == UTP_STATUS_OK) {
        LOG("nat_punch peer=%s [Connected] role=%s remote=%s:%" PRIu16 " local_cid=%" PRIu32 " peer_cid=%" PRIu32,
            app->peer_id, app->initiator ? "active" : "passive", description.remote_host, description.remote_port,
            description.local_cid, description.peer_cid);
    } else {
        LOG("nat_punch peer=%s [Connected] role=%s", app->peer_id, app->initiator ? "active" : "passive");
    }
    if (!app->initiator) {
        utp_connection_set_on_incoming_stream(connection, nat_punch_incoming_stream, app);
        return;
    }
    if (utp_connection_create_stream(connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) != UTP_STATUS_OK) {
        nat_punch_fail(app, "create_stream_failed");
        return;
    }
    app->stream = utp_connection_get_stream(connection, stream_id);
    if (app->stream == NULL) {
        nat_punch_fail(app, "get_stream_failed");
        return;
    }
    LOG("nat_punch peer=%s [StreamCreated] id=%" PRIu32, app->peer_id, stream_id);
    utp_stream_set_on_readable(app->stream, nat_punch_stream_readable, app);
    utp_stream_set_on_writable(app->stream, nat_punch_stream_writable, app);
    utp_stream_set_on_closed(app->stream, nat_punch_stream_closed, app);
    nat_punch_try_write(app);
}

static void nat_punch_incoming_stream(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);

    app->connection = connection;
    app->stream     = stream;
    utp_stream_set_on_readable(stream, nat_punch_stream_readable, app);
    utp_stream_set_on_writable(stream, nat_punch_stream_writable, app);
    utp_stream_set_on_closed(stream, nat_punch_stream_closed, app);
    LOG("nat_punch peer=%s <- Peer [IncomingStream] id=%" PRIu32, app->peer_id, utp_stream_id(stream));
}

static bool nat_punch_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);
    char         remote[80];

    LOG("nat_punch peer=%s <- Peer=%s [NewConnection] local_cid=%" PRIu32 " peer_cid=%" PRIu32 " encryption=%s",
        app->peer_id, nat_punch_endpoint_format(&info->remote, remote), info->local_cid, info->peer_cid,
        nat_punch_encryption_name(info->encryption));
    if (utp_context_accept(app->context) != UTP_STATUS_OK) {
        LOG("nat_punch peer=%s [ConnectionRejected] reason=accept_failed", app->peer_id);
        return false;
    }
    LOG("nat_punch peer=%s [ConnectionAccepted]", app->peer_id);
    return true;
}

static void nat_punch_connection_error(utp_connection_t* connection, const utp_connection_error_info_t* info,
                                       void* user_data)
{
    NatPunchApp*                 app = static_cast<NatPunchApp*>(user_data);
    utp_connection_description_t description;
    const char*                  remote      = "<unknown>";
    uint16_t                     remote_port = 0u;

    if (utp_connection_get_description(connection, &description) == UTP_STATUS_OK) {
        remote      = description.remote_host;
        remote_port = description.remote_port;
    }
    if (info != NULL) {
        LOG("nat_punch peer=%s <- Peer=%s:%" PRIu16 " [ConnectionError] status=%s peer_error=%" PRIu16
            " peer_initiated=%s reason=%.*s",
            app->peer_id, remote, remote_port, utp_status_string(info->status), info->peer_error_code,
            info->peer_initiated ? "true" : "false", static_cast<int>(info->reason_length),
            info->reason == NULL ? "" : reinterpret_cast<const char*>(info->reason));
    } else {
        LOG("nat_punch peer=%s [ConnectionError] status=unknown", app->peer_id);
    }

    if (!app->finished && (info == NULL || info->status != UTP_STATUS_OK))
        nat_punch_fail(app, info == NULL ? "connection_error" : utp_status_string(info->status));
}

static void nat_punch_registered(utp_context_t*, const utp_ntrs_registered_info_t* info, void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);
    char         endpoint[80];

    LOG("nat_punch peer=%s <- NTRS=%s [Registered]", app->peer_id,
        nat_punch_endpoint_format(&info->ntrs_endpoint, endpoint));
}

static void nat_punch_start_actions(NatPunchApp* app)
{
    utp_status_t status;

    if (app->register_requested && !app->register_started) {
        app->register_started = true;
        LOG("nat_punch peer=%s -> NTRS=%s:%" PRIu16 " [Register]", app->peer_id, app->ntrs_address, app->ntrs_port);
        status = utp_context_register_ntrs(app->context, &app->register_options, nat_punch_registered, app);
        if (status != UTP_STATUS_OK) {
            nat_punch_fail(app, "register_ntrs_failed");
            return;
        }
    }
    if (app->target_peer_id != NULL && !app->connect_started) {
        app->connect_started = true;
        LOG("nat_punch peer=%s -> %s=%s:%" PRIu16 " [%s] target=%s encryption=%s", app->peer_id,
            app->rendezvous_connect ? "NTRS" : "Peer", app->connect_options.address, app->connect_options.port,
            app->rendezvous_connect ? "RendezvousConnect" : "Connect", app->target_peer_id,
            nat_punch_encryption_name(app->connect_options.encryption));
        status = utp_context_connect(app->context, &app->connect_options);
        if (status != UTP_STATUS_OK) nat_punch_fail(app, "connect_start_failed");
    }
    if (!app->register_requested && !app->listen_requested && app->target_peer_id == NULL)
        nat_punch_finish(app, "nat_probe_complete");
}

static void nat_punch_nat_complete(utp_context_t*, utp_status_t status, const utp_nat_probe_result_t* result,
                                   void* user_data)
{
    NatPunchApp* app = static_cast<NatPunchApp*>(user_data);

    app->nat_finished = true;
    if (status == UTP_STATUS_OK && result != NULL) {
        char primary[80];
        char secondary[80];

        LOG("nat_punch peer=%s [NatDetected] class=%s primary=%s secondary=%s samples=%u primary_rtt_ms=%" PRId32
            " secondary_rtt_ms=%" PRId32,
            app->peer_id, nat_punch_nat_name(result->nat_class),
            nat_punch_endpoint_format(&result->primary_mapped_endpoint, primary),
            nat_punch_endpoint_format(&result->secondary_mapped_endpoint, secondary),
            static_cast<unsigned>(result->port_sample_count), result->primary_rtt_ms, result->secondary_rtt_ms);
    } else {
        LOG("nat_punch nat_probe_failed status=%s; continue as UNKNOWN", utp_status_string(status));
        if (!app->register_requested && !app->listen_requested && app->target_peer_id == NULL) {
            nat_punch_fail(app, "nat_probe_failed");
            return;
        }
    }
    nat_punch_start_actions(app);
}

static void nat_punch_timeout(evutil_socket_t, short, void* user_data)
{
    nat_punch_fail(static_cast<NatPunchApp*>(user_data), "timeout");
}

static void nat_punch_signal(evutil_socket_t, short, void* user_data)
{
    nat_punch_fail(static_cast<NatPunchApp*>(user_data), "signal");
}

int main(int argc, char** argv)
{
    std::string           peer_id;
    std::string           nat_address;
    std::string           ntrs_address;
    std::string           peer_address;
    std::string           bind_address;
    std::string           interface_name;
    std::string           target_peer_id;
    uint16_t              nat_port           = 24001u;
    uint16_t              ntrs_port          = 24000u;
    uint16_t              peer_port          = 0u;
    uint16_t              bind_port          = 0u;
    uint64_t              send_bytes         = 0u;
    uint64_t              timeout_ms         = 60000u;
    uint64_t              nat_timeout_ms     = 3000u;
    utp_encryption_mode_t encryption         = UTP_ENCRYPTION_NONE;
    bool                  register_requested = false;
    bool                  listen_requested   = false;
    bool                  use_ipv6           = false;
    bool                  has_direct_peer    = false;
    bool                  rendezvous_connect = false;
    int                   index;

    for (index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "-i") == 0 || strcmp(argument, "--peer-id") == 0) {
            if (++index >= argc) goto usage;
            peer_id = argv[index];
        } else if (strcmp(argument, "-n") == 0 || strcmp(argument, "--nat-address") == 0) {
            if (++index >= argc) goto usage;
            nat_address = argv[index];
        } else if (strcmp(argument, "-N") == 0 || strcmp(argument, "--nat-port") == 0) {
            if (++index >= argc || !nat_punch_parse_u16(argv[index], &nat_port)) goto usage;
        } else if (strcmp(argument, "-s") == 0 || strcmp(argument, "--ntrs-address") == 0) {
            if (++index >= argc) goto usage;
            ntrs_address = argv[index];
        } else if (strcmp(argument, "-S") == 0 || strcmp(argument, "--ntrs-port") == 0) {
            if (++index >= argc || !nat_punch_parse_u16(argv[index], &ntrs_port)) goto usage;
        } else if (strcmp(argument, "-t") == 0 || strcmp(argument, "--target-peer-id") == 0) {
            if (++index >= argc) goto usage;
            target_peer_id = argv[index];
        } else if (strcmp(argument, "-a") == 0 || strcmp(argument, "--peer-address") == 0) {
            if (++index >= argc) goto usage;
            peer_address = argv[index];
        } else if (strcmp(argument, "-p") == 0 || strcmp(argument, "--peer-port") == 0) {
            if (++index >= argc || !nat_punch_parse_u16(argv[index], &peer_port)) goto usage;
        } else if (strcmp(argument, "-b") == 0 || strcmp(argument, "--bind-ip") == 0) {
            if (++index >= argc) goto usage;
            bind_address = argv[index];
        } else if (strcmp(argument, "-P") == 0 || strcmp(argument, "--bind-port") == 0) {
            if (++index >= argc || !nat_punch_parse_u16(argv[index], &bind_port)) goto usage;
        } else if (strcmp(argument, "-I") == 0 || strcmp(argument, "--bind-interface") == 0 ||
                   strcmp(argument, "--interface") == 0) {
            if (++index >= argc) goto usage;
            interface_name = argv[index];
        } else if (strcmp(argument, "-d") == 0 || strcmp(argument, "--send-bytes") == 0) {
            if (++index >= argc || !nat_punch_parse_u64(argv[index], &send_bytes)) goto usage;
        } else if (strcmp(argument, "-T") == 0 || strcmp(argument, "--timeout-ms") == 0) {
            if (++index >= argc || !nat_punch_parse_u64(argv[index], &timeout_ms)) goto usage;
        } else if (strcmp(argument, "-M") == 0 || strcmp(argument, "--nat-timeout-ms") == 0) {
            if (++index >= argc || !nat_punch_parse_u64(argv[index], &nat_timeout_ms)) goto usage;
        } else if (strcmp(argument, "-e") == 0 || strcmp(argument, "--encryption") == 0) {
            if (++index >= argc || !nat_punch_parse_encryption(argv[index], &encryption)) goto usage;
        } else if (strcmp(argument, "-r") == 0 || strcmp(argument, "--register") == 0) {
            register_requested = true;
        } else if (strcmp(argument, "-l") == 0 || strcmp(argument, "--listen") == 0) {
            listen_requested = true;
        } else if (strcmp(argument, "-6") == 0 || strcmp(argument, "--ipv6") == 0) {
            use_ipv6 = true;
        } else {
            goto usage;
        }
    }
    has_direct_peer    = !peer_address.empty() || peer_port != 0u;
    rendezvous_connect = !target_peer_id.empty() && !has_direct_peer;

    if (peer_id.empty() || peer_id.size() > UTP_PEER_ID_MAX_LENGTH || nat_address.empty() || nat_port == 0u ||
        ((register_requested || rendezvous_connect) && (ntrs_address.empty() || ntrs_port == 0u)) ||
        target_peer_id.size() > UTP_PEER_ID_MAX_LENGTH ||
        (has_direct_peer && (peer_address.empty() || peer_port == 0u)) || (target_peer_id.empty() && has_direct_peer) ||
        (listen_requested && (!target_peer_id.empty() || register_requested)) || timeout_ms == 0u ||
        nat_timeout_ms == 0u)
        goto usage;

    {
        const int               family = use_ipv6 ? AF_INET6 : AF_INET;
        std::string             nat_numeric;
        std::string             ntrs_numeric;
        std::string             peer_numeric;
        std::string             bind_numeric;
        const char*             bind_ip;
        event_base*             base            = NULL;
        NatPunchApp             app             = {};
        utp_context_options_t   context_options = UTP_CONTEXT_OPTIONS_INIT;
        utp_nat_probe_options_t probe_options   = UTP_NAT_PROBE_OPTIONS_INIT;
        uint16_t                actual_port     = 0u;
        utp_status_t            status;

        const bool              ntrs_required = register_requested || rendezvous_connect;
        if (!nat_punch_resolve(nat_address, family, &nat_numeric) ||
            (ntrs_required && !nat_punch_resolve(ntrs_address, family, &ntrs_numeric)) ||
            (has_direct_peer && !nat_punch_resolve(peer_address, family, &peer_numeric))) {
            LOG("nat_punch address_resolve_failed");
            return EXIT_FAILURE;
        }
        if (bind_address.empty()) {
            bind_ip = use_ipv6 ? "::" : "0.0.0.0";
        } else {
            if (!nat_punch_resolve(bind_address, family, &bind_numeric)) {
                LOG("nat_punch bind_address_resolve_failed");
                return EXIT_FAILURE;
            }
            bind_ip = bind_numeric.c_str();
        }
        base = event_base_new();
        if (base == NULL) {
            LOG("nat_punch event_base_create_failed");
            return EXIT_FAILURE;
        }
        context_options.event_base = base;
        context_options.peer_id    = peer_id.c_str();
        context_options.mtu_min    = 1280u;
        context_options.mtu_base   = 1400u;
        context_options.mtu_max    = 1500u;
        status                     = utp_context_create(&context_options, &app.context);
        if (status == UTP_STATUS_OK) {
            status = utp_context_bind(app.context, bind_ip, bind_port,
                                      interface_name.empty() ? NULL : interface_name.c_str(), &actual_port);
        }
        if (status != UTP_STATUS_OK) {
            LOG("nat_punch context_setup_failed status=%s", utp_status_string(status));
            if (app.context != NULL) utp_context_destroy(app.context);
            event_base_free(base);
            return EXIT_FAILURE;
        }
        app.base                                       = base;
        app.peer_id                                    = peer_id.c_str();
        app.ntrs_address                               = ntrs_required ? ntrs_numeric.c_str() : NULL;
        app.target_peer_id                             = target_peer_id.empty() ? NULL : target_peer_id.c_str();
        app.ntrs_port                                  = ntrs_port;
        app.send_bytes                                 = send_bytes;
        app.register_requested                         = register_requested;
        app.listen_requested                           = listen_requested;
        app.rendezvous_connect                         = rendezvous_connect;
        app.initiator                                  = !target_peer_id.empty();
        app.start_ms                                   = nat_punch_now_ms();
        app.register_options                           = utp_ntrs_register_options_t();
        app.register_options.ntrs_address              = app.ntrs_address;
        app.register_options.ntrs_port                 = ntrs_port;
        app.register_options.timeout_ms                = 1000u;
        app.register_options.retries                   = 3u;
        app.register_options.address_update_timeout_ms = 1000u;
        app.register_options.address_update_retries    = 3u;
        app.register_options.keepalive_interval_ms     = 30000u;
        app.register_options.keepalive_timeout_ms      = 3000u;
        app.connect_options                            = utp_connect_options_t();
        app.connect_options.address =
            target_peer_id.empty() ? NULL : (rendezvous_connect ? ntrs_numeric.c_str() : peer_numeric.c_str());
        app.connect_options.target_peer_id = app.target_peer_id;
        app.connect_options.port           = rendezvous_connect ? ntrs_port : peer_port;
        app.connect_options.timeout_ms     = 1000u;
        app.connect_options.retries        = 5;
        app.connect_options.encryption     = encryption;
        app.request_length =
            static_cast<size_t>(snprintf(app.request, sizeof(app.request), "DATA %" PRIu64 "\n", send_bytes));
        if (app.request_length >= sizeof(app.request)) {
            LOG("nat_punch request_format_failed");
            utp_context_destroy(app.context);
            event_base_free(base);
            return EXIT_FAILURE;
        }
        utp_context_set_on_connected(app.context, nat_punch_connected, &app);
        utp_context_set_on_connect_error(
            app.context,
            [](utp_status_t error_status, const char* message, const utp_connect_attempt_info_t* attempt, void* data) {
                NatPunchApp* callback_app = static_cast<NatPunchApp*>(data);
                char         remote[80];

                LOG("nat_punch peer=%s <- %s=%s [ConnectError] status=%s reason=%s", callback_app->peer_id,
                    callback_app->rendezvous_connect ? "NTRS" : "Peer",
                    nat_punch_endpoint_format(&attempt->remote, remote), utp_status_string(error_status), message);
                nat_punch_fail(callback_app, "connect_error");
            },
            &app);
        utp_context_set_on_new_connection(app.context, nat_punch_new_connection, &app);
        utp_context_set_on_connection_error(app.context, nat_punch_connection_error, &app);
        probe_options.nat_service_address = nat_numeric.c_str();
        probe_options.nat_service_port    = nat_port;
        probe_options.phase_timeout_ms    = static_cast<uint32_t>(std::min<uint64_t>(nat_timeout_ms, UINT32_MAX));
        app.timeout_event = (app.initiator || app.listen_requested) ? evtimer_new(base, nat_punch_timeout, &app) : NULL;
        if (app.timeout_event != NULL) {
            timeval timeout = {static_cast<time_t>(timeout_ms / 1000u),
                               static_cast<suseconds_t>((timeout_ms % 1000u) * 1000u)};
            event_add(app.timeout_event, &timeout);
        }
        app.signal_int  = evsignal_new(base, SIGINT, nat_punch_signal, &app);
        app.signal_term = evsignal_new(base, SIGTERM, nat_punch_signal, &app);
        if (app.signal_int == NULL || app.signal_term == NULL || event_add(app.signal_int, NULL) != 0 ||
            event_add(app.signal_term, NULL) != 0) {
            LOG("nat_punch signal_setup_failed");
            return EXIT_FAILURE;
        }
        LOG("nat_punch peer=%s [Bound] local=%s:%" PRIu16 " interface=%s mode=%s nat=%s:%" PRIu16 " ntrs=%s:%" PRIu16,
            app.peer_id, bind_ip, actual_port, interface_name.empty() ? "<any>" : interface_name.c_str(),
            register_requested
                ? "register"
                : (rendezvous_connect ? "rendezvous-connect" : (listen_requested ? "listen" : "nat-probe")),
            nat_numeric.c_str(), nat_port, ntrs_required ? ntrs_numeric.c_str() : "<none>",
            ntrs_required ? ntrs_port : 0u);
        LOG("nat_punch peer=%s -> NAT=%s:%" PRIu16 " [PrimaryBinding]", app.peer_id, nat_numeric.c_str(), nat_port);
        status = utp_context_probe_nat(app.context, &probe_options, nat_punch_nat_complete, &app);
        if (status != UTP_STATUS_OK) {
            LOG("nat_punch nat_probe_start_failed status=%s; continue as UNKNOWN", utp_status_string(status));
            if (!app.nat_finished) nat_punch_nat_complete(app.context, status, NULL, &app);
        }
        if (!app.finished) event_base_dispatch(base);
        if (!app.finished && (app.initiator || app.listen_requested)) nat_punch_fail(&app, "event_loop_stopped");
        if (app.signal_int != NULL) event_free(app.signal_int);
        if (app.signal_term != NULL) event_free(app.signal_term);
        if (app.timeout_event != NULL) event_free(app.timeout_event);
        utp_context_destroy(app.context);
        event_base_free(base);
        return app.failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }

usage:
    nat_punch_usage(argv[0]);
    return 2;
}
