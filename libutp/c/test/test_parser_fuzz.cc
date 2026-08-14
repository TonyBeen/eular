#define CATCH_CONFIG_MAIN

#include <array>
#include <cstddef>
#include <cstdint>

#include <catch2/catch.hpp>

extern "C" {
#include "proto/frame.h"
#include "proto/proto.h"
}

namespace {

constexpr size_t   kParserFuzzPacketCapacity = 2048u;
constexpr uint32_t kParserFuzzIterations     = 100000u;

static uint64_t    parser_fuzz_next(uint64_t* state)
{
    *state ^= *state << 13u;
    *state ^= *state >> 7u;
    *state ^= *state << 17u;
    return *state;
}

static void parser_fuzz_fill(uint8_t* data, size_t length, uint64_t* state)
{
    size_t index;

    for (index = 0u; index < length; ++index) {
        data[index] = static_cast<uint8_t>(parser_fuzz_next(state));
    }
}

static bool parser_fuzz_walk_packet(const uint8_t* packet, size_t packet_length)
{
    utp_packet_header_t  header = {};
    utp_packet_view_t    view   = {};
    size_t               offset = 0u;
    utp_internal_error_t error;

    error = utp_proto_decode_header(&header, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return true;
    }
    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return true;
    }
    while (offset < view.payload_length) {
        const uint8_t* frame_data;
        size_t         frame_length;
        size_t         previous_offset = offset;
        uint8_t        frame_type;

        error = utp_packet_view_next_frame(&view, &offset, &frame_type, &frame_data, &frame_length);
        if (error != UTP_INTERNAL_ERROR_OK || frame_type == UTP_FRAME_TYPE_INVALID || frame_length == 0u ||
            frame_data != view.payload + previous_offset || offset > view.payload_length) {
            return false;
        }
    }
    return offset == view.payload_length;
}

static bool parser_fuzz_build_structured_packet(uint8_t* packet, size_t capacity, uint64_t* state,
                                                size_t* packet_length)
{
    utp_packet_header_t header;
    size_t              payload_length;

    payload_length        = (size_t)(parser_fuzz_next(state) % (capacity - UTP_PACKET_HEADER_SIZE + 1u));
    header.scid           = static_cast<uint32_t>(parser_fuzz_next(state));
    header.dcid           = static_cast<uint32_t>(parser_fuzz_next(state));
    header.packet_number  = parser_fuzz_next(state) & UTP_PACKET_NUMBER_MAX;
    header.payload_length = static_cast<uint16_t>(payload_length);
    header.type           = static_cast<uint8_t>(UTP_PACKET_TYPE_INITIAL + parser_fuzz_next(state) % 6u);
    header.reserve        = static_cast<uint8_t>(parser_fuzz_next(state));
    if (utp_proto_encode_header(packet, capacity, &header) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    parser_fuzz_fill(packet + UTP_PACKET_HEADER_SIZE, payload_length, state);
    *packet_length = UTP_PACKET_HEADER_SIZE + payload_length;
    return true;
}

}  // namespace

TEST_CASE("packet parser tolerates 100000 deterministic mixed inputs", "[fuzz][proto]")
{
    std::array<uint8_t, kParserFuzzPacketCapacity> packet = {};
    uint64_t                                       state  = UINT64_C(0x99d3a74c5b1e628f);
    bool                                           valid  = true;
    uint32_t                                       index;

    for (index = 0u; index < kParserFuzzIterations; ++index) {
        size_t packet_length;

        if ((index & 1u) == 0u) {
            parser_fuzz_fill(packet.data(), packet.size(), &state);
            packet_length = (size_t)(parser_fuzz_next(&state) % (packet.size() + 1u));
        } else {
            valid = parser_fuzz_build_structured_packet(packet.data(), packet.size(), &state, &packet_length);
            if (!valid) {
                break;
            }
        }
        valid = parser_fuzz_walk_packet(packet.data(), packet_length);
        if (!valid) {
            break;
        }
    }
    REQUIRE(valid);
}
