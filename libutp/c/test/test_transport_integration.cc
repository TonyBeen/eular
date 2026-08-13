#define CATCH_CONFIG_MAIN

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <catch2/catch.hpp>
#include <event2/event.h>

extern "C" {
#include <utp/context.h>

#include "context/event_loop.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "socket/address.h"
#include "socket/udp.h"
#include "util/error.h"
}

namespace {

constexpr size_t kRelayDatagramCapacity = 2048u;
constexpr auto   kDriveTimeout          = std::chrono::milliseconds(2000);

enum class relay_direction : uint8_t { client_to_server, server_to_client };
enum class relay_action : uint8_t { drop, duplicate, hold };

struct relay_rule {
    relay_direction direction;        // 规则匹配的转发方向
    relay_action    action;           // 命中后的故障动作
    uint8_t         packet_type;      // 0 表示不限制包类型
    uint32_t        required_frames;  // 明文帧类型位图，0 表示不限制
    bool            pending;          // 是否仍可命中一次
    uint32_t        hits;             // 命中次数
};

struct udp_relay {
    struct event_base*                          event_base               = nullptr;
    utp_event_loop_t                            event_loop               = {};
    utp_event_t                                 read_event               = {};
    utp_udp_socket_t                            socket                   = {};
    utp_address_t                               address                  = {};
    utp_address_t                               server                   = {};
    utp_address_t                               client                   = {};
    relay_rule                                  rule                     = {};
    std::array<uint8_t, kRelayDatagramCapacity> held                     = {};
    size_t                                      held_length              = 0u;
    utp_address_t                               held_destination         = {};
    uint32_t                                    held_stream_id           = UINT32_MAX;
    uint64_t                                    held_stream_offset       = 0u;
    bool                                        client_known             = false;
    bool                                        held_valid               = false;
    uint32_t                                    forwarded_stream_packets = 0u;
};

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
    int32_t           connection_errors = 0;
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

static void on_connection_error(utp_connection_t* connection, const utp_connection_error_info_t* info, void* user_data)
{
    auto* probe = static_cast<endpoint_probe*>(user_data);

    REQUIRE(connection != nullptr);
    REQUIRE(info != nullptr);
    ++probe->connection_errors;
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
    if (relay.rule.required_frames != 0u) {
        utp_packet_view_t view = {};

        if (utp_packet_view_decode(&view, packet, packet_length) != UTP_INTERNAL_ERROR_OK ||
            (view.frame_types & relay.rule.required_frames) != relay.rule.required_frames) {
            return false;
        }
    }
    return true;
}

static void relay_forward(udp_relay* relay, const uint8_t* packet, size_t packet_length,
                          const utp_address_t* destination)
{
    size_t sent_length = 0u;

    REQUIRE(utp_udp_socket_send_to(&relay->socket, packet, packet_length, destination, &sent_length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(sent_length == packet_length);
}

static void relay_on_readable(uint32_t events, void* user_data)
{
    auto* relay = static_cast<udp_relay*>(user_data);

    if ((events & UTP_EVENT_READABLE) == 0u) {
        return;
    }
    for (;;) {
        std::array<uint8_t, kRelayDatagramCapacity> packet        = {};
        utp_address_t                               source        = {};
        size_t                                      packet_length = 0u;
        utp_internal_error_t                        error;

        error = utp_udp_socket_recv_from(&relay->socket, packet.data(), packet.size(), &packet_length, &source);
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            return;
        }
        REQUIRE(error == UTP_INTERNAL_ERROR_OK);
        const relay_direction direction = utp_address_equal(&source, &relay->server)
                                              ? relay_direction::server_to_client
                                              : relay_direction::client_to_server;
        const utp_address_t*  destination;

        if (direction == relay_direction::client_to_server) {
            relay->client       = source;
            relay->client_known = true;
            destination         = &relay->server;
        } else {
            REQUIRE(relay->client_known);
            destination = &relay->client;
        }
        if (relay_packet_matches(*relay, direction, packet.data(), packet_length)) {
            relay->rule.pending = false;
            ++relay->rule.hits;
            if (relay->rule.action == relay_action::drop) {
                continue;
            }
            if (relay->rule.action == relay_action::hold) {
                REQUIRE(relay_find_stream(packet.data(), packet_length, &relay->held_stream_id,
                                          &relay->held_stream_offset));
                REQUIRE(packet_length <= relay->held.size());
                std::copy(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(packet_length),
                          relay->held.begin());
                relay->held_length      = packet_length;
                relay->held_destination = *destination;
                relay->held_valid       = true;
                continue;
            }
            relay_forward(relay, packet.data(), packet_length, destination);
            relay_forward(relay, packet.data(), packet_length, destination);
            continue;
        }
        if (relay->held_valid && direction == relay_direction::client_to_server) {
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
        relay_forward(relay, packet.data(), packet_length, destination);
    }
}

static void relay_init(udp_relay* relay, struct event_base* event_base, uint16_t server_port)
{
    utp_address_t requested = {};

    relay->event_base = event_base;
    REQUIRE(utp_address_parse(&requested, "127.0.0.1", 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_parse(&relay->server, "127.0.0.1", server_port) == UTP_INTERNAL_ERROR_OK);
    utp_udp_socket_init(&relay->socket);
    utp_event_init(&relay->read_event);
    REQUIRE(utp_udp_socket_open(&relay->socket, requested.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_udp_socket_bind(&relay->socket, &requested, nullptr, &relay->address) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_loop_init(&relay->event_loop, event_base, nullptr, nullptr) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_event_add_udp(&relay->event_loop, &relay->read_event, &relay->socket, UTP_EVENT_READABLE, true,
                              relay_on_readable, relay) == UTP_INTERNAL_ERROR_OK);
}

static void relay_release_held(udp_relay* relay)
{
    REQUIRE(relay->held_valid);
    relay_forward(relay, relay->held.data(), relay->held_length, &relay->held_destination);
    relay->held_valid = false;
}

static void relay_cleanup(udp_relay* relay)
{
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

static void transport_pair_init(transport_pair* pair, relay_rule rule, utp_encryption_mode_t encryption)
{
    utp_context_options_t client_options  = UTP_CONTEXT_OPTIONS_INIT;
    utp_context_options_t server_options  = UTP_CONTEXT_OPTIONS_INIT;
    utp_connect_options_t connect_options = UTP_CONNECT_OPTIONS_INIT;
    uint16_t              server_port     = 0u;

    pair->event_base = event_base_new();
    REQUIRE(pair->event_base != nullptr);
    client_options.event_base = pair->event_base;
    client_options.context_id = 5001u;
    server_options.event_base = pair->event_base;
    server_options.context_id = 5002u;
    REQUIRE(utp_context_create(&client_options, &pair->client) == UTP_STATUS_OK);
    REQUIRE(utp_context_create(&server_options, &pair->server) == UTP_STATUS_OK);
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
        relay_direction::client_to_server, relay_action::drop, UTP_PACKET_TYPE_INITIAL, 0u, true, 0u};

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
        relay_direction::server_to_client, relay_action::drop, UTP_PACKET_TYPE_HANDSHAKE, 0u, true, 0u};

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
        relay_direction::server_to_client, relay_action::drop, UTP_PACKET_TYPE_HANDSHAKE, 0u, true, 0u};

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_AES_GCM_128);
    transport_pair_connect(&pair);
    REQUIRE(pair.relay.rule.hits == 1u);
    REQUIRE(pair.server_probe.new_connections == 1);
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
                             0u};

    transport_pair_init(&pair, rule, UTP_ENCRYPTION_NONE);
    transport_pair_connect(&pair);
    utp_connection_close(pair.client_probe.connection);
    drive_until(pair.event_base,
                [&pair] { return pair.relay.rule.hits == 1u && pair.server_probe.connection_errors == 1; });
    REQUIRE(pair.client_probe.connection_errors == 0);
    REQUIRE(pair.server_probe.connection_errors == 1);
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
                                                     0u};
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

TEST_CASE("relay holds the first STREAM packet and delivery resumes after reordering", "[transport][integration]")
{
    transport_pair             pair = {};
    const relay_rule           rule = {relay_direction::client_to_server,
                                       relay_action::hold,
                                       UTP_PACKET_TYPE_CTRL,
                                       UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM),
                                       true,
                                       0u};
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
