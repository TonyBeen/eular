#define CATCH_CONFIG_MAIN

#include <array>
#include <catch2/catch.hpp>
#include <cstring>

extern "C" {
#include "context/pending_incoming.h"
#include "proto/proto.h"
}

namespace {

utp_address_t loopback_address(uint16_t port) {
    utp_address_t address = {};

    REQUIRE(utp_address_parse(&address, "127.0.0.1", port) == UTP_INTERNAL_ERROR_OK);
    return address;
}

std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_VERSION_SIZE> version_packet(uint32_t scid, uint32_t dcid,
                                                                                    uint64_t packet_number) {
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_VERSION_SIZE> packet = {};
    const utp_packet_header_t header  = {scid, dcid, packet_number, UTP_FRAME_VERSION_SIZE, UTP_PACKET_TYPE_CTRL, 0u};
    const utp_frame_version_t version = {UTP_PROTOCOL_VERSION};

    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_version_encode(packet.data() + UTP_PACKET_HEADER_SIZE, UTP_FRAME_VERSION_SIZE, &version) ==
            UTP_INTERNAL_ERROR_OK);
    return packet;
}

std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_HANDSHAKE_DONE_SIZE> handshake_done_packet(
    uint32_t scid, uint32_t dcid, uint64_t packet_number, uint64_t ack_number) {
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_HANDSHAKE_DONE_SIZE> packet = {};
    const utp_packet_header_t header = {scid, dcid, packet_number, UTP_FRAME_HANDSHAKE_DONE_SIZE, UTP_PACKET_TYPE_CTRL,
                                        0u};
    const utp_frame_handshake_done_t done = {ack_number};

    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_handshake_done_encode(packet.data() + UTP_PACKET_HEADER_SIZE, UTP_FRAME_HANDSHAKE_DONE_SIZE,
                                            &done) == UTP_INTERNAL_ERROR_OK);
    return packet;
}

struct replay_capture {
    size_t   count             = 0u;
    uint64_t packet_numbers[2] = {};
};

utp_internal_error_t capture_replay(const uint8_t *packet, size_t packet_length, void *user_data) {
    auto               *capture = static_cast<replay_capture *>(user_data);
    utp_packet_header_t header  = {};

    if (capture->count >= 2u || utp_proto_decode_header(&header, packet, packet_length) != UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    capture->packet_numbers[capture->count++] = header.packet_number;
    return UTP_INTERNAL_ERROR_OK;
}

}  // namespace

TEST_CASE("pending incoming promotes only for a matching accepted HandshakeDone", "[pending][handshake]") {
    const utp_address_t           peer     = loopback_address(12001u);
    const auto                    first    = version_packet(11u, 22u, 1u);
    const auto                    wrong    = handshake_done_packet(11u, 22u, 2u, 6u);
    const auto                    matching = handshake_done_packet(11u, 22u, 3u, 7u);
    std::array<uint8_t, 256>      storage  = {};
    utp_pending_incoming_t        pending  = {};
    utp_pending_incoming_result_t result   = UTP_PENDING_INCOMING_PROMOTE;
    replay_capture                capture  = {};

    REQUIRE(utp_pending_incoming_init(&pending, 22u, 11u, &peer, storage.data(), storage.size(), 2u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_pending_incoming_on_packet(&pending, first.data(), first.size(), &peer, &result) ==
            UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_pending_incoming_accept(&pending) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_pending_incoming_mark_handshake_sent(&pending, 7u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_pending_incoming_on_packet(&pending, first.data(), first.size(), &peer, &result) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(result == UTP_PENDING_INCOMING_BUFFERED);
    REQUIRE(utp_pending_incoming_on_packet(&pending, wrong.data(), wrong.size(), &peer, &result) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(result == UTP_PENDING_INCOMING_BUFFERED);
    REQUIRE(utp_pending_incoming_on_packet(&pending, matching.data(), matching.size(), &peer, &result) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(result == UTP_PENDING_INCOMING_PROMOTE);
    REQUIRE(pending.packet_count == 2u);
    REQUIRE(utp_pending_incoming_replay(&pending, capture_replay, &capture) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(capture.count == 2u);
    REQUIRE(capture.packet_numbers[0] == 1u);
    REQUIRE(capture.packet_numbers[1] == 2u);
}

TEST_CASE("pending incoming enforces bounded packet and byte storage", "[pending][bounds]") {
    const utp_address_t peer   = loopback_address(12002u);
    const auto          packet = version_packet(31u, 41u, 1u);
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_VERSION_SIZE + sizeof(uint16_t)> storage = {};
    utp_pending_incoming_t                                                                  pending = {};
    utp_pending_incoming_result_t result = UTP_PENDING_INCOMING_PROMOTE;

    REQUIRE(utp_pending_incoming_init(&pending, 41u, 31u, &peer, storage.data(), storage.size(), 1u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_pending_incoming_accept(&pending) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_pending_incoming_mark_handshake_sent(&pending, 9u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_pending_incoming_on_packet(&pending, packet.data(), packet.size(), &peer, &result) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_pending_incoming_on_packet(&pending, packet.data(), packet.size(), &peer, &result) ==
            UTP_INTERNAL_ERROR_LIMIT);
}
