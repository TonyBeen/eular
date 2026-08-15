#define CATCH_CONFIG_MAIN

#include <algorithm>
#include <array>
#if defined(__APPLE__)
#include <cerrno>
#endif
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <catch2/catch.hpp>
#include <event2/event.h>

extern "C" {
#include <utp/context.h>

#include "connection/connection.h"
#if defined(__APPLE__)
#include "context/context.h"
#endif
#include "context/event_loop.h"
#include "proto/ack.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "socket/address.h"
#include "socket/udp.h"
#if defined(__APPLE__)
#include "socket_send_hook.h"
#endif
#include "util/error.h"
}

namespace {

constexpr size_t kRelayDatagramCapacity = 2048u;
constexpr auto   kDriveTimeout          = std::chrono::milliseconds(2000);

enum class relay_direction : uint8_t { client_to_server, server_to_client };
enum class relay_action : uint8_t { drop, duplicate, hold, corrupt };

struct relay_rule {
    relay_direction direction;                             // 规则匹配的转发方向
    relay_action    action;                                // 命中后的故障动作
    uint8_t         packet_type;                           // 0 表示不限制包类型
    uint32_t        required_frames;                       // 明文帧类型位图，0 表示不限制
    bool            pending;                               // 是否仍可命中一次
    uint32_t        hits;                                  // 命中次数
    bool            match_stream_ack;                      // 是否只匹配确认已观测 STREAM 包号的 ACK
    bool            drop_stream_ack_until_retransmission;  // 是否持续丢弃目标 ACK，直到 STREAM 重传
};

struct udp_relay {
    struct event_base*                          event_base                    = nullptr;
    utp_event_loop_t                            event_loop                    = {};
    utp_event_t                                 read_event                    = {};
    utp_event_t                                 migration_read_event          = {};
    utp_udp_socket_t                            socket                        = {};
    utp_udp_socket_t                            migration_socket              = {};
    utp_address_t                               address                       = {};
    utp_address_t                               migration_address             = {};
    utp_address_t                               server                        = {};
    utp_address_t                               client                        = {};
    relay_rule                                  rule                          = {};
    std::array<uint8_t, kRelayDatagramCapacity> held                          = {};
    size_t                                      held_length                   = 0u;
    uint64_t                                    held_packet_number            = 0u;
    utp_address_t                               held_destination              = {};
    uint32_t                                    held_stream_id                = UINT32_MAX;
    uint64_t                                    held_stream_offset            = 0u;
    bool                                        client_known                  = false;
    bool                                        held_valid                    = false;
    uint32_t                                    forwarded_stream_packets      = 0u;
    uint64_t                                    stream_packet_number          = 0u;
    uint32_t                                    stream_packet_transmissions   = 0u;
    uint32_t                                    forwarded_stream_acks         = 0u;
    uint32_t                                    forwarded_client_frame_types  = 0u;
    uint32_t                                    forwarded_server_frame_types  = 0u;
    uint64_t                                    target_packet_number          = 0u;
    bool                                        held_replayed_after_successor = false;
    bool                                        migration_enabled             = false;
    bool                                        held_manual                   = false;  // 是否仅允许测试显式释放持有包
};

static bool relay_acknowledges_packet(const uint8_t* packet, size_t packet_length, uint64_t packet_number)
{
    utp_packet_view_t view         = {};
    size_t            frame_offset = 0u;

    if (packet_number == 0u || utp_packet_view_decode(&view, packet, packet_length) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    while (frame_offset < view.payload_length) {
        const uint8_t*       frame;
        uint8_t              frame_type;
        size_t               frame_length;
        utp_internal_error_t error =
            utp_packet_view_next_frame(&view, &frame_offset, &frame_type, &frame, &frame_length);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return false;
        }
        if (frame_type == UTP_FRAME_TYPE_ACK) {
            std::array<utp_ack_range_t, UTP_ACK_MAX_RANGES> ranges   = {};
            utp_ack_info_t                                  ack      = {0u, 0u, ranges.data(), 0u, ranges.size()};
            size_t                                          consumed = 0u;

            if (utp_ack_decode(&ack, frame, frame_length, 0u, &consumed) != UTP_INTERNAL_ERROR_OK ||
                consumed != frame_length) {
                return false;
            }
            for (size_t index = 0u; index < ack.range_count; ++index) {
                if (packet_number >= ack.ranges[index].low && packet_number <= ack.ranges[index].high) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool relay_find_stream(const uint8_t* packet, size_t packet_length, uint32_t* stream_id, uint64_t* offset)
{
    utp_packet_view_t view         = {};
    size_t            frame_offset = 0u;

    if (utp_packet_view_decode(&view, packet, packet_length) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    while (frame_offset < view.payload_length) {
        const uint8_t*       frame;
        uint8_t              frame_type;
        size_t               frame_length;
        utp_internal_error_t error =
            utp_packet_view_next_frame(&view, &frame_offset, &frame_type, &frame, &frame_length);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return false;
        }
        if (frame_type == UTP_FRAME_TYPE_STREAM) {
            utp_frame_stream_t stream = {};

            if (utp_frame_stream_decode(&stream, frame, frame_length) != UTP_INTERNAL_ERROR_OK) {
                return false;
            }
            *stream_id = stream.stream_id;
            *offset    = stream.offset;
            return true;
        }
    }
    return false;
}

struct endpoint_probe {
    utp_context_t*    context           = nullptr;
    utp_connection_t* connection        = nullptr;
    utp_stream_t*     incoming_stream   = nullptr;
    int32_t           new_connections   = 0;
    int32_t           connected         = 0;
    int32_t           connect_errors    = 0;
    int32_t           connection_errors = 0;
};

struct stream_send_probe {
    const uint8_t* data               = nullptr;
    size_t         length             = 0u;
    size_t         offset             = 0u;
    int32_t        writable_callbacks = 0;
    utp_status_t   status             = UTP_STATUS_OK;
    bool           write_shutdown     = false;
};

struct session_token_probe {
    std::array<uint8_t, 256u> token       = {};
    size_t                    length      = 0u;
    int32_t                   ready_count = 0;
};

struct transport_pair {
    struct event_base* event_base   = nullptr;
    utp_context_t*     client       = nullptr;
    utp_context_t*     server       = nullptr;
    endpoint_probe     client_probe = {};
    endpoint_probe     server_probe = {};
    udp_relay          relay        = {};
};

static bool on_new_connection(const utp_new_connection_info_t* info, void* user_data)
{
    auto* probe = static_cast<endpoint_probe*>(user_data);

    REQUIRE(info != nullptr);
    ++probe->new_connections;
    return utp_context_accept(probe->context) == UTP_STATUS_OK;
}

static void on_connected(utp_connection_t* connection, void* user_data)
{
    auto* probe = static_cast<endpoint_probe*>(user_data);

    REQUIRE(connection != nullptr);
    ++probe->connected;
    probe->connection = connection;
}

static void on_incoming_stream(utp_connection_t* connection, utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<endpoint_probe*>(user_data);

    REQUIRE(connection != nullptr);
    REQUIRE(stream != nullptr);
    probe->incoming_stream = stream;
}

static void on_stream_writable(utp_stream_t* stream, void* user_data)
{
    auto* probe = static_cast<stream_send_probe*>(user_data);

    REQUIRE(stream != nullptr);
    REQUIRE(probe != nullptr);
    ++probe->writable_callbacks;
    while (probe->offset < probe->length) {
        const size_t chunk_length = std::min(static_cast<size_t>(8192u), probe->length - probe->offset);

        probe->status = utp_stream_write(stream, probe->data + probe->offset, chunk_length);
        if (probe->status == UTP_STATUS_OK) {
            probe->offset += chunk_length;
            continue;
        }
        if (probe->status == UTP_STATUS_WOULD_BLOCK) {
            probe->status = UTP_STATUS_OK;
        }
        return;
    }
    if (!probe->write_shutdown) {
        probe->status = utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE);
        if (probe->status == UTP_STATUS_OK) {
            probe->write_shutdown = true;
        }
    }
}

static void on_session_token_ready(utp_connection_t* connection, void* user_data)
{
    auto* probe = static_cast<session_token_probe*>(user_data);

    REQUIRE(connection != nullptr);
    REQUIRE(probe != nullptr);
    ++probe->ready_count;
    REQUIRE(utp_connection_export_session_token(connection, probe->token.data(), probe->token.size(), &probe->length) ==
            UTP_STATUS_OK);
}

static void on_connection_error(utp_connection_t* connection, const utp_connection_error_info_t* info, void* user_data)
{
    auto* probe = static_cast<endpoint_probe*>(user_data);

    REQUIRE(connection != nullptr);
    REQUIRE(info != nullptr);
    ++probe->connection_errors;
}

static void on_connect_error(utp_status_t status, const char* message, const utp_connect_attempt_info_t* attempt,
                             void* user_data)
{
    auto* probe = static_cast<endpoint_probe*>(user_data);

    REQUIRE(status != UTP_STATUS_OK);
    REQUIRE(message != nullptr);
    REQUIRE(attempt != nullptr);
    ++probe->connect_errors;
}

static bool relay_packet_matches(const udp_relay& relay, relay_direction direction, const uint8_t* packet,
                                 size_t packet_length)
{
    utp_packet_header_t header = {};

    if (!relay.rule.pending || relay.rule.direction != direction ||
        utp_proto_decode_header(&header, packet, packet_length) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    if (relay.rule.packet_type != 0u && header.type != relay.rule.packet_type) {
        return false;
    }
    if (relay.target_packet_number != 0u && header.packet_number != relay.target_packet_number) {
        return false;
    }
    if (relay.rule.required_frames != 0u) {
        utp_packet_view_t view = {};

        if (utp_packet_view_decode(&view, packet, packet_length) != UTP_INTERNAL_ERROR_OK ||
            (view.frame_types & relay.rule.required_frames) != relay.rule.required_frames) {
            return false;
        }
    }
    if (relay.rule.match_stream_ack && !relay_acknowledges_packet(packet, packet_length, relay.stream_packet_number)) {
        return false;
    }
    if (relay.rule.drop_stream_ack_until_retransmission && relay.stream_packet_transmissions >= 2u) {
        return false;
    }
    return true;
}

static void relay_forward_from(utp_udp_socket_t* socket, const uint8_t* packet, size_t packet_length,
                               const utp_address_t* destination)
{
    size_t sent_length = 0u;

    REQUIRE(utp_udp_socket_send_to(socket, packet, packet_length, destination, &sent_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == packet_length);
}

static void relay_forward(udp_relay* relay, const uint8_t* packet, size_t packet_length,
                          const utp_address_t* destination)
{
    relay_forward_from(&relay->socket, packet, packet_length, destination);
}

static void relay_on_readable_from(udp_relay* relay, utp_udp_socket_t* receive_socket, bool migration_socket,
                                   uint32_t events)
{
    utp_udp_socket_t* send_socket;

    if ((events & UTP_EVENT_READABLE) == 0u) {
        return;
    }
    for (;;) {
        std::array<uint8_t, kRelayDatagramCapacity> packet        = {};
        utp_address_t                               source        = {};
        size_t                                      packet_length = 0u;
        utp_internal_error_t                        error;

        error = utp_udp_socket_recv_from(receive_socket, packet.data(), packet.size(), &packet_length, &source);
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            return;
        }
        REQUIRE(error == UTP_INTERNAL_ERROR_OK);
        const relay_direction direction = utp_address_equal(&source, &relay->server)
                                              ? relay_direction::server_to_client
                                              : relay_direction::client_to_server;
        const utp_address_t*  destination;
        bool                  target_stream_ack;

        if (direction == relay_direction::client_to_server) {
            relay->client       = source;
            relay->client_known = true;
            destination         = &relay->server;
            send_socket         = relay->migration_enabled ? &relay->migration_socket : &relay->socket;
        } else {
            REQUIRE(relay->client_known);
            destination = &relay->client;
            // 服务端发往候选 relay 的验证报文，必须经原 relay 返回，避免客户端也误判路径迁移。
            send_socket = migration_socket ? &relay->socket : receive_socket;
        }
        if (direction == relay_direction::client_to_server) {
            utp_packet_view_t view = {};

            if (utp_packet_view_decode(&view, packet.data(), packet_length) == UTP_INTERNAL_ERROR_OK &&
                (view.frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u) {
                if (relay->stream_packet_number == 0u) {
                    relay->stream_packet_number = view.header.packet_number;
                }
                ++relay->stream_packet_transmissions;
            }
        }
        target_stream_ack = direction == relay_direction::server_to_client && relay->rule.match_stream_ack &&
                            relay_acknowledges_packet(packet.data(), packet_length, relay->stream_packet_number);
        if (relay_packet_matches(*relay, direction, packet.data(), packet_length)) {
            if (!relay->rule.drop_stream_ack_until_retransmission) {
                relay->rule.pending = false;
            }
            ++relay->rule.hits;
            if (relay->rule.action == relay_action::drop) {
                continue;
            }
            if (relay->rule.action == relay_action::hold) {
                utp_packet_header_t header = {};

                // 加密包无法由 relay 解出 STREAM 标识；测试在重传前释放该包，因此无需抑制同一分片的重传。
                (void)relay_find_stream(packet.data(), packet_length, &relay->held_stream_id,
                                        &relay->held_stream_offset);
                REQUIRE(utp_proto_decode_header(&header, packet.data(), packet_length) == UTP_INTERNAL_ERROR_OK);
                REQUIRE(packet_length <= relay->held.size());
                std::copy(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(packet_length),
                          relay->held.begin());
                relay->held_length        = packet_length;
                relay->held_packet_number = header.packet_number;
                relay->held_destination   = *destination;
                relay->held_valid         = true;
                relay->held_manual = (relay->rule.required_frames & UTP_FRAME_BIT(UTP_FRAME_TYPE_PATH_RESPONSE)) != 0u;
                continue;
            }
            if (relay->rule.action == relay_action::corrupt) {
                REQUIRE(packet_length > 0u);
                packet[packet_length - 1u] ^= UINT8_C(0x01);
                relay_forward_from(send_socket, packet.data(), packet_length, destination);
                continue;
            }
            relay_forward_from(send_socket, packet.data(), packet_length, destination);
            relay_forward_from(send_socket, packet.data(), packet_length, destination);
            continue;
        }
        if (relay->held_valid && !relay->held_manual && relay->held_stream_id == UINT32_MAX &&
            direction == relay_direction::client_to_server) {
            // 加密包无法识别分片偏移；同轮先转发后继包再释放首包，使接收端观察到确定的乱序。
            relay_forward_from(send_socket, packet.data(), packet_length, destination);
            relay_forward_from(send_socket, relay->held.data(), relay->held_length, &relay->held_destination);
            relay->held_valid                    = false;
            relay->held_replayed_after_successor = true;
            continue;
        }
        if (relay->held_valid && !relay->held_manual && relay->held_stream_id != UINT32_MAX &&
            direction == relay_direction::client_to_server) {
            uint32_t stream_id;
            uint64_t stream_offset;

            if (relay_find_stream(packet.data(), packet_length, &stream_id, &stream_offset) &&
                stream_id == relay->held_stream_id && stream_offset == relay->held_stream_offset) {
                continue;
            }
        }
        {
            utp_packet_view_t view = {};

            if (direction == relay_direction::client_to_server &&
                utp_packet_view_decode(&view, packet.data(), packet_length) == UTP_INTERNAL_ERROR_OK &&
                (view.frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u) {
                ++relay->forwarded_stream_packets;
            }
        }
        if (target_stream_ack) {
            ++relay->forwarded_stream_acks;
        }
        if (direction == relay_direction::client_to_server) {
            utp_packet_view_t view = {};

            if (utp_packet_view_decode(&view, packet.data(), packet_length) == UTP_INTERNAL_ERROR_OK) {
                relay->forwarded_client_frame_types |= view.frame_types;
            }
        } else {
            utp_packet_view_t view = {};

            if (utp_packet_view_decode(&view, packet.data(), packet_length) == UTP_INTERNAL_ERROR_OK) {
                relay->forwarded_server_frame_types |= view.frame_types;
            }
        }
        relay_forward_from(send_socket, packet.data(), packet_length, destination);
    }
}

static void relay_on_readable(uint32_t events, void* user_data)
{
    auto* relay = static_cast<udp_relay*>(user_data);

    relay_on_readable_from(relay, &relay->socket, false, events);
}

static void relay_on_migration_readable(uint32_t events, void* user_data)
{
    auto* relay = static_cast<udp_relay*>(user_data);

    relay_on_readable_from(relay, &relay->migration_socket, true, events);
}

static void relay_init(udp_relay* relay, struct event_base* event_base, uint16_t server_port)
{
    utp_address_t requested = {};

    relay->event_base = event_base;
    REQUIRE(utp_address_parse(&requested, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_parse(&relay->server, "127.0.0.1", server_port) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&relay->socket);
    utp_udp_socket_init(&relay->migration_socket);
    utp_event_init(&relay->read_event);
    utp_event_init(&relay->migration_read_event);
    REQUIRE(utp_udp_socket_open(&relay->socket, requested.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&relay->socket, &requested, nullptr, &relay->address) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_loop_init(&relay->event_loop, event_base, nullptr, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_add_udp(&relay->event_loop, &relay->read_event, &relay->socket, UTP_EVENT_READABLE, true,
                              relay_on_readable, relay) == UTP_INTERNAL_ERROR_OK);
}

/** @brief 创建第二 relay 源端口，后续客户端报文将通过它发送给服务端以模拟 NAT 重绑定。 */
static void relay_enable_migration(udp_relay* relay)
{
    utp_address_t requested = {};

    REQUIRE(!relay->migration_enabled);
    REQUIRE(utp_address_parse(&requested, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_open(&relay->migration_socket, requested.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&relay->migration_socket, &requested, nullptr, &relay->migration_address) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_add_udp(&relay->event_loop, &relay->migration_read_event, &relay->migration_socket,
                              UTP_EVENT_READABLE, true, relay_on_migration_readable, relay) == UTP_INTERNAL_ERROR_OK);
    relay->migration_enabled = true;
}

static void relay_release_held(udp_relay* relay)
{
    REQUIRE(relay->held_valid);
    relay_forward(relay, relay->held.data(), relay->held_length, &relay->held_destination);
    relay->held_valid  = false;
    relay->held_manual = false;
}

static void relay_cleanup(udp_relay* relay)
{
    if (relay->migration_enabled) {
        utp_event_remove(&relay->migration_read_event);
        utp_udp_socket_close(&relay->migration_socket);
    }
    utp_event_remove(&relay->read_event);
    utp_event_loop_close(&relay->event_loop);
    utp_udp_socket_close(&relay->socket);
}

template <typename predicate>
static void drive_until(struct event_base* event_base, predicate complete)
{
    const auto deadline = std::chrono::steady_clock::now() + kDriveTimeout;

    while (!complete() && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(event_base_loop(event_base, EVLOOP_NONBLOCK) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(complete());
}

static void drive_for(struct event_base* event_base, std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < deadline) {
        REQUIRE(event_base_loop(event_base, EVLOOP_NONBLOCK) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/** @brief 使用调用方提供的 Context 选项初始化回环传输对。 */
static void transport_pair_init_with_options(transport_pair* pair, relay_rule rule, utp_encryption_mode_t encryption,
                                             const utp_context_options_t* client_options,
                                             const utp_context_options_t* server_options)
{
    utp_context_options_t client_opts     = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t server_opts     = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;
    uint16_t              server_port     = 0u;

    if (client_options != nullptr) {
        client_opts = *client_options;
    }
    if (server_options != nullptr) {
        server_opts = *server_options;
    }
    pair->event_base = event_base_new();
    REQUIRE(pair->event_base != nullptr);
    client_opts.event_base = pair->event_base;
    client_opts.context_id = client_opts.context_id == 0u ? 5001u : client_opts.context_id;
    server_opts.event_base = pair->event_base;
    server_opts.context_id = server_opts.context_id == 0u ? 5002u : server_opts.context_id;
    REQUIRE(utp_context_create(&client_opts, &pair->client) == UTP_STATUS_OK);
    REQUIRE(utp_context_create(&server_opts, &pair->server) == UTP_STATUS_OK);
    pair->server_probe.context = pair->server;
    REQUIRE(utp_context_bind(pair->client, "127.0.0.1", 0u, nullptr, nullptr) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(pair->server, "127.0.0.1", 0u, nullptr, &server_port) == UTP_STATUS_OK);
    relay_init(&pair->relay, pair->event_base, server_port);
    pair->relay.rule = rule;
    utp_context_set_on_connected(pair->client, on_connected, &pair->client_probe);
    utp_context_set_on_connected(pair->server, on_connected, &pair->server_probe);
    utp_context_set_on_new_connection(pair->server, on_new_connection, &pair->server_probe);
    utp_context_set_on_connection_error(pair->client, on_connection_error, &pair->client_probe);
    utp_context_set_on_connection_error(pair->server, on_connection_error, &pair->server_probe);
    connect_options.address    = "127.0.0.1";
    connect_options.port       = pair->relay.address.port;
    connect_options.timeout_ms = 10u;
    connect_options.retries    = 3;
    connect_options.encryption = encryption;
    REQUIRE(utp_context_connect(pair->client, &connect_options) == UTP_STATUS_OK);
}

static void transport_pair_init(transport_pair* pair, relay_rule rule, utp_encryption_mode_t encryption)
{
    transport_pair_init_with_options(pair, rule, encryption, nullptr, nullptr);
}

static void transport_pair_connect(transport_pair* pair)
{
    drive_until(pair->event_base,
                [pair] { return pair->client_probe.connected == 1 && pair->server_probe.connected == 1; });
    REQUIRE(pair->client_probe.connection != nullptr);
    REQUIRE(pair->server_probe.connection != nullptr);
    REQUIRE(pair->client_probe.connection_errors == 0);
    REQUIRE(pair->server_probe.connection_errors == 0);
}

static void transport_pair_cleanup(transport_pair* pair)
{
    if (pair->client != nullptr) {
        utp_context_destroy(pair->client);
    }
    if (pair->server != nullptr) {
        utp_context_destroy(pair->server);
    }
    relay_cleanup(&pair->relay);
    event_base_free(pair->event_base);
}

}  // namespace

TEST_CASE("relay drops the first INITIAL and the public handshake retransmits", "[transport][integration]")
{
    transport_pair   pair = {};
    const relay_rule rule = {
        relay_direction::client_to_server, relay_action::drop, UTP_PACKET_TYPE_INITIAL, 0u, true, 0u, false, false};

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    REQUIRE(pair.relay.rule.hits == 1u);
    REQUIRE(pair.server_probe.new_connections == 1);
    transport_pair_cleanup(&pair);
}

TEST_CASE("relay drops the first server HANDSHAKE and the public handshake converges", "[transport][integration]")
{
    transport_pair   pair = {};
    const relay_rule rule = {
        relay_direction::server_to_client, relay_action::drop, UTP_PACKET_TYPE_HANDSHAKE, 0u, true, 0u, false, false};

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    REQUIRE(pair.relay.rule.hits == 1u);
    REQUIRE(pair.server_probe.new_connections == 1);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted handshake retransmission keeps the same accept decision", "[transport][integration]")
{
    transport_pair   pair = {};
    const relay_rule rule = {
        relay_direction::server_to_client, relay_action::drop, UTP_PACKET_TYPE_HANDSHAKE, 0u, true, 0u, false, false};

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    REQUIRE(pair.relay.rule.hits == 1u);
    REQUIRE(pair.server_probe.new_connections == 1);
    transport_pair_cleanup(&pair);
}

TEST_CASE("0-RTT response loss retransmits without duplicate early delivery", "[transport][integration][0rtt]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const relay_rule drop_done = {
        relay_direction::server_to_client, relay_action::drop, UTP_PACKET_TYPE_HANDSHAKE, 0u, true, 0u, false, false};
    const std::array<uint8_t, 9>           early_data    = {'0', '-', 'r', 't', 't', '-', 'd', 'a', 't'};
    session_token_probe                    token         = {};
    endpoint_probe                         early_probe   = {};
    utp_context_options_t                  early_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_0rtt_options_t             early_connect = UTP_CONNECT_0RTT_OPTIONS_INIT;
    utp_context_t*                         early_client  = nullptr;
    utp_stream_t*                          stream;
    std::array<uint8_t, early_data.size()> received        = {};
    size_t                                 received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_session_token_ready(pair.client_probe.connection, on_session_token_ready, &token);
    drive_until(pair.event_base, [&token] { return token.ready_count == 1; });
    REQUIRE(token.length > 0u);

    early_options.event_base = pair.event_base;
    early_options.context_id = 6003u;
    REQUIRE(utp_context_create(&early_options, &early_client) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(early_client, "127.0.0.1", 0u, nullptr, nullptr) == UTP_STATUS_OK);
    utp_context_set_on_connected(early_client, on_connected, &early_probe);
    utp_context_set_on_connection_error(early_client, on_connection_error, &early_probe);
    pair.relay.rule                  = drop_done;
    early_connect.address            = "127.0.0.1";
    early_connect.port               = pair.relay.address.port;
    early_connect.timeout_ms         = 10u;
    early_connect.retries            = 3;
    early_connect.session_token      = token.token.data();
    early_connect.session_token_size = token.length;
    early_connect.early_data         = early_data.data();
    early_connect.early_data_size    = early_data.size();
    early_connect.early_fin          = true;
    REQUIRE(utp_context_connect_0rtt(early_client, &early_connect) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair] { return pair.relay.rule.hits == 1u; });
    drive_until(pair.event_base, [&early_probe] { return early_probe.connected == 1; });
    drive_until(pair.event_base,
                [&pair] { return pair.server_probe.new_connections == 2 && pair.server_probe.connection != nullptr; });
    stream = utp_connection_get_stream(pair.server_probe.connection, 0u);
    REQUIRE(stream != nullptr);
    drive_until(pair.event_base,
                [stream, &early_data] { return utp_stream_readable_bytes(stream) == early_data.size(); });
    REQUIRE(utp_stream_read(stream, received.data(), received.size(), &received_length) == UTP_STATUS_OK);
    REQUIRE(received_length == early_data.size());
    REQUIRE(received == early_data);
    REQUIRE(utp_stream_read(stream, received.data(), received.size(), &received_length) == UTP_STATUS_CLOSED);
    REQUIRE(early_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    utp_context_destroy(early_client);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted 0-RTT continues early stream data after the first packet", "[transport][integration][0rtt]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    std::array<uint8_t, 4096u>             early_data    = {};
    session_token_probe                    token         = {};
    endpoint_probe                         early_probe   = {};
    utp_context_options_t                  early_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_0rtt_options_t             early_connect = UTP_CONNECT_0RTT_OPTIONS_INIT;
    utp_context_t*                         early_client  = nullptr;
    utp_stream_t*                          stream;
    std::array<uint8_t, early_data.size()> received        = {};
    size_t                                 received_length = 0u;

    std::iota(early_data.begin(), early_data.end(), static_cast<uint8_t>(0u));
    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    utp_connection_set_on_session_token_ready(pair.client_probe.connection, on_session_token_ready, &token);
    drive_until(pair.event_base, [&token] { return token.ready_count == 1; });
    REQUIRE(token.length > 0u);

    early_options.event_base = pair.event_base;
    early_options.context_id = 5004u;
    REQUIRE(utp_context_create(&early_options, &early_client) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(early_client, "127.0.0.1", 0u, nullptr, nullptr) == UTP_STATUS_OK);
    utp_context_set_on_connected(early_client, on_connected, &early_probe);
    utp_context_set_on_connection_error(early_client, on_connection_error, &early_probe);
    early_connect.address            = "127.0.0.1";
    early_connect.port               = pair.relay.address.port;
    early_connect.timeout_ms         = 10u;
    early_connect.retries            = 3;
    early_connect.session_token      = token.token.data();
    early_connect.session_token_size = token.length;
    early_connect.early_data         = early_data.data();
    early_connect.early_data_size    = early_data.size();
    early_connect.early_fin          = true;
    REQUIRE(utp_context_connect_0rtt(early_client, &early_connect) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&early_probe] { return early_probe.connected == 1; });
    drive_until(pair.event_base,
                [&pair] { return pair.server_probe.new_connections == 2 && pair.server_probe.connection != nullptr; });
    stream = utp_connection_get_stream(pair.server_probe.connection, 0u);
    REQUIRE(stream != nullptr);
    drive_until(pair.event_base,
                [stream, &early_data] { return utp_stream_readable_bytes(stream) == early_data.size(); });
    REQUIRE(utp_stream_read(stream, received.data(), received.size(), &received_length) == UTP_STATUS_OK);
    REQUIRE(received_length == early_data.size());
    REQUIRE(received == early_data);
    REQUIRE(utp_stream_read(stream, received.data(), received.size(), &received_length) == UTP_STATUS_CLOSED);
    REQUIRE(early_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    utp_context_destroy(early_client);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted 0-RTT rejects a corrupted early ciphertext before accepting", "[transport][integration][0rtt]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const relay_rule corrupt_early = {
        relay_direction::client_to_server, relay_action::corrupt, UTP_PACKET_TYPE_0RTT, 0u, true, 0u, false, false};
    const std::array<uint8_t, 6u> early_data    = {'a', 'u', 't', 'h', '!', '!'};
    session_token_probe           token         = {};
    endpoint_probe                early_probe   = {};
    utp_context_options_t         early_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_0rtt_options_t    early_connect = UTP_CONNECT_0RTT_OPTIONS_INIT;
    utp_context_t*                early_client  = nullptr;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    utp_connection_set_on_session_token_ready(pair.client_probe.connection, on_session_token_ready, &token);
    drive_until(pair.event_base, [&token] { return token.ready_count == 1; });
    REQUIRE(token.length > 0u);

    early_options.event_base = pair.event_base;
    early_options.context_id = 5005u;
    REQUIRE(utp_context_create(&early_options, &early_client) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(early_client, "127.0.0.1", 0u, nullptr, nullptr) == UTP_STATUS_OK);
    utp_context_set_on_connected(early_client, on_connected, &early_probe);
    utp_context_set_on_connect_error(early_client, on_connect_error, &early_probe);
    pair.relay.rule                  = corrupt_early;
    early_connect.address            = "127.0.0.1";
    early_connect.port               = pair.relay.address.port;
    early_connect.timeout_ms         = 10u;
    early_connect.retries            = 0;
    early_connect.session_token      = token.token.data();
    early_connect.session_token_size = token.length;
    early_connect.early_data         = early_data.data();
    early_connect.early_data_size    = early_data.size();
    early_connect.early_fin          = true;
    REQUIRE(utp_context_connect_0rtt(early_client, &early_connect) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&early_probe] { return early_probe.connect_errors == 1; });
    REQUIRE(pair.relay.rule.hits == 1u);
    REQUIRE(early_probe.connected == 0);
    REQUIRE(pair.server_probe.new_connections == 1);
    REQUIRE(pair.server_probe.connection_errors == 0);
    utp_context_destroy(early_client);
    transport_pair_cleanup(&pair);
}

TEST_CASE("path MTU probes reach the configured ceiling over UDP", "[transport][integration][mtu]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    utp_context_options_t      client_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t      server_options   = UTP_CONTEXT_OPTIONS_INIT;
    utp_connection_statistic_t client_statistic = {};
    utp_connection_statistic_t server_statistic = {};

    client_options.mtu_min            = 1280u;
    client_options.mtu_base           = 1400u;
    client_options.mtu_max            = 1450u;
    client_options.mtu_probe_timeout  = 10u;
    client_options.mtu_probe_interval = 1u;
    server_options.mtu_min            = 1280u;
    server_options.mtu_base           = 1400u;
    server_options.mtu_max            = 1450u;
    server_options.mtu_probe_timeout  = 10u;
    server_options.mtu_probe_interval = 1u;
    transport_pair_init_with_options(&pair, no_rule, UTP_ENCRYPTION_NONE, &client_options, &server_options);
    transport_pair_connect(&pair);
    drive_until(pair.event_base, [&pair, &client_statistic, &server_statistic] {
        REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &client_statistic) == UTP_STATUS_OK);
        REQUIRE(utp_connection_get_statistic(pair.server_probe.connection, &server_statistic) == UTP_STATUS_OK);
        return client_statistic.pmtu == 1450u && server_statistic.pmtu == 1450u;
    });
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("path migration validates the new address and replays the buffered STREAM", "[transport][integration][path]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 14> payload = {'p', 'a', 't', 'h', '-', 'm', 'i', 'g', 'r', 'a', 't', 'i', 'o', 'n'};
    std::array<uint8_t, payload.size()> received    = {};
    utp_connection_description_t        description = {};
    utp_connection_statistic_t          statistic   = {};
    uint32_t                            stream_id   = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    drive_for(pair.event_base, std::chrono::milliseconds(80));
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    relay_enable_migration(&pair.relay);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_connection_get_description(pair.server_probe.connection, &description) == UTP_STATUS_OK);
    REQUIRE(description.remote_port == pair.relay.migration_address.port);
    REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes == 0u);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("path validation preserves cached packets within its configured capacity", "[transport][integration][path]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const relay_rule hold_path_response           = {relay_direction::client_to_server,
                                                     relay_action::hold,
                                                     UTP_PACKET_TYPE_CTRL,
                                                     UTP_FRAME_BIT(UTP_FRAME_TYPE_PATH_RESPONSE),
                                                     true,
                                                     0u,
                                                     false,
                                                     false};
    const std::array<uint8_t, 32>     first       = {'c', 'a', 'c', 'h', 'e', '-', 'f', 'i', 'r', 's', 't'};
    const std::array<uint8_t, 32>     second      = {'c', 'a', 'c', 'h', 'e', '-', 's', 'e', 'c', 'o', 'n', 'd'};
    std::array<uint8_t, first.size()> received    = {};
    utp_context_options_t             server_opts = UTP_CONTEXT_OPTIONS_INIT;
    uint32_t                          stream_id   = UINT32_MAX;
    utp_stream_t*                     stream;
    size_t                            cached_bytes;
    size_t                            received_length = 0u;

    server_opts.context_id                      = 5302u;
    server_opts.path_validation_buffer_capacity = 96u;
    transport_pair_init_with_options(&pair, no_rule, UTP_ENCRYPTION_NONE, nullptr, &server_opts);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    relay_enable_migration(&pair.relay);
    pair.relay.rule = hold_path_response;
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, first.data(), first.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair] {
        return pair.relay.held_valid && pair.server_probe.connection->candidate_packet_head != nullptr;
    });
    cached_bytes = pair.server_probe.connection->candidate_packet_bytes;
    REQUIRE(cached_bytes != 0u);
    REQUIRE(cached_bytes <= server_opts.path_validation_buffer_capacity);
    REQUIRE(utp_stream_write(stream, second.data(), second.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_for(pair.event_base, std::chrono::milliseconds(20));
    REQUIRE(pair.server_probe.connection->candidate_packet_bytes == cached_bytes);
    relay_release_held(&pair.relay);
    drive_until(pair.event_base, [&pair, &first] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == first.size();
    });
    REQUIRE(pair.server_probe.connection->candidate_packet_head == nullptr);
    REQUIRE(pair.server_probe.connection->candidate_packet_bytes == 0u);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == first.size());
    REQUIRE(received == first);
    drive_until(pair.event_base, [&pair, &second] {
        return utp_stream_readable_bytes(pair.server_probe.incoming_stream) == second.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == second.size());
    REQUIRE(received == second);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("application reads replenish connection and stream flow-control windows", "[transport][integration][flow]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    std::array<uint8_t, 192> payload      = {};
    std::array<uint8_t, 64>  received     = {};
    utp_context_options_t    client_opts  = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t    server_opts  = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_options_t    connect_opts = UTP_CONNECT_OPTIONS_INIT;
    uint16_t                 server_port  = 0u;
    uint32_t                 stream_id    = UINT32_MAX;
    utp_stream_t*            stream;
    size_t                   received_length = 0u;

    for (size_t index = 0u; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(index);
    }
    pair.event_base = event_base_new();
    REQUIRE(pair.event_base != nullptr);
    client_opts.event_base                          = pair.event_base;
    client_opts.context_id                          = 5101u;
    server_opts.event_base                          = pair.event_base;
    server_opts.context_id                          = 5102u;
    server_opts.initial_max_data                    = 96u;
    server_opts.initial_max_stream_data_bidi_remote = 64u;
    REQUIRE(utp_context_create(&client_opts, &pair.client) == UTP_STATUS_OK);
    REQUIRE(utp_context_create(&server_opts, &pair.server) == UTP_STATUS_OK);
    pair.server_probe.context = pair.server;
    REQUIRE(utp_context_bind(pair.client, "127.0.0.1", 0u, nullptr, nullptr) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(pair.server, "127.0.0.1", 0u, nullptr, &server_port) == UTP_STATUS_OK);
    relay_init(&pair.relay, pair.event_base, server_port);
    pair.relay.rule = no_rule;
    utp_context_set_on_connected(pair.client, on_connected, &pair.client_probe);
    utp_context_set_on_connected(pair.server, on_connected, &pair.server_probe);
    utp_context_set_on_new_connection(pair.server, on_new_connection, &pair.server_probe);
    utp_context_set_on_connection_error(pair.client, on_connection_error, &pair.client_probe);
    utp_context_set_on_connection_error(pair.server, on_connection_error, &pair.server_probe);
    connect_opts.address    = "127.0.0.1";
    connect_opts.port       = pair.relay.address.port;
    connect_opts.timeout_ms = 10u;
    connect_opts.retries    = 3;
    REQUIRE(utp_context_connect(pair.client, &connect_opts) == UTP_STATUS_OK);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    for (size_t offset = 0u; offset < payload.size(); offset += received.size()) {
        drive_until(pair.event_base, [&pair, &received] {
            return pair.server_probe.incoming_stream != nullptr &&
                   utp_stream_readable_bytes(pair.server_probe.incoming_stream) == received.size();
        });
        REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(),
                                &received_length) == UTP_STATUS_OK);
        REQUIRE(received_length == received.size());
        REQUIRE(std::equal(received.begin(), received.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset)));
    }
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE((pair.relay.forwarded_server_frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX_DATA)) != 0u);
    REQUIRE((pair.relay.forwarded_server_frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX_STREAM_DATA)) != 0u);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("dropped MAX_DATA is retransmitted and unblocks the sender", "[transport][integration][flow]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const relay_rule drop_flow_control    = {relay_direction::server_to_client,
                                             relay_action::drop,
                                             UTP_PACKET_TYPE_CTRL,
                                             UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX_DATA),
                                             true,
                                             0u,
                                             false,
                                             false};
    std::array<uint8_t, 128> payload      = {};
    std::array<uint8_t, 64>  received     = {};
    utp_context_options_t    client_opts  = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t    server_opts  = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_options_t    connect_opts = UTP_CONNECT_OPTIONS_INIT;
    uint16_t                 server_port  = 0u;
    uint32_t                 stream_id    = UINT32_MAX;
    utp_stream_t*            stream;
    size_t                   received_length = 0u;

    for (size_t index = 0u; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(index);
    }
    pair.event_base = event_base_new();
    REQUIRE(pair.event_base != nullptr);
    client_opts.event_base                          = pair.event_base;
    client_opts.context_id                          = 5201u;
    server_opts.event_base                          = pair.event_base;
    server_opts.context_id                          = 5202u;
    server_opts.initial_max_data                    = 96u;
    server_opts.initial_max_stream_data_bidi_remote = 64u;
    REQUIRE(utp_context_create(&client_opts, &pair.client) == UTP_STATUS_OK);
    REQUIRE(utp_context_create(&server_opts, &pair.server) == UTP_STATUS_OK);
    pair.server_probe.context = pair.server;
    REQUIRE(utp_context_bind(pair.client, "127.0.0.1", 0u, nullptr, nullptr) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(pair.server, "127.0.0.1", 0u, nullptr, &server_port) == UTP_STATUS_OK);
    relay_init(&pair.relay, pair.event_base, server_port);
    pair.relay.rule = no_rule;
    utp_context_set_on_connected(pair.client, on_connected, &pair.client_probe);
    utp_context_set_on_connected(pair.server, on_connected, &pair.server_probe);
    utp_context_set_on_new_connection(pair.server, on_new_connection, &pair.server_probe);
    utp_context_set_on_connection_error(pair.client, on_connection_error, &pair.client_probe);
    utp_context_set_on_connection_error(pair.server, on_connection_error, &pair.server_probe);
    connect_opts.address    = "127.0.0.1";
    connect_opts.port       = pair.relay.address.port;
    connect_opts.timeout_ms = 10u;
    connect_opts.retries    = 3;
    REQUIRE(utp_context_connect(pair.client, &connect_opts) == UTP_STATUS_OK);
    transport_pair_connect(&pair);
    pair.relay.rule = drop_flow_control;
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &received] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == received.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == received.size());
    REQUIRE(std::equal(received.begin(), received.end(), payload.begin()));
    drive_until(pair.event_base, [&pair] { return pair.relay.rule.hits == 1u; });
    drive_until(pair.event_base, [&pair, &received] {
        return utp_stream_readable_bytes(pair.server_probe.incoming_stream) == received.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == received.size());
    REQUIRE(
        std::equal(received.begin(), received.end(), payload.begin() + static_cast<std::ptrdiff_t>(received.size())));
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE((pair.relay.forwarded_server_frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX_DATA)) != 0u);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("STOP_SENDING cancels a peer unidirectional sender and receives RESET_STREAM",
          "[transport][integration][stream]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 64> payload   = {'s', 't', 'o', 'p'};
    uint32_t                      stream_id = UINT32_MAX;
    utp_stream_t*                 stream;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_UNIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_stream_shutdown(pair.server_probe.incoming_stream, UTP_STREAM_SHUTDOWN_READ) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair] {
        return (pair.relay.forwarded_server_frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STOP_SENDING)) != 0u &&
               (pair.relay.forwarded_client_frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_RESET_STREAM)) != 0u;
    });
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_CANCELLED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("RESET_STREAM aborts local writes but preserves the bidirectional read side",
          "[transport][integration][stream]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 32>        request   = {'r', 'e', 's', 'e', 't'};
    const std::array<uint8_t, 32>        response  = {'r', 'e', 'p', 'l', 'y'};
    std::array<uint8_t, response.size()> received  = {};
    uint32_t                             stream_id = UINT32_MAX;
    utp_stream_t*                        client_stream;
    utp_stream_t*                        server_stream;
    size_t                               received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    client_stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(client_stream != nullptr);
    REQUIRE(utp_stream_write(client_stream, request.data(), request.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &request] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == request.size();
    });
    REQUIRE(utp_stream_reset(client_stream, UINT16_C(42)) == UTP_STATUS_OK);
    REQUIRE(utp_stream_write(client_stream, request.data(), request.size()) == UTP_STATUS_CANCELLED);
    server_stream = pair.server_probe.incoming_stream;
    drive_until(pair.event_base, [server_stream] { return server_stream->peer_reset; });
    REQUIRE(utp_stream_read(server_stream, received.data(), received.size(), &received_length) == UTP_STATUS_CANCELLED);
    REQUIRE(utp_stream_write(server_stream, response.data(), response.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(server_stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base,
                [client_stream, &response] { return utp_stream_readable_bytes(client_stream) == response.size(); });
    REQUIRE(utp_stream_read(client_stream, received.data(), received.size(), &received_length) == UTP_STATUS_OK);
    REQUIRE(received_length == response.size());
    REQUIRE(received == response);
    REQUIRE(utp_stream_read(client_stream, received.data(), received.size(), &received_length) == UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("an invalid frame closes the receiving connection with a protocol error",
          "[transport][integration][protocol]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 1u> packet = {};
    utp_packet_header_t                              header;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    header = {pair.client_probe.connection->local_cid,
              pair.server_probe.connection->local_cid,
              pair.client_probe.connection->send_control.current_packet_number + UINT64_C(100),
              1u,
              UTP_PACKET_TYPE_CTRL,
              0u};
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    packet[UTP_PACKET_HEADER_SIZE] = UINT8_MAX;
    relay_forward(&pair.relay, packet.data(), packet.size(), &pair.relay.server);
    drive_until(pair.event_base, [&pair] {
        return pair.server_probe.connection_errors == 1 && pair.client_probe.connection_errors == 1;
    });
    REQUIRE((utp_connection_state(pair.server_probe.connection) == UTP_CONNECTION_STATE_CLOSING ||
             utp_connection_state(pair.server_probe.connection) == UTP_CONNECTION_STATE_DRAINING));
    REQUIRE(pair.client_probe.connection_errors == 1);
    transport_pair_cleanup(&pair);
}

TEST_CASE("relay drops the first CONNECTION_CLOSE and the close is retransmitted", "[transport][integration]")
{
    transport_pair   pair = {};
    const relay_rule rule = {relay_direction::client_to_server,
                             relay_action::drop,
                             UTP_PACKET_TYPE_CONNECTION_CLOSE,
                             UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE),
                             true,
                             0u,
                             false,
                             false};

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_close(pair.client_probe.connection);
    drive_until(pair.event_base,
                [&pair] { return pair.relay.rule.hits == 1u && pair.server_probe.connection_errors == 1; });
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 1);
    transport_pair_cleanup(&pair);
}

TEST_CASE("simultaneous local closes converge to draining without peer error callbacks",
          "[transport][integration][close]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_close(pair.client_probe.connection);
    utp_connection_close(pair.server_probe.connection);
    drive_until(pair.event_base, [&pair] {
        return utp_connection_state(pair.client_probe.connection) == UTP_CONNECTION_STATE_DRAINING &&
               utp_connection_state(pair.server_probe.connection) == UTP_CONNECTION_STATE_DRAINING;
    });
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

// sendto/sendmsg 的故障注入只在 macOS 使用 fishhook 实现。
#if defined(__APPLE__)
TEST_CASE("macOS send hook retries one EAGAIN without data loss", "[transport][integration][socket]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 8> payload   = {'r', 'e', 't', 'r', 'y'};
    uint32_t                     stream_id = UINT32_MAX;
    utp_stream_t*                stream;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_test_send_hook_configure((int32_t)pair.client->udp_socket.native_handle, EAGAIN, 1u));
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_test_send_hook_remaining() == 0u);
    REQUIRE(pair.client_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("macOS send hook backs off an MTU probe after EMSGSIZE", "[transport][integration][socket][mtu]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 8> payload    = {'m', 't', 'u', '-', 'f', 'a', 'i', 'l'};
    utp_mtu_config_t             mtu_config = UTP_MTU_CONFIG_INIT;
    utp_connection_t*            connection;
    uint32_t                     stream_id = UINT32_MAX;
    utp_stream_t*                stream;
    utp_internal_error_t         error;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    connection                        = pair.client_probe.connection;
    mtu_config.mtu_min                = 1280u;
    mtu_config.mtu_base               = 1400u;
    mtu_config.mtu_max                = 1450u;
    mtu_config.probe_retries          = 0u;
    mtu_config.probe_interval_seconds = 1u;
    utp_mtu_discovery_init(&connection->mtu_discovery, &mtu_config, connection->peer.family);
    REQUIRE(utp_test_send_hook_configure((int32_t)pair.client->udp_socket.native_handle, EMSGSIZE, 1u));
    error = utp_context_flush_public_connection(pair.client, connection);
    REQUIRE(utp_internal_error_to_errno(error) == EMSGSIZE);
    REQUIRE(utp_test_send_hook_remaining() == 0u);
    REQUIRE(connection->mtu_discovery.search_high_mtu == 1449u);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&connection->mtu_discovery) == 1425u);
    REQUIRE(pair.client_probe.connection_errors == 0);

    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) == UTP_STATUS_OK);
    stream = utp_connection_get_stream(connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(pair.client_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("macOS send hook reports ENOBUFS as a terminal local error", "[transport][integration][socket]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 8> payload   = {'n', 'o', 'b', 'u', 'f', 's'};
    uint32_t                     stream_id = UINT32_MAX;
    utp_stream_t*                stream;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_test_send_hook_configure((int32_t)pair.client->udp_socket.native_handle, ENOBUFS, 1u));
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_LIMIT);
    REQUIRE(utp_test_send_hook_remaining() == 0u);
    REQUIRE(pair.client_probe.connection_errors == 1);
    transport_pair_cleanup(&pair);
}
#endif

