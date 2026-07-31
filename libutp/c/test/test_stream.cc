#define CATCH_CONFIG_MAIN

#include <array>
#include <catch2/catch.hpp>
#include <cstring>

extern "C" {
#include "connection/connection.h"
}

namespace {

utp_address_t loopback_address(uint16_t port) {
    utp_address_t address = {};

    REQUIRE(utp_address_parse(&address, "127.0.0.1", port) == UTP_INTERNAL_ERROR_OK);
    return address;
}

std::array<uint8_t, UTP_FRAME_STREAM_HEADER_SIZE + 16u> stream_frame(uint32_t stream_id, uint64_t offset,
                                                                     const char *data, bool fin) {
    std::array<uint8_t, UTP_FRAME_STREAM_HEADER_SIZE + 16u> encoded = {};
    const size_t                                            length  = std::strlen(data);
    const utp_frame_stream_t frame = {static_cast<uint8_t>(fin ? UTP_STREAM_FLAG_FIN : UTP_STREAM_FLAG_NONE), stream_id,
                                      offset, reinterpret_cast<const uint8_t *>(data), static_cast<uint16_t>(length)};

    REQUIRE(length <= 16u);
    REQUIRE(utp_frame_stream_encode(encoded.data(), encoded.size(), &frame) == UTP_INTERNAL_ERROR_OK);
    return encoded;
}

void transfer_next_packet(utp_connection_t *sender, utp_connection_t *receiver, const utp_address_t *sender_address,
                          uint64_t now_us) {
    utp_packet_out_t         *packet = utp_connection_next_packet_to_send(sender);
    std::array<uint8_t, 1280> wire   = {};
    size_t                    length;

    REQUIRE(packet != nullptr);
    REQUIRE(packet->data_size <= wire.size());
    REQUIRE(utp_packet_out_flatten(packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == packet->data_size);
    REQUIRE(utp_connection_on_packet_sent(sender, packet, now_us) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(receiver, wire.data(), length, sender_address, now_us) ==
            UTP_INTERNAL_ERROR_OK);
}

}  // namespace

TEST_CASE("stream reassembles out-of-order frames and reports FIN after data is consumed", "[stream]") {
    utp_stream_t stream     = {};
    const auto   tail       = stream_frame(0u, 5u, "world", true);
    const auto   head       = stream_frame(0u, 0u, "hello", false);
    uint8_t      buffer[16] = {};
    size_t       length     = 0u;
    bool         fin        = false;

    utp_stream_init(&stream, 0u);
    {
        utp_frame_stream_t decoded = {};

        REQUIRE(utp_frame_stream_decode(&decoded, tail.data(), tail.size()) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_stream_on_frame(&stream, &decoded) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_stream_readable_bytes(&stream) == 0u);
        REQUIRE(utp_frame_stream_decode(&decoded, head.data(), head.size()) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_stream_on_frame(&stream, &decoded) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_stream_readable_bytes(&stream) == 10u);
    REQUIRE(utp_stream_read(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 10u);
    REQUIRE(std::memcmp(buffer, "helloworld", 10u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_read(&stream, buffer, sizeof(buffer), &length, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 0u);
    REQUIRE(fin);
}

TEST_CASE("stream write credit includes in-flight bytes until ACKed", "[stream]") {
    utp_stream_t                                                                        stream           = {};
    std::array<uint8_t, UTP_STREAM_SEND_BUFFER_CAPACITY>                                data             = {};
    std::array<uint8_t, UTP_STREAM_SEND_BUFFER_CAPACITY + UTP_FRAME_STREAM_HEADER_SIZE> payload          = {};
    size_t                                                                              payload_length   = 0u;
    uint32_t                                                                            stream_data_size = 0u;
    uint64_t                                                                            stream_offset    = 0u;
    bool                                                                                fin              = false;

    utp_stream_init(&stream, 0u);
    REQUIRE(utp_stream_write(&stream, data.data(), data.size(), false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == data.size());
    REQUIRE_FALSE(fin);
    REQUIRE(utp_stream_commit_built_frame(&stream, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_send_in_flight_bytes(&stream) == data.size());
    REQUIRE(utp_stream_write(&stream, data.data(), 1u, false) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    REQUIRE(utp_stream_on_packet_acked(&stream, stream_data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_send_in_flight_bytes(&stream) == 0u);
    REQUIRE(utp_stream_write(&stream, data.data(), 1u, false) == UTP_INTERNAL_ERROR_OK);
}

TEST_CASE("stream send and receive paths enforce stream-level flow-control limits", "[stream][flow]") {
    utp_stream_t            stream           = {};
    std::array<uint8_t, 64> payload          = {};
    size_t                  payload_length   = 0u;
    uint32_t                stream_data_size = 0u;
    uint64_t                stream_offset    = 0u;
    bool                    fin              = false;

    utp_stream_init(&stream, 0u);
    stream.peer_max_stream_data = 2u;
    REQUIRE(utp_stream_write(&stream, reinterpret_cast<const uint8_t *>("abc"), 3u, false) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == 2u);
    REQUIRE(utp_stream_commit_built_frame(&stream, stream_data_size, fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_WOULD_BLOCK);
    utp_stream_update_peer_max_stream_data(&stream, 3u);
    REQUIRE(utp_stream_build_frame(&stream, payload.data(), payload.size(), &payload_length, &stream_data_size,
                                   &stream_offset, &fin) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_data_size == 1u);

    stream.local_max_stream_data_advertised = 1u;
    {
        const auto         frame   = stream_frame(0u, 0u, "ab", false);
        utp_frame_stream_t decoded = {};

        REQUIRE(utp_frame_stream_decode(&decoded, frame.data(), frame.size()) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_stream_on_frame(&stream, &decoded) == UTP_INTERNAL_ERROR_LIMIT);
    }
}

TEST_CASE("connection allocates a local stream, sends STREAM frames, and the peer reads the data", "[stream]") {
    const utp_address_t active_address  = loopback_address(13001u);
    const utp_address_t passive_address = loopback_address(13002u);
    utp_connection_t    active          = {};
    utp_connection_t    passive         = {};
    uint32_t            stream_id       = UINT32_MAX;
    uint8_t             buffer[32]      = {};
    size_t              length          = 0u;
    bool                fin             = false;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 11u, 22u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 22u, 11u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t *>("abc"), 3u, true) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&active, &passive, &active_address, 100u);
    {
        utp_stream_t *stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_send_in_flight_bytes(stream) == 3u);
    }

    REQUIRE(utp_connection_stream_read(&passive, stream_id, buffer, sizeof(buffer), &length, &fin) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 3u);
    REQUIRE(std::memcmp(buffer, "abc", 3u) == 0);
    REQUIRE_FALSE(fin);
    REQUIRE(utp_connection_stream_read(&passive, stream_id, buffer, sizeof(buffer), &length, &fin) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(length == 0u);
    REQUIRE(fin);
    REQUIRE(utp_connection_queue_ack(&passive, 200u) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 200u);
    {
        utp_stream_t *stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(utp_stream_send_in_flight_bytes(stream) == 0u);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection STREAM packets use an external data slice", "[stream][zero-copy]") {
    const utp_address_t active_address  = loopback_address(13011u);
    const utp_address_t passive_address = loopback_address(13012u);
    utp_connection_t    active          = {};
    uint32_t            stream_id       = UINT32_MAX;
    utp_packet_out_t   *packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 31u, 32u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);

    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_stream_write(&active, stream_id, reinterpret_cast<const uint8_t *>("abcd"), 4u, false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->slice_count == 2u);
    REQUIRE(packet->slices[0].source == UTP_PACKET_OUT_SLICE_RAW_OFFSET);
    REQUIRE(packet->slices[0].length == UTP_PACKET_HEADER_SIZE + UTP_FRAME_STREAM_HEADER_SIZE);
    REQUIRE(packet->slices[1].source == UTP_PACKET_OUT_SLICE_EXTERNAL);
    REQUIRE(packet->slices[1].length == 4u);
    REQUIRE(packet->slices[1].data != nullptr);

    utp_packet_out_pool_release(&active.packet_pool, packet);
    utp_connection_cleanup(&active);
    (void)active_address;
}

TEST_CASE("connection applies incoming flow-control limit updates monotonically", "[stream][flow]") {
    const utp_address_t               active_address  = loopback_address(13003u);
    const utp_address_t               passive_address = loopback_address(13004u);
    utp_connection_t                  active          = {};
    utp_connection_t                  passive         = {};
    uint32_t                          stream_id       = UINT32_MAX;
    uint8_t                           payload[13]     = {};
    const utp_frame_max_data_t        max_data        = {UINT64_C(123456)};
    const utp_frame_max_stream_data_t max_stream_data = {0u, UINT64_C(654321)};

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 11u, 22u, &passive_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 22u, 11u, &active_address, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    active.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&active.send_control, true);
    REQUIRE(utp_connection_create_stream(&active, true, &stream_id) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(stream_id == 0u);
    active.peer_max_data = 1u;
    {
        utp_stream_t *stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        stream->peer_max_stream_data = 1u;
    }

    REQUIRE(utp_frame_max_data_encode(payload, sizeof(payload), &max_data) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, payload, 9u, false) == UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 100u);
    REQUIRE(active.peer_max_data == max_data.maximum_data);

    REQUIRE(utp_frame_max_stream_data_encode(payload, sizeof(payload), &max_stream_data) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), false) ==
            UTP_INTERNAL_ERROR_OK);
    transfer_next_packet(&passive, &active, &passive_address, 200u);
    {
        utp_stream_t *stream = utp_connection_find_stream(&active, stream_id);

        REQUIRE(stream != nullptr);
        REQUIRE(stream->peer_max_stream_data == max_stream_data.maximum_stream_data);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}
