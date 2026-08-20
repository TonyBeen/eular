#define CATCH_CONFIG_MAIN

#include <array>
#include <cstdint>
#include <cstring>

#include <catch2/catch.hpp>
#include <event2/event.h>

#if defined(__linux__)
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#endif

extern "C" {
#include <utp/utp.h>

#include "nat/nat.h"
#include "proto/proto.h"
#include "proto/wire.h"
}

static void test_nat_write_endpoint(utp_wire_writer_t* writer, uint16_t port, uint8_t last_octet)
{
    REQUIRE(utp_wire_write_u8(writer, UTP_ADDRESS_FAMILY_IPV4) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(writer, 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u16(writer, port) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(writer, 198u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(writer, 51u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(writer, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(writer, last_octet) == UTP_INTERNAL_ERROR_OK);
}

static void test_nat_write_tlv(utp_wire_writer_t* writer, uint16_t type, const uint8_t* value, uint16_t length)
{
    REQUIRE(utp_wire_write_u16(writer, type) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u16(writer, length) == UTP_INTERNAL_ERROR_OK);
    for (uint16_t index = 0u; index < length; ++index) {
        REQUIRE(utp_wire_write_u8(writer, value[index]) == UTP_INTERNAL_ERROR_OK);
    }
}

struct test_nat_server;

struct test_nat_server_socket {
    test_nat_server* server = nullptr;
    int32_t          fd     = -1;
    uint8_t          role   = 0u;
};

struct test_nat_server {
    event_base*                            base      = nullptr;
    std::array<test_nat_server_socket, 3u> sockets   = {};
    std::array<event*, 3u>                 events    = {};
    std::array<utp_address_t, 3u>          endpoints = {};
};

enum : uint8_t {
    TEST_NAT_SERVER_PRIMARY = 0u,
    TEST_NAT_SERVER_CHANGE_PORT,
    TEST_NAT_SERVER_ALTERNATE,
};

#if defined(__linux__)

static void test_nat_server_stop(test_nat_server* server);

static void test_nat_write_address(utp_wire_writer_t* writer, const utp_address_t& address)
{
    const size_t address_length = address.family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;

    REQUIRE(utp_wire_write_u8(writer, address.family) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(writer, 0u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u16(writer, address.port) == UTP_INTERNAL_ERROR_OK);
    for (size_t index = 0u; index < address_length; ++index) {
        REQUIRE(utp_wire_write_u8(writer, address.address[index]) == UTP_INTERNAL_ERROR_OK);
    }
}

static void test_nat_write_address_tlv(utp_wire_writer_t* writer, uint16_t type, const utp_address_t& address)
{
    std::array<uint8_t, 20u> endpoint        = {};
    utp_wire_writer_t        endpoint_writer = {};
    const size_t             address_length  = address.family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;

    REQUIRE(utp_wire_writer_init(&endpoint_writer, endpoint.data(), endpoint.size()) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_address(&endpoint_writer, address);
    test_nat_write_tlv(writer, type, endpoint.data(), static_cast<uint16_t>(address_length + 4u));
}

static bool test_nat_server_parse_request(const uint8_t* packet, size_t packet_length, utp_packet_header_t* header,
                                          uint8_t* message_type, uint8_t* phase, const uint8_t** token)
{
    utp_wire_reader_t reader = {};
    uint8_t           version;
    uint8_t           flags;

    if (utp_proto_decode_header(header, packet, packet_length) != UTP_INTERNAL_ERROR_OK ||
        header->type != UTP_PACKET_TYPE_NAT_PROBE || header->scid != 0u || header->dcid != 0u ||
        header->packet_number == 0u ||
        static_cast<size_t>(header->payload_length) + UTP_PACKET_HEADER_SIZE != packet_length ||
        utp_wire_reader_init(&reader, packet + UTP_PACKET_HEADER_SIZE, header->payload_length) !=
            UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u8(&reader, &version) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u8(&reader, message_type) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u8(&reader, phase) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u8(&reader, &flags) != UTP_INTERNAL_ERROR_OK || version != UTP_NAT_PROBE_VERSION || flags != 0u) {
        return false;
    }
    while (reader.remaining != 0u) {
        uint16_t       type;
        uint16_t       length;
        const uint8_t* value;

        if (utp_wire_read_u16(&reader, &type) != UTP_INTERNAL_ERROR_OK ||
            utp_wire_read_u16(&reader, &length) != UTP_INTERNAL_ERROR_OK || reader.remaining < length) {
            return false;
        }
        value             = reader.cursor;
        reader.cursor    += length;
        reader.remaining -= length;
        if (type == UTP_NAT_PROBE_TLV_PROBE_TOKEN && length == UTP_NAT_PROBE_TOKEN_SIZE && *token == nullptr) {
            *token = value;
        }
    }
    return *token != nullptr;
}

static bool test_nat_server_build_response(const test_nat_server& server, const utp_packet_header_t& request_header,
                                           uint8_t phase, const uint8_t* token, const utp_address_t& client,
                                           uint8_t response_role, uint8_t response[UTP_NAT_PROBE_PACKET_SIZE],
                                           size_t* response_length)
{
    utp_packet_header_t header = {};
    utp_address_t       mapped = client;
    utp_wire_writer_t   writer = {};

    header.packet_number = request_header.packet_number;
    header.type          = UTP_PACKET_TYPE_NAT_PROBE;
    mapped.address[0]    = 198u;
    mapped.address[1]    = 51u;
    mapped.address[2]    = 100u;
    mapped.address[3]    = 9u;
    if (utp_wire_writer_init(&writer, response + UTP_PACKET_HEADER_SIZE,
                             UTP_NAT_PROBE_PACKET_SIZE - UTP_PACKET_HEADER_SIZE) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_write_u8(&writer, UTP_NAT_PROBE_VERSION) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_write_u8(&writer, utp_nat_probe_response_message_type(phase)) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_write_u8(&writer, phase) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_write_u8(&writer, 0u) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_PROBE_TOKEN, token, UTP_NAT_PROBE_TOKEN_SIZE);
    test_nat_write_address_tlv(&writer, UTP_NAT_PROBE_TLV_MAPPED_ADDR, mapped);
    test_nat_write_address_tlv(&writer, UTP_NAT_PROBE_TLV_ORIGIN_ADDR, server.endpoints[response_role]);
    if (phase == UTP_NAT_PROBE_PHASE_PROBE1) {
        test_nat_write_address_tlv(&writer, UTP_NAT_PROBE_TLV_ALTERNATE_PROBE_ENDPOINT,
                                   server.endpoints[TEST_NAT_SERVER_ALTERNATE]);
    }
    *response_length = UTP_NAT_PROBE_PACKET_SIZE - UTP_PACKET_HEADER_SIZE - writer.remaining + UTP_PACKET_HEADER_SIZE;
    header.payload_length = static_cast<uint16_t>(*response_length - UTP_PACKET_HEADER_SIZE);
    return utp_proto_encode_header(response, UTP_NAT_PROBE_PACKET_SIZE, &header) == UTP_INTERNAL_ERROR_OK;
}

static void test_nat_server_on_read(evutil_socket_t fd, int16_t events, void* user_data)
{
    auto* const               socket      = static_cast<test_nat_server_socket*>(user_data);
    std::array<uint8_t, 256u> request     = {};
    std::array<uint8_t, 128u> response    = {};
    sockaddr_storage          peer        = {};
    socklen_t                 peer_length = sizeof(peer);
    utp_packet_header_t       header      = {};
    utp_address_t             client      = {};
    const uint8_t*            token       = nullptr;
    uint8_t                   message_type;
    uint8_t                   phase;
    uint8_t                   response_role;
    size_t                    response_length;
    const ssize_t             received =
        recvfrom(fd, request.data(), request.size(), 0, reinterpret_cast<sockaddr*>(&peer), &peer_length);

    (void)events;
    if (received <= 0 || peer.ss_family != AF_INET ||
        !test_nat_server_parse_request(request.data(), static_cast<size_t>(received), &header, &message_type, &phase,
                                       &token)) {
        return;
    }
    client.family = UTP_ADDRESS_FAMILY_IPV4;
    client.port   = ntohs(reinterpret_cast<const sockaddr_in*>(&peer)->sin_port);
    std::memcpy(client.address, &reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr, 4u);
    response_role = phase == UTP_NAT_PROBE_PHASE_CHANGE_PORT ? static_cast<uint8_t>(TEST_NAT_SERVER_CHANGE_PORT)
                    : phase == UTP_NAT_PROBE_PHASE_CHANGE_IP ? static_cast<uint8_t>(TEST_NAT_SERVER_ALTERNATE)
                                                             : socket->role;
    if (!test_nat_server_build_response(*socket->server, header, phase, token, client, response_role, response.data(),
                                        &response_length)) {
        return;
    }
    (void)sendto(socket->server->sockets[response_role].fd, response.data(), response_length, 0,
                 reinterpret_cast<const sockaddr*>(&peer), peer_length);
}

static int32_t test_nat_server_open_socket(uint8_t last_octet, utp_address_t* endpoint)
{
    sockaddr_in   address        = {};
    const int32_t fd             = socket(AF_INET, SOCK_DGRAM, 0);
    socklen_t     address_length = sizeof(address);

    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl((UINT32_C(127) << 24u) | last_octet);
    if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return -1;
    }
    *endpoint            = {};
    endpoint->port       = ntohs(address.sin_port);
    endpoint->family     = UTP_ADDRESS_FAMILY_IPV4;
    endpoint->address[0] = 127u;
    endpoint->address[3] = last_octet;
    return fd;
}

static bool test_nat_server_start(test_nat_server* server, event_base* base)
{
    const std::array<uint8_t, 3u> octets = {1u, 1u, 2u};

    server->base = base;
    for (size_t index = 0u; index < server->sockets.size(); ++index) {
        server->sockets[index].server = server;
        server->sockets[index].fd     = test_nat_server_open_socket(octets[index], &server->endpoints[index]);
        server->sockets[index].role   = static_cast<uint8_t>(index);
        if (server->sockets[index].fd < 0 ||
            (server->events[index] = event_new(base, server->sockets[index].fd, EV_READ | EV_PERSIST,
                                               test_nat_server_on_read, &server->sockets[index])) == nullptr ||
            event_add(server->events[index], nullptr) != 0) {
            test_nat_server_stop(server);
            return false;
        }
    }
    return true;
}

static void test_nat_server_stop(test_nat_server* server)
{
    for (size_t index = 0u; index < server->sockets.size(); ++index) {
        if (server->events[index] != nullptr) {
            event_free(server->events[index]);
        }
        if (server->sockets[index].fd >= 0) {
            (void)close(server->sockets[index].fd);
        }
    }
}

#endif

struct test_nat_probe_callback {
    size_t                 calls      = 0u;
    utp_status_t           status     = UTP_STATUS_IO;
    utp_nat_probe_result_t result     = {};
    bool                   has_result = false;
};

static void test_nat_probe_complete(utp_context_t* context, utp_status_t status, const utp_nat_probe_result_t* result,
                                    void* user_data)
{
    auto* callback = static_cast<test_nat_probe_callback*>(user_data);

    (void)context;
    ++callback->calls;
    callback->status     = status;
    callback->has_result = result != nullptr;
    if (result != nullptr) {
        callback->result = *result;
    }
}

TEST_CASE("nat probe request has a fixed UTP packet layout", "[nat]")
{
    const std::array<uint8_t, UTP_NAT_PROBE_TOKEN_SIZE> token = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u,
    };
    std::array<uint8_t, UTP_NAT_PROBE_PACKET_SIZE> packet = {};
    utp_packet_header_t                            header = {};

    REQUIRE(utp_nat_probe_encode_request(packet.data(), 42u, UTP_NAT_PROBE_MESSAGE_PROBE_REQ,
                                         UTP_NAT_PROBE_PHASE_PROBE1, token.data()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_decode_header(&header, packet.data(), packet.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(header.scid == 0u);
    REQUIRE(header.dcid == 0u);
    REQUIRE(header.packet_number == 42u);
    REQUIRE(header.type == UTP_PACKET_TYPE_NAT_PROBE);
    REQUIRE(header.reserve == 0u);
    REQUIRE(header.payload_length == UTP_NAT_PROBE_PACKET_SIZE - UTP_PACKET_HEADER_SIZE);
    REQUIRE(packet[UTP_PACKET_HEADER_SIZE] == UTP_NAT_PROBE_VERSION);
    REQUIRE(packet[UTP_PACKET_HEADER_SIZE + 1u] == UTP_NAT_PROBE_MESSAGE_PROBE_REQ);
    REQUIRE(packet[UTP_PACKET_HEADER_SIZE + 2u] == UTP_NAT_PROBE_PHASE_PROBE1);
    REQUIRE(packet[UTP_PACKET_HEADER_SIZE + 3u] == 0u);
    REQUIRE(utp_nat_probe_encode_request(packet.data(), 0u, UTP_NAT_PROBE_MESSAGE_PROBE_REQ, UTP_NAT_PROBE_PHASE_PROBE1,
                                         token.data()) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("nat probe response decodes mandatory endpoint fields", "[nat]")
{
    const std::array<uint8_t, UTP_NAT_PROBE_TOKEN_SIZE> token = {
        10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u,
    };
    std::array<uint8_t, 128> payload         = {};
    std::array<uint8_t, 8>   mapped          = {};
    std::array<uint8_t, 8>   origin          = {};
    std::array<uint8_t, 8>   alternate       = {};
    utp_wire_writer_t        endpoint_writer = {};
    utp_wire_writer_t        writer          = {};
    utp_nat_probe_response_t response        = {};

    REQUIRE(utp_wire_writer_init(&endpoint_writer, mapped.data(), mapped.size()) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_endpoint(&endpoint_writer, 40000u, 1u);
    REQUIRE(utp_wire_writer_init(&endpoint_writer, origin.data(), origin.size()) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_endpoint(&endpoint_writer, 3478u, 10u);
    REQUIRE(utp_wire_writer_init(&endpoint_writer, alternate.data(), alternate.size()) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_endpoint(&endpoint_writer, 3479u, 20u);
    REQUIRE(utp_wire_writer_init(&writer, payload.data(), payload.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_VERSION) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_MESSAGE_PROBE_RSP) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_PHASE_PROBE1) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, 0u) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_PROBE_TOKEN, token.data(), (uint16_t)token.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_MAPPED_ADDR, mapped.data(), (uint16_t)mapped.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_ORIGIN_ADDR, origin.data(), (uint16_t)origin.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_ALTERNATE_PROBE_ENDPOINT, alternate.data(),
                       (uint16_t)alternate.size());

    const size_t payload_length = payload.size() - writer.remaining;

    REQUIRE(utp_nat_probe_decode_response(payload.data(), payload_length, &response) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(response.message_type == UTP_NAT_PROBE_MESSAGE_PROBE_RSP);
    REQUIRE(response.phase == UTP_NAT_PROBE_PHASE_PROBE1);
    REQUIRE(response.token_length == token.size());
    REQUIRE(std::memcmp(response.token, token.data(), token.size()) == 0);
    REQUIRE(response.mapped.port == 40000u);
    REQUIRE(response.origin.port == 3478u);
    REQUIRE(response.has_alternate);
    REQUIRE(response.alternate.port == 3479u);
}

TEST_CASE("nat probe response rejects duplicate or request-only TLVs", "[nat]")
{
    const std::array<uint8_t, UTP_NAT_PROBE_TOKEN_SIZE> token = {
        1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
    };
    std::array<uint8_t, 128> payload         = {};
    std::array<uint8_t, 8>   endpoint        = {};
    utp_wire_writer_t        endpoint_writer = {};
    utp_wire_writer_t        writer          = {};
    utp_nat_probe_response_t response        = {};

    REQUIRE(utp_wire_writer_init(&endpoint_writer, endpoint.data(), endpoint.size()) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_endpoint(&endpoint_writer, 40000u, 1u);
    REQUIRE(utp_wire_writer_init(&writer, payload.data(), payload.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_VERSION) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_MESSAGE_PROBE_RSP) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_PHASE_PROBE1) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, 0u) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_PROBE_TOKEN, token.data(), (uint16_t)token.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_MAPPED_ADDR, endpoint.data(), (uint16_t)endpoint.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_ORIGIN_ADDR, endpoint.data(), (uint16_t)endpoint.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_PADDING, nullptr, 0u);

    const size_t payload_length = payload.size() - writer.remaining;

    REQUIRE(utp_nat_probe_decode_response(payload.data(), payload_length, &response) == UTP_INTERNAL_ERROR_PROTOCOL);
}

TEST_CASE("nat probe response only permits alternate endpoint during probe1", "[nat]")
{
    const std::array<uint8_t, UTP_NAT_PROBE_TOKEN_SIZE> token = {
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u,
    };
    std::array<uint8_t, 128> payload         = {};
    std::array<uint8_t, 8>   endpoint        = {};
    utp_wire_writer_t        endpoint_writer = {};
    utp_wire_writer_t        writer          = {};
    utp_nat_probe_response_t response        = {};

    REQUIRE(utp_wire_writer_init(&endpoint_writer, endpoint.data(), endpoint.size()) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_endpoint(&endpoint_writer, 3478u, 1u);
    REQUIRE(utp_wire_writer_init(&writer, payload.data(), payload.size()) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_VERSION) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_MESSAGE_PROBE_RSP) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, UTP_NAT_PROBE_PHASE_PROBE2) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_wire_write_u8(&writer, 0u) == UTP_INTERNAL_ERROR_OK);
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_PROBE_TOKEN, token.data(), (uint16_t)token.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_MAPPED_ADDR, endpoint.data(), (uint16_t)endpoint.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_ORIGIN_ADDR, endpoint.data(), (uint16_t)endpoint.size());
    test_nat_write_tlv(&writer, UTP_NAT_PROBE_TLV_ALTERNATE_PROBE_ENDPOINT, endpoint.data(), (uint16_t)endpoint.size());

    REQUIRE(utp_nat_probe_decode_response(payload.data(), payload.size() - writer.remaining, &response) ==
            UTP_INTERNAL_ERROR_PROTOCOL);
}

TEST_CASE("nat probe timeout returns a normal UDP blocked result", "[nat][context]")
{
    event_base*             event_base      = event_base_new();
    utp_context_options_t   context_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_nat_probe_options_t probe_options   = UTP_NAT_PROBE_OPTIONS_INIT;
    utp_context_t*          context         = nullptr;
    test_nat_probe_callback callback        = {};
    uint16_t                local_port      = 0u;

    REQUIRE(event_base != nullptr);
    context_options.event_base = event_base;
    REQUIRE(utp_context_create(&context_options, &context) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(context, "127.0.0.1", 0u, nullptr, &local_port) == UTP_STATUS_OK);
    REQUIRE(local_port != 0u);
    probe_options.nat_service_address = "127.0.0.1";
    probe_options.nat_service_port    = 9u;
    probe_options.phase_timeout_ms    = UTP_NAT_PROBE_MIN_TIMEOUT;
    REQUIRE(utp_context_probe_nat(context, &probe_options, test_nat_probe_complete, &callback) == UTP_STATUS_OK);
    REQUIRE(utp_context_probe_nat(context, &probe_options, test_nat_probe_complete, &callback) ==
            UTP_STATUS_IN_PROGRESS);
    for (uint8_t index = 0u; index < 4u && callback.calls == 0u; ++index) {
        REQUIRE(event_base_loop(event_base, EVLOOP_ONCE) == 0);
    }
    REQUIRE(callback.calls == 1u);
    REQUIRE(callback.status == UTP_STATUS_OK);
    REQUIRE(callback.has_result);
    REQUIRE(callback.result.nat_class == UTP_NAT_CLASS_UDP_BLOCKED);
    REQUIRE(utp_context_cancel_nat_probe(context) == UTP_STATUS_NOT_FOUND);
    utp_context_destroy(context);
    event_base_free(event_base);
}

#if defined(__linux__)

TEST_CASE("nat probe completes all phases with a responsive service", "[nat][context]")
{
    event_base*             event_base      = event_base_new();
    utp_context_options_t   context_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_nat_probe_options_t probe_options   = UTP_NAT_PROBE_OPTIONS_INIT;
    utp_context_t*          context         = nullptr;
    test_nat_probe_callback callback        = {};
    test_nat_server         server          = {};
    uint16_t                local_port      = 0u;

    REQUIRE(event_base != nullptr);
    REQUIRE(test_nat_server_start(&server, event_base));
    context_options.event_base = event_base;
    REQUIRE(utp_context_create(&context_options, &context) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(context, "127.0.0.1", 0u, nullptr, &local_port) == UTP_STATUS_OK);
    probe_options.nat_service_address = "127.0.0.1";
    probe_options.nat_service_port    = server.endpoints[TEST_NAT_SERVER_PRIMARY].port;
    probe_options.phase_timeout_ms    = 60u;
    REQUIRE(utp_context_probe_nat(context, &probe_options, test_nat_probe_complete, &callback) == UTP_STATUS_OK);
    for (uint8_t index = 0u; index < 64u && callback.calls == 0u; ++index) {
        REQUIRE(event_base_loop(event_base, EVLOOP_ONCE) == 0);
    }
    REQUIRE(callback.calls == 1u);
    REQUIRE(callback.status == UTP_STATUS_OK);
    REQUIRE(callback.has_result);
    REQUIRE(callback.result.nat_class == UTP_NAT_CLASS_FULL_CONE);
    REQUIRE(callback.result.address_family == 4u);
    REQUIRE(callback.result.primary_mapped_endpoint.family == 4u);
    REQUIRE(callback.result.secondary_mapped_endpoint.family == 4u);
    REQUIRE(callback.result.primary_mapped_endpoint.port == local_port);
    REQUIRE(callback.result.secondary_mapped_endpoint.port == local_port);
    REQUIRE(callback.result.primary_rtt_ms >= 0);
    REQUIRE(callback.result.secondary_rtt_ms >= 0);
    REQUIRE(callback.result.port_sample_count == 1u);
    utp_context_destroy(context);
    test_nat_server_stop(&server);
    event_base_free(event_base);
}

#endif