TEST_CASE("relay drops the first STREAM packet and PTO retransmission delivers it once", "[transport][integration]")
{
    transport_pair                      pair      = {};
    const relay_rule                    rule      = {relay_direction::client_to_server,
                                                     relay_action::drop,
                                                     UTP_PACKET_TYPE_CTRL,
                                                     UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM),
                                                     true,
                                                     0u,
                                                     false,
                                                     false};
    const std::array<uint8_t, 12>       payload   = {'r', 'e', 't', 'r', 'a', 'n', 's', 'm', 'i', 't', '!', '!'};
    std::array<uint8_t, payload.size()> received  = {};
    utp_connection_statistic_t          statistic = {};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.relay.rule.hits == 1u && pair.relay.forwarded_stream_packets >= 1u &&
               pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes > 0u);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    transport_pair_cleanup(&pair);
}

TEST_CASE("CUBIC shrinks its congestion window after an end-to-end STREAM loss", "[transport][integration][congestion]")
{
    transport_pair                      pair           = {};
    utp_context_options_t               client_options = UTP_CONTEXT_OPTIONS_INIT;
    const relay_rule                    rule           = {relay_direction::client_to_server,
                                                          relay_action::drop,
                                                          UTP_PACKET_TYPE_CTRL,
                                                          UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM),
                                                          true,
                                                          0u,
                                                          false,
                                                          false};
    const std::array<uint8_t, 12>       payload        = {'c', 'u', 'b', 'i', 'c', '-', 'l', 'o', 's', 's', '!', '!'};
    std::array<uint8_t, payload.size()> received       = {};
    utp_connection_statistic_t          statistic      = {};
    uint64_t                            initial_cwnd   = 0u;
    uint32_t                            stream_id      = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    client_options.cc_algorithm        = UTP_CONGESTION_CUBIC;
    client_options.cubic_init_cwnd_mss = 8u;
    client_options.cubic_min_cwnd_mss  = 4u;
    client_options.enable_dplpmtud     = false;
    transport_pair_init_with_options(&pair, rule, UTP_ENCRYPTION_NONE, &client_options, nullptr);
    transport_pair_connect(&pair);
    initial_cwnd = pair.client_probe.connection->cubic_congestion.cwnd;
    REQUIRE(pair.client_probe.connection->cubic_congestion.initial_cwnd == UINT64_C(8) * UTP_CUBIC_DEFAULT_MSS);
    REQUIRE(initial_cwnd > pair.client_probe.connection->cubic_congestion.minimum_cwnd);

    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.relay.rule.hits == 1u && pair.relay.forwarded_stream_packets >= 1u &&
               pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });

    REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes > 0u);
    REQUIRE(pair.client_probe.connection->cubic_congestion.cwnd < initial_cwnd);
    REQUIRE(pair.client_probe.connection->cubic_congestion.cwnd >=
            pair.client_probe.connection->cubic_congestion.minimum_cwnd);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("relay drops the first ACK and a retransmitted STREAM is acknowledged", "[transport][integration]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const relay_rule ack_rule                     = {relay_direction::server_to_client,
                                                     relay_action::drop,
                                                     UTP_PACKET_TYPE_CTRL,
                                                     UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK),
                                                     true,
                                                     0u,
                                                     true,
                                                     true};
    const std::array<uint8_t, 7>        payload   = {'a', 'c', 'k', 'l', 'o', 's', 's'};
    std::array<uint8_t, payload.size()> received  = {};
    utp_connection_statistic_t          statistic = {};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    pair.relay.rule = ack_rule;
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size() &&
               pair.relay.stream_packet_transmissions >= 1u;
    });
    drive_until(pair.event_base, [&pair] { return pair.relay.rule.hits >= 1u; });
    drive_until(pair.event_base, [&pair] { return pair.relay.stream_packet_transmissions >= 2u; });
    drive_until(pair.event_base, [&pair] { return pair.relay.forwarded_stream_acks >= 1u; });
    REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes > 0u);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted 1-RTT STREAM data and FIN are delivered", "[transport][integration][crypto]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const std::array<uint8_t, 14> payload = {'e', 'n', 'c', 'r', 'y', 'p', 't', 'e', 'd', '-', '1', 'r', 't', 't'};
    std::array<uint8_t, payload.size()> received  = {};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("tampered encrypted STREAM packet is discarded and retransmitted", "[transport][integration][crypto]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    const relay_rule corrupt_rule = {
        relay_direction::client_to_server, relay_action::corrupt, UTP_PACKET_TYPE_CTRL, 0u, true, 0u, false, false};
    const std::array<uint8_t, 8>        payload   = {'t', 'a', 'm', 'p', 'e', 'r', 'e', 'd'};
    std::array<uint8_t, payload.size()> received  = {};
    utp_connection_statistic_t          statistic = {};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    drive_for(pair.event_base, std::chrono::milliseconds(80));
    pair.relay.rule = corrupt_rule;
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.relay.rule.hits == 1u && pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes > 0u);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted STREAM loss is recovered by PTO without duplicate delivery", "[transport][integration][crypto]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    relay_rule       drop_rule = {
        relay_direction::client_to_server, relay_action::drop, UTP_PACKET_TYPE_CTRL, 0u, true, 0u, false, false};
    const std::array<uint8_t, 1024> payload = {'e', 'n', 'c', 'r', 'y', 'p', 't', 'e', 'd', '-', 'l', 'o', 's', 's'};
    std::array<uint8_t, payload.size()> received  = {};
    utp_connection_statistic_t          statistic = {};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    drive_for(pair.event_base, std::chrono::milliseconds(80));
    pair.relay.target_packet_number = pair.client_probe.connection->send_control.current_packet_number + 1u;
    pair.relay.rule                 = drop_rule;
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.relay.rule.hits == 1u && pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_connection_get_statistic(pair.client_probe.connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes > 0u);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("large encrypted STREAM is segmented and reassembled", "[transport][integration][crypto]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    std::vector<uint8_t> payload(64u * 1024u);
    std::vector<uint8_t> received(payload.size());
    stream_send_probe    send_probe = {};
    uint32_t             stream_id  = UINT32_MAX;
    utp_stream_t*        stream;
    size_t               received_length = 0u;

    for (size_t index = 0u; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(index);
    }
    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_256);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    send_probe.data   = payload.data();
    send_probe.length = payload.size();
    utp_stream_set_on_writable(stream, on_stream_writable, &send_probe);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(send_probe.status == UTP_STATUS_OK);
    REQUIRE(send_probe.offset == payload.size());
    REQUIRE(send_probe.writable_callbacks > 1);
    REQUIRE(send_probe.write_shutdown);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("relay duplicates a STREAM packet without duplicate delivery", "[transport][integration]")
{
    transport_pair                      pair      = {};
    const relay_rule                    rule      = {relay_direction::client_to_server,
                                                     relay_action::duplicate,
                                                     UTP_PACKET_TYPE_CTRL,
                                                     UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM),
                                                     true,
                                                     0u,
                                                     false,
                                                     false};
    const std::array<uint8_t, 9>        payload   = {'d', 'u', 'p', 'l', 'i', 'c', 'a', 't', 'e'};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    std::array<uint8_t, payload.size()> received        = {};
    size_t                              received_length = 0u;

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.relay.rule.hits == 1u && pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted duplicate STREAM packet is delivered once", "[transport][integration][crypto]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    relay_rule       rule    = {
        relay_direction::client_to_server, relay_action::duplicate, UTP_PACKET_TYPE_CTRL, 0u, true, 0u, false, false};
    const std::array<uint8_t, 1024>     payload   = {'e', 'n', 'c', '-', 'd', 'u', 'p', 'l', 'i', 'c', 'a', 't'};
    std::array<uint8_t, payload.size()> received  = {};
    uint32_t                            stream_id = UINT32_MAX;
    utp_stream_t*                       stream;
    size_t                              received_length = 0u;

    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    drive_for(pair.event_base, std::chrono::milliseconds(80));
    pair.relay.target_packet_number = pair.client_probe.connection->send_control.current_packet_number + 1u;
    pair.relay.rule                 = rule;
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, payload.data(), payload.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &payload] {
        return pair.relay.rule.hits == 1u && pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == payload.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == payload.size());
    REQUIRE(received == payload);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}

