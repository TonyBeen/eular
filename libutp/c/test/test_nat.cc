#define CATCH_CONFIG_MAIN

#include <array>
#include <cstdint>
#include <cstring>

#include <catch2/catch.hpp>
#include <event2/event.h>

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

struct test_nat_probe_callback {
    size_t                 calls = 0u;
    utp_status_t           status = UTP_STATUS_IO;
    utp_nat_probe_result_t result = {};
    bool                   has_result = false;
};

static void test_nat_probe_complete(utp_context_t* context, utp_status_t status, const utp_nat_probe_result_t* result,
                                    void* user_data)
{
    auto* callback = static_cast<test_nat_probe_callback*>(user_data);

    (void)context;
    ++callback->calls;
    callback->status = status;
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
    utp_packet_header_t                             header = {};

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
    REQUIRE(utp_nat_probe_encode_request(packet.data(), 0u, UTP_NAT_PROBE_MESSAGE_PROBE_REQ,
                                         UTP_NAT_PROBE_PHASE_PROBE1, token.data()) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("nat probe response decodes mandatory endpoint fields", "[nat]")
{
    const std::array<uint8_t, UTP_NAT_PROBE_TOKEN_SIZE> token = {
        10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u,
    };
    std::array<uint8_t, 128> payload = {};
    std::array<uint8_t, 8>   mapped  = {};
    std::array<uint8_t, 8>   origin  = {};
    std::array<uint8_t, 8>   alternate = {};
    utp_wire_writer_t        endpoint_writer = {};
    utp_wire_writer_t        writer = {};
    utp_nat_probe_response_t response = {};

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
    std::array<uint8_t, 128> payload = {};
    std::array<uint8_t, 8>   endpoint = {};
    utp_wire_writer_t        endpoint_writer = {};
    utp_wire_writer_t        writer = {};
    utp_nat_probe_response_t response = {};

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

TEST_CASE("nat probe timeout returns a normal UDP blocked result", "[nat][context]")
{
    event_base*             event_base = event_base_new();
    utp_context_options_t   context_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_nat_probe_options_t probe_options = UTP_NAT_PROBE_OPTIONS_INIT;
    utp_context_t*          context = nullptr;
    test_nat_probe_callback callback = {};
    uint16_t                local_port = 0u;

    REQUIRE(event_base != nullptr);
    context_options.event_base = event_base;
    REQUIRE(utp_context_create(&context_options, &context) == UTP_STATUS_OK);
    REQUIRE(utp_context_bind(context, "127.0.0.1", 0u, nullptr, &local_port) == UTP_STATUS_OK);
    REQUIRE(local_port != 0u);
    probe_options.nat_service_address = "127.0.0.1";
    probe_options.nat_service_port = 9u;
    probe_options.phase_timeout_ms = UTP_NAT_PROBE_MIN_TIMEOUT;
    REQUIRE(utp_context_probe_nat(context, &probe_options, test_nat_probe_complete, &callback) == UTP_STATUS_OK);
    REQUIRE(utp_context_probe_nat(context, &probe_options, test_nat_probe_complete, &callback) == UTP_STATUS_IN_PROGRESS);
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
