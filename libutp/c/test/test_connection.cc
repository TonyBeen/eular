#define CATCH_CONFIG_MAIN

#include <array>
#include <cstring>

#include <catch2/catch.hpp>

extern "C" {
#include "connection/connection.h"
#include "proto/ack.h"
}

namespace {

std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version_frame()
{
    std::array<uint8_t, UTP_FRAME_VERSION_SIZE> frame   = {};
    const utp_frame_version_t                   version = {UTP_PROTOCOL_VERSION};

    REQUIRE(utp_frame_version_encode(frame.data(), frame.size(), &version) == UTP_INTERNAL_ERROR_OK);
    return frame;
}

utp_address_t loopback_address(uint16_t port)
{
    utp_address_t address = {};

    REQUIRE(utp_address_parse(&address, "127.0.0.1", port) == UTP_INTERNAL_ERROR_OK);
    return address;
}

void send_to_peer(utp_connection_t* sender, utp_connection_t* receiver, const utp_address_t* sender_address,
                  const utp_address_t* receiver_address, uint64_t now_us)
{
    utp_packet_out_t*         packet = utp_connection_next_packet_to_send(sender);
    std::array<uint8_t, 1280> wire   = {};
    size_t                    length = 0u;

    REQUIRE(packet != nullptr);
    REQUIRE(utp_packet_out_flatten(packet, wire.data(), wire.size(), &length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(sender, packet, now_us) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(receiver, wire.data(), length, sender_address, now_us) ==
            UTP_INTERNAL_ERROR_OK);
    (void)receiver_address;
}

}  // namespace

TEST_CASE("active connection binds a peer CID and replies to a Handshake without waiting for an ACK",
          "[connection][handshake]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE>        version = version_frame();
    const std::array<uint8_t, UTP_FRAME_HANDSHAKE_DONE_SIZE> done    = [] {
        std::array<uint8_t, UTP_FRAME_HANDSHAKE_DONE_SIZE> frame = {};
        const utp_frame_handshake_done_t                   value = {1u};

        REQUIRE(utp_frame_handshake_done_encode(frame.data(), frame.size(), &value) == UTP_INTERNAL_ERROR_OK);
        return frame;
    }();
    const utp_address_t active_address  = loopback_address(10001u);
    const utp_address_t passive_address = loopback_address(10002u);
    utp_connection_t    active          = {};
    utp_connection_t    passive         = {};
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 11u, 0u, &passive_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 22u, 11u, &active_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_INITIAL, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->raw_data[4] == 0u);
    REQUIRE(packet->raw_data[5] == 0u);
    REQUIRE(packet->raw_data[6] == 0u);
    REQUIRE(packet->raw_data[7] == 0u);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_state(&active) == UTP_CONNECTION_STATE_INITIAL_SENT);

    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_HANDSHAKE, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    send_to_peer(&passive, &active, &passive_address, &active_address, 200u);
    REQUIRE(utp_connection_is_connected(&active));
    REQUIRE(active.peer_cid == 22u);
    REQUIRE(active.peer_handshake_packet_number == 1u);

    const size_t unacked_before_handshake_done = utp_send_control_unacked_packet_count(&active.send_control);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_CTRL, done.data(), done.size(), false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 300u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_is_connected(&active));
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == unacked_before_handshake_done);

    REQUIRE(utp_connection_queue_close(&active, 0u) == UTP_INTERNAL_ERROR_OK);
    send_to_peer(&active, &passive, &active_address, &passive_address, 400u);
    REQUIRE(utp_connection_state(&active) == UTP_CONNECTION_STATE_CLOSING);
    REQUIRE(utp_connection_state(&passive) == UTP_CONNECTION_STATE_DRAINING);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection retransmission timeout resends a tracked handshake packet with a fresh packet number",
          "[connection][retransmission]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version         = version_frame();
    const utp_address_t                               passive_address = loopback_address(10009u);
    utp_connection_t                                  active          = {};
    utp_packet_out_t*                                 packet;
    utp_packet_view_t                                 view = {};
    uint64_t                                          deadline;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 55u, 0u, &passive_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_INITIAL, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 1u);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 1u);
    deadline = utp_connection_retransmission_deadline(&active);
    REQUIRE(deadline > 100u);

    REQUIRE(utp_connection_on_retransmission_timeout(&active, deadline) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 0u);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 2u);
    REQUIRE(utp_packet_view_decode(&view, packet->raw_data, packet->data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(view.header.packet_number == 2u);
    REQUIRE(view.header.type == UTP_PACKET_TYPE_INITIAL);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, deadline + 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 1u);
    REQUIRE(utp_connection_retransmission_deadline(&active) > deadline + 1u);

    utp_connection_cleanup(&active);
}