TEST_CASE("relay holds the first STREAM packet and delivery resumes after reordering", "[transport][integration]")
{
    transport_pair             pair = {};
    const relay_rule           rule = {relay_direction::client_to_server,
                                       relay_action::hold,
                                       UTP_PACKET_TYPE_CTRL,
                                       UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM),
                                       true,
                                       0u,
                                       false,
                                       false};
    const std::vector<uint8_t> first(1024u, UINT8_C(0x31));
    const std::vector<uint8_t> second(1024u, UINT8_C(0x62));
    std::vector<uint8_t>       expected;
    uint32_t                   stream_id = UINT32_MAX;
    utp_stream_t*              stream;
    std::vector<uint8_t>       received;
    size_t                     received_length = 0u;

    expected.insert(expected.end(), first.begin(), first.end());
    expected.insert(expected.end(), second.begin(), second.end());
    received.resize(expected.size());
    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, first.data(), first.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair] { return pair.relay.held_valid; });
    REQUIRE(utp_stream_write(stream, second.data(), second.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair] {
        return pair.relay.forwarded_stream_packets > 0u && pair.server_probe.incoming_stream != nullptr;
    });
    REQUIRE(utp_stream_readable_bytes(pair.server_probe.incoming_stream) == 0u);
    relay_release_held(&pair.relay);
    drive_until(pair.event_base, [&pair, &expected] {
        return utp_stream_readable_bytes(pair.server_probe.incoming_stream) == expected.size();
    });
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == expected.size());
    REQUIRE(received == expected);
    transport_pair_cleanup(&pair);
}