TEST_CASE("connection releases acknowledged and non-tracked packets back to its pool", "[connection][ack]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version         = version_frame();
    const utp_address_t                               active_address  = loopback_address(10003u);
    const utp_address_t                               passive_address = loopback_address(10004u);
    utp_connection_t                                  active          = {};
    utp_connection_t                                  passive         = {};
    utp_ack_range_t                                   ranges[]        = {{1u, 1u}};
    const utp_ack_info_t                              ack             = {1u, 0u, ranges, 1u, 1u};
    std::array<uint8_t, UTP_ACK_FRAME_HEADER_SIZE>    ack_frame       = {};
    std::array<uint8_t, 1280>                         wire            = {};
    size_t                                            ack_length      = 0u;
    utp_packet_out_t*                                 packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 33u, 44u, &passive_address, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 44u, 33u, &active_address, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_INITIAL, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 1u);

    REQUIRE(utp_ack_encode(ack_frame.data(), ack_frame.size(), &ack, 0u, &ack_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_CTRL, ack_frame.data(), ack_length, false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->data_size <= wire.size());
    std::memcpy(wire.data(), packet->raw_data, packet->data_size);
    const size_t wire_length = packet->data_size;
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&passive.send_control) == 0u);
    REQUIRE(utp_connection_on_packet_received(&active, wire.data(), wire_length, &passive_address, 200u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 0u);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection queues an ACK frame from receive history and clears peer unacked packets", "[connection][ack]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version         = version_frame();
    const uint8_t                                     ping            = UTP_FRAME_TYPE_PING;
    const utp_address_t                               active_address  = loopback_address(10007u);
    const utp_address_t                               passive_address = loopback_address(10008u);
    utp_connection_t                                  active          = {};
    utp_connection_t                                  passive         = {};
    std::array<uint8_t, 1280>                         ack_wire        = {};
    size_t                                            ack_wire_length = 0u;
    utp_packet_out_t*                                 packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 33u, 44u, &passive_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 44u, 33u, &active_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_INITIAL, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    send_to_peer(&active, &passive, &active_address, &passive_address, 100u);
    REQUIRE(utp_connection_ack_pending_count(&passive) == 1u);
    REQUIRE(utp_connection_ack_deadline(&passive) != 0u);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 1u);

    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    send_to_peer(&active, &passive, &active_address, &passive_address, 200u);
    REQUIRE(utp_connection_ack_pending_count(&passive) == 2u);
    REQUIRE(utp_connection_ack_deadline(&passive) == 0u);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 2u);

    REQUIRE(utp_connection_queue_ack(&passive, 300u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_ack_pending_count(&passive) == 2u);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->data_size <= ack_wire.size());
    std::memcpy(ack_wire.data(), packet->raw_data, packet->data_size);
    ack_wire_length = packet->data_size;
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 300u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_ack_pending_count(&passive) == 0u);
    REQUIRE(utp_connection_on_packet_received(&active, ack_wire.data(), ack_wire_length, &passive_address, 400u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 0u);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection rejects a datagram with a peer or destination CID mismatch", "[connection][validation]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version       = version_frame();
    const utp_address_t                               expected_peer = loopback_address(10005u);
    const utp_address_t                               wrong_peer    = loopback_address(10006u);
    utp_connection_t                                  connection    = {};
    utp_packet_header_t header = {11u, 77u, 1u, (uint16_t)version.size(), UTP_PACKET_TYPE_INITIAL, 0u};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_VERSION_SIZE> packet = {};

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &expected_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet.data() + UTP_PACKET_HEADER_SIZE, version.data(), version.size());
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &wrong_peer, 100u) ==
            UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    header.dcid = 78u;
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &expected_peer, 100u) ==
            UTP_INTERNAL_ERROR_PROTOCOL);
    utp_connection_cleanup(&connection);
}