TEST_CASE("encrypted STREAM packets are reassembled after reordering", "[transport][integration][crypto]")
{
    transport_pair   pair    = {};
    const relay_rule no_rule = {relay_direction::client_to_server, relay_action::drop, 0u, 0u, false, 0u, false, false};
    relay_rule       rule    = {
        relay_direction::client_to_server, relay_action::hold, UTP_PACKET_TYPE_CTRL, 0u, true, 0u, false, false};
    const std::vector<uint8_t> first(1024u, UINT8_C(0x41));
    const std::vector<uint8_t> second(1024u, UINT8_C(0x52));
    std::vector<uint8_t>       expected;
    std::vector<uint8_t>       received;
    uint32_t                   stream_id = UINT32_MAX;
    utp_stream_t*              stream;
    size_t                     received_length = 0u;

    expected.insert(expected.end(), first.begin(), first.end());
    expected.insert(expected.end(), second.begin(), second.end());
    received.resize(expected.size());
    transport_pair_init(&pair, no_rule, UTP_ENCRYPTION_AES_GCM_256);
    transport_pair_connect(&pair);
    drive_for(pair.event_base, std::chrono::milliseconds(80));
    pair.relay.target_packet_number = pair.client_probe.connection->send_control.current_packet_number + 1u;
    pair.relay.rule                 = rule;
    utp_connection_set_on_incoming_stream(pair.server_probe.connection, on_incoming_stream, &pair.server_probe);
    REQUIRE(utp_connection_create_stream(pair.client_probe.connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) ==
            UTP_STATUS_OK);
    stream = utp_connection_get_stream(pair.client_probe.connection, stream_id);
    REQUIRE(stream != nullptr);
    REQUIRE(utp_stream_write(stream, first.data(), first.size()) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair] { return pair.relay.held_valid; });
    REQUIRE(utp_stream_write(stream, second.data(), second.size()) == UTP_STATUS_OK);
    REQUIRE(utp_stream_shutdown(stream, UTP_STREAM_SHUTDOWN_WRITE) == UTP_STATUS_OK);
    drive_until(pair.event_base, [&pair, &expected] {
        return pair.server_probe.incoming_stream != nullptr &&
               utp_stream_readable_bytes(pair.server_probe.incoming_stream) == expected.size();
    });
    REQUIRE(pair.relay.held_replayed_after_successor);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_OK);
    REQUIRE(received_length == expected.size());
    REQUIRE(received == expected);
    REQUIRE(utp_stream_read(pair.server_probe.incoming_stream, received.data(), received.size(), &received_length) ==
            UTP_STATUS_CLOSED);
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 0);
    transport_pair_cleanup(&pair);
}
