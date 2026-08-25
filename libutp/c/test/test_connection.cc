#define CATCH_CONFIG_MAIN

#include <algorithm>
#include <array>
#include <cstring>

#include <catch2/catch.hpp>

extern "C" {
#include "connection/connection.h"
#include "proto/ack.h"
#include "proto/packet_in.h"
}

namespace {

std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version_frame()
{
    std::array<uint8_t, UTP_FRAME_VERSION_SIZE> frame   = {};
    const utp_frame_version_t                   version = {UTP_PROTOCOL_VERSION};

    REQUIRE(utp_frame_version_encode(frame.data(), frame.size(), &version) == UTP_INTERNAL_ERROR_OK);
    return frame;
}

std::array<uint8_t, UTP_FRAME_VERSION_SIZE + UTP_FRAME_TRANSPORT_PARAMS_SIZE + UTP_FRAME_ACK_FREQUENCY_SIZE>
handshake_negotiation_payload(const utp_frame_transport_params_t& params)
{
    std::array<uint8_t, UTP_FRAME_VERSION_SIZE + UTP_FRAME_TRANSPORT_PARAMS_SIZE + UTP_FRAME_ACK_FREQUENCY_SIZE>
                                    payload   = {};
    const utp_frame_version_t       version   = {UTP_PROTOCOL_VERSION};
    const utp_frame_ack_frequency_t frequency = {25u, 2u, 3u};

    REQUIRE(utp_frame_version_encode(payload.data(), UTP_FRAME_VERSION_SIZE, &version) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_transport_params_encode(payload.data() + UTP_FRAME_VERSION_SIZE, UTP_FRAME_TRANSPORT_PARAMS_SIZE,
                                              &params) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_frame_ack_frequency_encode(payload.data() + UTP_FRAME_VERSION_SIZE + UTP_FRAME_TRANSPORT_PARAMS_SIZE,
                                           UTP_FRAME_ACK_FREQUENCY_SIZE, &frequency) == UTP_INTERNAL_ERROR_OK);
    return payload;
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

void send_encrypted_to_peer(utp_connection_t* sender, utp_connection_t* receiver, const utp_address_t* sender_address,
                            uint64_t now_us)
{
    utp_packet_out_t*         packet = utp_connection_next_packet_to_send(sender);
    std::array<uint8_t, 1280> wire   = {};
    size_t                    length = 0u;

    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_encode_packet_wire(sender, packet, wire.data(), wire.size(), &length) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(sender, packet, now_us) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(receiver, wire.data(), length, sender_address, now_us) ==
            UTP_INTERNAL_ERROR_OK);
}

}  // namespace

TEST_CASE("connection query APIs report streams statistics and description", "[connection][query]")
{
    const utp_address_t          peer        = loopback_address(14001u);
    const uint8_t                stream_data = UINT8_C(0xa5);
    utp_connection_t             connection  = {};
    utp_packet_out_t*            packet      = nullptr;
    uint32_t                     stream_id   = UINT32_MAX;
    uint64_t                     deadline;
    utp_connection_statistic_t   statistic   = {};
    utp_connection_description_t description = {};

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 201u, 202u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);

    REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_ALL) == 0);
    REQUIRE(utp_connection_creatable_stream_count(&connection, UTP_STREAM_TYPE_BIDIRECTIONAL) == 64);
    REQUIRE(utp_connection_creatable_stream_count(&connection, UTP_STREAM_TYPE_UNIDIRECTIONAL) == 32);
    REQUIRE(utp_connection_create_stream(&connection, UTP_STREAM_TYPE_BIDIRECTIONAL, &stream_id) == UTP_STATUS_OK);
    REQUIRE(stream_id == 0u);
    REQUIRE(utp_connection_create_stream(&connection, UTP_STREAM_TYPE_UNIDIRECTIONAL, &stream_id) == UTP_STATUS_OK);
    REQUIRE(stream_id == UTP_STREAM_UNIDIRECTIONAL);
    REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_BIDIRECTIONAL) == 1);
    REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_UNIDIRECTIONAL) == 1);
    REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_ALL) == 2);
    REQUIRE(utp_connection_creatable_stream_count(&connection, UTP_STREAM_TYPE_BIDIRECTIONAL) == 63);
    REQUIRE(utp_connection_creatable_stream_count(&connection, UTP_STREAM_TYPE_UNIDIRECTIONAL) == 31);
    REQUIRE(utp_connection_stream_count(&connection, static_cast<utp_stream_type_t>(2u)) == -1);
    REQUIRE(utp_connection_creatable_stream_count(&connection, UTP_STREAM_TYPE_ALL) == -1);

    {
        utp_stream_t* stream = utp_connection_get_stream(&connection, 0u);

        REQUIRE(stream != nullptr);
        // 保持流对象在哈希表中，模拟尚有 PacketOut 引用而不能回收的已关闭流。
        stream->local_fin_queued = true;
        stream->local_fin_sent   = true;
        stream->peer_fin         = true;
        REQUIRE(utp_stream_is_closed(stream));
        REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_BIDIRECTIONAL) == 0);
        REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_UNIDIRECTIONAL) == 1);
        REQUIRE(utp_connection_stream_count(&connection, UTP_STREAM_TYPE_ALL) == 1);
        stream->local_fin_queued = false;
        stream->local_fin_sent   = false;
        stream->peer_fin         = false;
    }

    REQUIRE(utp_connection_get_description(&connection, &description) == UTP_STATUS_OK);
    REQUIRE(description.local_cid == 201u);
    REQUIRE(description.peer_cid == 202u);
    REQUIRE(std::strcmp(description.remote_host, "127.0.0.1") == 0);
    REQUIRE(description.remote_port == 14001u);
    REQUIRE(utp_connection_get_statistic(&connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.pmtu == UTP_MTU_DEFAULT_BASE);
    REQUIRE(statistic.tx_bytes == 0u);
    REQUIRE(statistic.rtx_bytes == 0u);

    {
        const uint8_t                                    ping     = UTP_FRAME_TYPE_PING;
        const utp_packet_header_t                        header   = {202u, 201u, 1u, 1u, UTP_PACKET_TYPE_CTRL, 0u};
        std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 1u> incoming = {};

        REQUIRE(utp_proto_encode_header(incoming.data(), UTP_PACKET_HEADER_SIZE, &header) == UTP_INTERNAL_ERROR_OK);
        incoming[UTP_PACKET_HEADER_SIZE] = ping;
        REQUIRE(utp_connection_on_packet_received(&connection, incoming.data(), incoming.size(), &peer,
                                                  UINT64_C(900000)) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_connection_get_statistic(&connection, &statistic) == UTP_STATUS_OK);
        REQUIRE(statistic.rx_bytes == incoming.size());
        REQUIRE(utp_connection_ack_pending_count(&connection) == 1u);
        REQUIRE(utp_connection_on_packet_received(&connection, incoming.data(), incoming.size(), &peer,
                                                  UINT64_C(900001)) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_connection_get_statistic(&connection, &statistic) == UTP_STATUS_OK);
        REQUIRE(statistic.rx_bytes == incoming.size());
        REQUIRE(utp_connection_ack_pending_count(&connection) == 1u);
    }

    REQUIRE(utp_connection_get_stream(&connection, 0u) != nullptr);
    REQUIRE(utp_stream_write_internal(utp_connection_get_stream(&connection, 0u), &stream_data, sizeof(stream_data)) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, UINT64_C(1000000)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_get_statistic(&connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.tx_bytes > UTP_PACKET_HEADER_SIZE);
    REQUIRE(statistic.rtx_bytes == 0u);

    deadline = utp_connection_retransmission_deadline(&connection);
    REQUIRE(deadline != 0u);
    REQUIRE(utp_connection_on_retransmission_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, deadline + 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_get_statistic(&connection, &statistic) == UTP_STATUS_OK);
    REQUIRE(statistic.rtx_bytes > UTP_PACKET_HEADER_SIZE);
    REQUIRE(statistic.tx_bytes == UINT64_C(2) * statistic.rtx_bytes);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection description formats IPv6 peers", "[connection][query]")
{
    utp_address_t                peer        = {};
    utp_connection_t             connection  = {};
    utp_connection_description_t description = {};

    REQUIRE(utp_address_parse(&peer, "2001:db8::10", 14002u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 203u, 204u, &peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_get_description(&connection, &description) == UTP_STATUS_OK);
    REQUIRE(std::strcmp(description.remote_host, "2001:db8::10") == 0);
    REQUIRE(description.remote_port == 14002u);
    utp_connection_cleanup(&connection);
}

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
    REQUIRE(utp_connection_state(&passive) == UTP_CONNECTION_STATE_CLOSING);
    REQUIRE(passive.close_pending);
    REQUIRE_FALSE(passive.local_close_started);
    send_to_peer(&passive, &active, &passive_address, &active_address, 500u);
    REQUIRE(utp_connection_state(&active) == UTP_CONNECTION_STATE_DRAINING);
    REQUIRE(utp_connection_state(&passive) == UTP_CONNECTION_STATE_DRAINING);

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection accepts an identical retransmitted Handshake transport parameters", "[connection][handshake]")
{
    const utp_address_t                active_address  = loopback_address(10021u);
    const utp_address_t                passive_address = loopback_address(10022u);
    const utp_frame_transport_params_t params          = {
        UINT64_C(8388608),
        UINT64_C(2097152),
        UINT64_C(2097152),
        600000u,
        UTP_TRANSPORT_PARAMS_DEFAULT_FLAGS,
        800u,
        64u,
        32u,
        3u,
    };
    const auto        payload = handshake_negotiation_payload(params);
    utp_connection_t  active  = {};
    utp_connection_t  passive = {};
    utp_packet_out_t* packet;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 31u, 0u, &passive_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 32u, 31u, &active_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_INITIAL, payload.data(), UTP_FRAME_VERSION_SIZE,
                                        true) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_HANDSHAKE, payload.data(), payload.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    send_to_peer(&passive, &active, &passive_address, &active_address, 200u);
    REQUIRE(active.peer_transport_params_received);
    REQUIRE(active.peer_max_data == params.initial_max_data);
    REQUIRE(active.peer_ack_delay_exponent == params.ack_delay_exponent);

    REQUIRE(utp_connection_queue_packet(&passive, UTP_PACKET_TYPE_HANDSHAKE, payload.data(), payload.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    send_to_peer(&passive, &active, &passive_address, &active_address, 300u);
    REQUIRE(active.peer_transport_params_received);
    REQUIRE(active.peer_max_data == params.initial_max_data);
    REQUIRE(active.peer_ack_frequency_received);
    {
        utp_frame_transport_params_t changed_params = params;

        ++changed_params.initial_max_data;
        REQUIRE(utp_connection_apply_peer_transport_params(&active, &changed_params) == UTP_INTERNAL_ERROR_PROTOCOL);
    }

    utp_connection_cleanup(&passive);
    utp_connection_cleanup(&active);
}

TEST_CASE("connection close bypasses the ordinary send queue and packet pool", "[connection][close]")
{
    const utp_address_t peer       = loopback_address(10003u);
    const uint8_t       ping       = UTP_FRAME_TYPE_PING;
    utp_connection_t    connection = {};
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 33u, 44u, &peer, 1u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_queue_packet(&connection, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), false) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_scheduled_packet_count(&connection.send_control) == 1u);

    REQUIRE(utp_connection_queue_close(&connection, 42u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_state(&connection) == UTP_CONNECTION_STATE_CLOSING);
    REQUIRE(connection.close_pending);
    REQUIRE(utp_send_control_scheduled_packet_count(&connection.send_control) == 1u);

    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet == &connection.close_packet);
    REQUIRE(packet->raw_data == connection.close_packet_data);
    REQUIRE(packet->slice_count == 1u);
    REQUIRE(packet->slices[0].length == packet->data_size);
    REQUIRE((packet->po_flags & UTP_PO_SCHED) == 0u);
    REQUIRE((packet->local_flags & UTP_POL_NO_TRACK_ON_SEND) != 0u);
    REQUIRE(utp_send_control_scheduled_packet_count(&connection.send_control) == 0u);
    REQUIRE(utp_connection_next_packet_to_send(&connection) == packet);

    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(!connection.close_pending);
    REQUIRE(packet->raw_data == connection.close_packet_data);
    REQUIRE(utp_send_control_unacked_packet_count(&connection.send_control) == 0u);

    REQUIRE(utp_connection_queue_close(&connection, 99u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.close_error_code == 42u);
    REQUIRE(utp_connection_next_packet_to_send(&connection) == nullptr);

    utp_connection_cleanup(&connection);
}

TEST_CASE("a pending local close is reused as the peer close response", "[connection][close]")
{
    const utp_address_t          address_a    = loopback_address(10004u);
    const utp_address_t          address_b    = loopback_address(10005u);
    utp_connection_t             connection_a = {};
    utp_connection_t             connection_b = {};
    utp_packet_out_t*            packet;
    utp_packet_view_t            view  = {};
    utp_frame_connection_close_t close = {};
    const uint8_t*               frame;
    uint8_t                      frame_type;
    size_t                       frame_length;
    size_t                       offset = 0u;

    REQUIRE(utp_connection_init(&connection_a, UTP_CONNECTION_ROLE_ACTIVE, 41u, 42u, &address_b, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection_b, UTP_CONNECTION_ROLE_PASSIVE, 42u, 41u, &address_a, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection_a.state = UTP_CONNECTION_STATE_CONNECTED;
    connection_b.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection_a.send_control, true);
    utp_send_control_set_connected(&connection_b.send_control, true);

    REQUIRE(utp_connection_queue_close(&connection_a, 11u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_close(&connection_b, 22u) == UTP_INTERNAL_ERROR_OK);
    send_to_peer(&connection_b, &connection_a, &address_b, &address_a, 100u);
    REQUIRE(utp_connection_state(&connection_a) == UTP_CONNECTION_STATE_CLOSING);
    REQUIRE(connection_a.close_pending);
    REQUIRE(connection_a.local_close_started);
    REQUIRE(connection_a.peer_close_received);

    packet = utp_connection_next_packet_to_send(&connection_a);
    REQUIRE(packet == &connection_a.close_packet);
    REQUIRE(utp_packet_view_decode(&view, packet->raw_data, packet->data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_CONNECTION_CLOSE);
    REQUIRE(utp_frame_connection_close_decode(&close, frame, frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(close.error_code == 11u);
    REQUIRE(offset == view.payload_length);

    send_to_peer(&connection_a, &connection_b, &address_a, &address_b, 200u);
    REQUIRE(utp_connection_state(&connection_a) == UTP_CONNECTION_STATE_DRAINING);
    REQUIRE(utp_connection_state(&connection_b) == UTP_CONNECTION_STATE_DRAINING);

    utp_connection_cleanup(&connection_b);
    utp_connection_cleanup(&connection_a);
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

TEST_CASE("a delayed ACK for an original packet confirms its retransmission", "[connection][ack][retransmission]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE>                       version   = version_frame();
    const utp_address_t                                                     peer      = loopback_address(10010u);
    utp_ack_range_t                                                         ranges[]  = {{1u, 1u}};
    const utp_ack_info_t                                                    ack       = {1u, 0u, ranges, 1u, 1u};
    std::array<uint8_t, UTP_ACK_FRAME_HEADER_SIZE>                          ack_frame = {};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_ACK_FRAME_HEADER_SIZE> wire      = {};
    const utp_packet_header_t header = {99u, 55u, 1u, UTP_ACK_FRAME_HEADER_SIZE, UTP_PACKET_TYPE_CTRL, 0u};
    utp_connection_t          active = {};
    utp_packet_out_t*         packet;
    size_t                    ack_length = 0u;
    uint64_t                  deadline;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 55u, 99u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_INITIAL, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 1u);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    deadline = utp_connection_retransmission_deadline(&active);
    REQUIRE(deadline > 100u);
    REQUIRE(utp_connection_on_retransmission_timeout(&active, deadline) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 2u);
    REQUIRE(utp_connection_on_packet_sent(&active, packet, deadline + 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 1u);

    REQUIRE(utp_ack_encode(ack_frame.data(), ack_frame.size(), &ack, 0u, &ack_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(wire.data(), wire.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(wire.data() + UTP_PACKET_HEADER_SIZE, ack_frame.data(), ack_length);
    REQUIRE(utp_connection_on_packet_received(&active, wire.data(), UTP_PACKET_HEADER_SIZE + ack_length, &peer,
                                              deadline + 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&active.send_control) == 0u);
    REQUIRE(utp_send_control_lost_packet_count(&active.send_control) == 0u);
    REQUIRE(utp_send_control_srtt(&active.send_control) == 0u);
    REQUIRE(utp_connection_next_packet_to_send(&active) == nullptr);

    utp_connection_cleanup(&active);
}

TEST_CASE("connection rejects rendezvous packet types", "[connection][protocol]")
{
    const utp_address_t                     peer   = loopback_address(10011u);
    const utp_packet_header_t               header = {99u, 55u, 1u, 0u, UTP_PACKET_TYPE_RENDEZVOUS, 0u};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE> wire = {};
    utp_connection_t                        connection = {};

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 55u, 99u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(wire.data(), wire.size(), &header) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&connection, wire.data(), wire.size(), &peer, 100u) ==
            UTP_INTERNAL_ERROR_PROTOCOL);
    utp_connection_cleanup(&connection);
}

TEST_CASE("0-RTT request and response retransmissions allocate a new packet number",
          "[connection][retransmission][0rtt]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE> version         = version_frame();
    const utp_address_t                               passive_address = loopback_address(10010u);
    const utp_address_t                               active_address  = loopback_address(10011u);
    utp_connection_t                                  active          = {};
    utp_connection_t                                  passive         = {};
    std::array<uint8_t, 1280>                         first_wire      = {};
    utp_packet_out_t*                                 packet;
    size_t                                            first_length;
    uint64_t                                          deadline;

    REQUIRE(utp_connection_init(&active, UTP_CONNECTION_ROLE_ACTIVE, 56u, 0u, &passive_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&active, UTP_PACKET_TYPE_0RTT, version.data(), version.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 1u);
    first_length = packet->data_size;
    std::copy_n(packet->raw_data, first_length, first_wire.begin());
    REQUIRE(utp_connection_on_packet_sent(&active, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    deadline = utp_connection_retransmission_deadline(&active);
    REQUIRE(deadline > 100u);
    REQUIRE(utp_connection_on_retransmission_timeout(&active, deadline) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&active);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 2u);
    REQUIRE(packet->data_size == first_length);
    REQUIRE_FALSE(std::equal(first_wire.begin(), first_wire.begin() + first_length, packet->raw_data));
    utp_connection_cleanup(&active);

    REQUIRE(utp_connection_init(&passive, UTP_CONNECTION_ROLE_PASSIVE, 57u, 56u, &active_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_begin_zero_rtt_response(&passive) == UTP_INTERNAL_ERROR_OK);
    REQUIRE_FALSE(utp_connection_is_connected(&passive));
    passive.bbr_congestion.cwnd                       = 1u;
    passive.send_control.pacer.burst_tokens           = 0u;
    passive.send_control.pacer.next_scheduled_time_us = UINT64_C(1000000);
    REQUIRE(utp_connection_queue_zero_rtt_response(&passive, version.data(), version.size(), false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&passive, 200u);
    REQUIRE(packet != nullptr);
    first_length = packet->data_size;
    std::copy_n(packet->raw_data, first_length, first_wire.begin());
    REQUIRE(utp_connection_on_packet_sent(&passive, packet, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_is_connected(&passive));
    REQUIRE(utp_connection_retransmission_deadline(&passive) == 0u);
    REQUIRE(utp_connection_begin_zero_rtt_response(&passive) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_zero_rtt_response(&passive, version.data(), version.size(), false) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&passive);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 2u);
    REQUIRE(packet->data_size == first_length);
    REQUIRE_FALSE(std::equal(first_wire.begin(), first_wire.begin() + first_length, packet->raw_data));
    utp_connection_cleanup(&passive);
}

TEST_CASE("encrypted 0-RTT retransmission uses a new nonce and ciphertext", "[connection][retransmission][0rtt]")
{
    const std::array<uint8_t, UTP_FRAME_VERSION_SIZE>          version       = version_frame();
    const utp_address_t                                        peer          = loopback_address(10012u);
    std::array<uint8_t, UTP_CRYPTO_RESUMPTION_PSK_SIZE>        psk           = {};
    std::array<uint8_t, UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE>   attempt_nonce = {};
    std::array<uint8_t, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE> server_info   = {};
    std::array<uint8_t, 1280>                                  first_wire    = {};
    std::array<uint8_t, 1280>                                  second_wire   = {};
    utp_connection_t                                           connection    = {};
    utp_packet_out_t*                                          packet;
    size_t                                                     first_length  = 0u;
    size_t                                                     second_length = 0u;
    uint64_t                                                   deadline;

    psk[0]           = 1u;
    attempt_nonce[0] = 2u;
    server_info[0]   = 3u;
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 58u, 0u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_configure_zero_rtt_crypto(&connection, psk.data(), attempt_nonce.data(), server_info.data(),
                                                     UTP_CRYPTO_TYPE_AES_GCM_128) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_early_packet(&connection, UTP_PACKET_TYPE_0RTT, version.data(), version.size(), 0u,
                                              true) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 1u);
    REQUIRE(utp_connection_encode_packet_wire(&connection, packet, first_wire.data(), first_wire.size(),
                                              &first_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    deadline = utp_connection_retransmission_deadline(&connection);
    REQUIRE(utp_connection_on_retransmission_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 2u);
    REQUIRE(utp_connection_encode_packet_wire(&connection, packet, second_wire.data(), second_wire.size(),
                                              &second_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(second_length == first_length);
    REQUIRE_FALSE(std::equal(first_wire.begin(), first_wire.begin() + first_length, second_wire.begin()));
    utp_connection_cleanup(&connection);
}

TEST_CASE("encrypted 0-RTT promotes matching bidirectional 1-RTT keys", "[connection][crypto][0rtt]")
{
    const utp_address_t                                              client_address = loopback_address(10013u);
    const utp_address_t                                              server_address = loopback_address(10014u);
    const std::array<uint8_t, UTP_CRYPTO_RESUMPTION_PSK_SIZE>        psk            = {1u};
    const std::array<uint8_t, UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE>   attempt_nonce  = {2u};
    const std::array<uint8_t, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE> server_info    = {3u};
    const uint8_t                                                    ping           = UTP_FRAME_TYPE_PING;
    utp_connection_t                                                 client         = {};
    utp_connection_t                                                 server         = {};

    REQUIRE(utp_connection_init(&client, UTP_CONNECTION_ROLE_ACTIVE, 61u, 62u, &server_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&server, UTP_CONNECTION_ROLE_PASSIVE, 62u, 61u, &client_address, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_configure_zero_rtt_crypto(&client, psk.data(), attempt_nonce.data(), server_info.data(),
                                                     UTP_CRYPTO_TYPE_AES_GCM_128) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_configure_zero_rtt_crypto(&server, psk.data(), attempt_nonce.data(), server_info.data(),
                                                     UTP_CRYPTO_TYPE_AES_GCM_128) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_complete_zero_rtt_crypto(&client, server.crypto_key_pair.public_key) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_complete_zero_rtt_crypto(&server, client.crypto_key_pair.public_key) ==
            UTP_INTERNAL_ERROR_OK);

    client.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&client.send_control, true);
    REQUIRE(utp_connection_queue_packet(&client, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), false) ==
            UTP_INTERNAL_ERROR_OK);
    send_encrypted_to_peer(&client, &server, &client_address, 100u);

    REQUIRE(utp_connection_queue_packet(&server, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), false) ==
            UTP_INTERNAL_ERROR_OK);
    send_encrypted_to_peer(&server, &client, &server_address, 200u);

    utp_connection_cleanup(&server);
    utp_connection_cleanup(&client);
}

TEST_CASE("0-RTT retains overflow early data for the first 1-RTT stream frame", "[connection][0rtt][stream]")
{
    const utp_address_t             peer       = loopback_address(10015u);
    const std::array<uint8_t, 128u> data       = {};
    utp_connection_t                connection = {};
    utp_stream_t*                   stream;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 63u, 64u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_reserve_zero_rtt_stream(&connection, data.data(), data.size(), 40u, true) ==
            UTP_INTERNAL_ERROR_OK);
    stream = utp_connection_find_stream_internal(&connection, 0u);
    REQUIRE(stream != nullptr);
    REQUIRE(stream->send_buffer_offset == 40u);
    REQUIRE(stream->next_send_offset == 40u);
    REQUIRE(stream->send_buffer_length == data.size() - 40u);
    REQUIRE(stream->local_fin_queued);
    REQUIRE_FALSE(stream->local_fin_sent);
    REQUIRE(connection.stream_data_sent_total == 40u);
    utp_connection_cleanup(&connection);
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

TEST_CASE("encrypted connections reject plaintext Handshake packets with non-handshake frames", "[connection][crypto]")
{
    const utp_address_t                                                      peer       = loopback_address(10026u);
    utp_connection_t                                                         connection = {};
    utp_packet_out_t*                                                        packet;
    utp_frame_crypto_t                                                       crypto       = {};
    std::array<uint8_t, UTP_FRAME_CRYPTO_SIZE>                               crypto_frame = {};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_CRYPTO_SIZE + 1u> wire         = {};
    const utp_packet_header_t header = {11u, 77u, 1u, UTP_FRAME_CRYPTO_SIZE + 1u, UTP_PACKET_TYPE_HANDSHAKE, 0u};
    const uint8_t             ping   = UTP_FRAME_TYPE_PING;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 77u, 11u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state             = UTP_CONNECTION_STATE_CONNECTED;
    connection.crypto_type       = UTP_FRAME_CRYPTO_TYPE_AES_GCM_128;
    connection.crypto_configured = true;
    connection.crypto_ready      = true;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_queue_packet(&connection, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&connection.send_control) == 1u);

    crypto.crypto_type = connection.crypto_type;
    REQUIRE(utp_frame_crypto_encode(crypto_frame.data(), crypto_frame.size(), &crypto) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(wire.data(), wire.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(wire.data() + UTP_PACKET_HEADER_SIZE, crypto_frame.data(), crypto_frame.size());
    wire.back() = ping;
    REQUIRE(utp_connection_on_packet_received(&connection, wire.data(), wire.size(), &peer, 200u) ==
            UTP_INTERNAL_ERROR_AUTH);
    REQUIRE(utp_send_control_unacked_packet_count(&connection.send_control) == 1u);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection sends a pure ACK while cwnd or pacer blocks ordinary packets", "[connection][ack][congestion]")
{
    const uint8_t            ping       = UTP_FRAME_TYPE_PING;
    const utp_address_t      peer       = loopback_address(10023u);
    utp_connection_t         connection = {};
    utp_packet_header_t      header;
    std::array<uint8_t, 32u> wire = {};
    utp_packet_out_t*        packet;
    size_t                   wire_length = 0u;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 77u, 11u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);

    REQUIRE(utp_connection_queue_packet(&connection, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&connection, 100u);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);

    connection.bbr_congestion.cwnd = 1u;
    REQUIRE(utp_connection_queue_packet(&connection, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_next_packet_to_send_at(&connection, 200u) == nullptr);

    connection.bbr_congestion.cwnd                       = UINT64_MAX;
    connection.send_control.pacer.burst_tokens           = 0u;
    connection.send_control.pacer.next_scheduled_time_us = UINT64_C(1000000);
    REQUIRE(utp_connection_next_packet_to_send_at(&connection, 200u) == nullptr);
    REQUIRE(utp_send_control_pacing_deadline(&connection.send_control) == UINT64_C(1000000));

    header = {11u, 77u, 1u, 1u, UTP_PACKET_TYPE_CTRL, 0u};
    REQUIRE(utp_proto_encode_header(wire.data(), wire.size(), &header) == UTP_INTERNAL_ERROR_OK);
    wire[UTP_PACKET_HEADER_SIZE] = ping;
    wire_length                  = UTP_PACKET_HEADER_SIZE + 1u;
    REQUIRE(utp_connection_on_packet_received(&connection, wire.data(), wire_length, &peer, 200u) ==
            UTP_INTERNAL_ERROR_OK);

    packet = utp_connection_next_packet_to_send_at(&connection, 200u);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->frame_types == UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK));
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 200u) == UTP_INTERNAL_ERROR_OK);

    utp_connection_cleanup(&connection);
}

TEST_CASE("connection fixes its congestion algorithm before the first packet is sent", "[connection][congestion]")
{
    const uint8_t       ping       = UTP_FRAME_TYPE_PING;
    const utp_address_t peer       = loopback_address(10024u);
    utp_connection_t    connection = {};
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 78u, 12u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.congestion_algorithm == UTP_CONGESTION_BBR);
    REQUIRE(utp_connection_set_congestion_algorithm(&connection, UTP_CONGESTION_CUBIC, nullptr, nullptr, 1u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.send_control.congestion == utp_cubic_as_congestion(&connection.cubic_congestion));
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_queue_packet(&connection, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_set_congestion_algorithm(&connection, UTP_CONGESTION_BBR, nullptr, nullptr, 1u) ==
            UTP_INTERNAL_ERROR_STATE);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection validates a candidate address before migration", "[connection][path]")
{
    const utp_address_t                                                    expected_peer   = loopback_address(10005u);
    const utp_address_t                                                    candidate_peer  = loopback_address(10006u);
    const utp_address_t                                                    active_local    = loopback_address(11005u);
    const utp_address_t                                                    candidate_local = loopback_address(11006u);
    const std::array<uint8_t, 1u>                                          stream_data     = {'x'};
    std::array<uint8_t, UTP_FRAME_STREAM_HEADER_SIZE + stream_data.size()> payload         = {};
    utp_connection_t                                                       connection      = {};
    utp_packet_header_t header = {11u, 77u, 1u, (uint16_t)payload.size(), UTP_PACKET_TYPE_CTRL, 0u};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + payload.size()> packet         = {};
    std::array<uint8_t, UTP_FRAME_PATH_SIZE>                     response       = {};
    utp_frame_path_t                                             challenge_data = {};
    utp_packet_out_t*                                            challenge;
    utp_packet_view_t                                            view         = {};
    const uint8_t*                                               frame        = nullptr;
    uint8_t                                                      frame_type   = 0u;
    size_t                                                       frame_length = 0u;
    size_t                                                       offset       = 0u;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &expected_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.local = active_local;
    {
        const utp_frame_stream_t stream = {UTP_STREAM_FLAG_NONE, 0u, 0u, stream_data.data(),
                                           (uint16_t)stream_data.size()};

        REQUIRE(utp_frame_stream_encode(payload.data(), payload.size(), &stream) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet.data() + UTP_PACKET_HEADER_SIZE, payload.data(), payload.size());
    utp_connection_set_received_local(&connection, &candidate_local);
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &candidate_peer, 100u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_equal(&connection.peer, &expected_peer));
    REQUIRE(utp_address_equal(&connection.local, &active_local));
    REQUIRE(utp_address_equal(&connection.candidate_local, &candidate_local));
    REQUIRE(connection.path_state == UTP_CONNECTION_PATH_STATE_VALIDATING);
    REQUIRE(utp_connection_find_stream_internal(&connection, 0u) == nullptr);

    challenge = utp_connection_next_packet_to_send(&connection);
    REQUIRE(challenge != nullptr);
    REQUIRE(challenge->has_destination);
    REQUIRE(utp_address_equal(&challenge->destination, &candidate_peer));
    REQUIRE(utp_packet_view_decode(&view, challenge->raw_data, challenge->data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_PATH_CHALLENGE);
    REQUIRE(utp_frame_path_decode(&challenge_data, frame, frame_length, UTP_FRAME_TYPE_PATH_CHALLENGE) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_sent(&connection, challenge, 101u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_frame_path_encode(response.data(), response.size(), UTP_FRAME_TYPE_PATH_RESPONSE, &challenge_data) ==
            UTP_INTERNAL_ERROR_OK);
    header.packet_number                                                              = 2u;
    header.payload_length                                                             = (uint16_t)response.size();
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_FRAME_PATH_SIZE> response_packet = {};
    REQUIRE(utp_proto_encode_header(response_packet.data(), response_packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(response_packet.data() + UTP_PACKET_HEADER_SIZE, response.data(), response.size());
    utp_connection_set_received_local(&connection, &candidate_local);
    REQUIRE(utp_connection_on_packet_received(&connection, response_packet.data(), response_packet.size(),
                                              &candidate_peer, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_address_equal(&connection.peer, &candidate_peer));
    REQUIRE(utp_address_equal(&connection.local, &candidate_local));
    REQUIRE(connection.path_state == UTP_CONNECTION_PATH_STATE_VALIDATED);

    header.dcid = 78u;
    REQUIRE(utp_proto_encode_header(response_packet.data(), response_packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&connection, response_packet.data(), response_packet.size(),
                                              &candidate_peer, 300u) == UTP_INTERNAL_ERROR_PROTOCOL);
    utp_connection_cleanup(&connection);
}

TEST_CASE("candidate path cache retains early packets and releases them after validation timeout", "[connection][path]")
{
    const utp_address_t            active_peer    = loopback_address(10015u);
    const utp_address_t            candidate_peer = loopback_address(10016u);
    const std::array<uint8_t, 16u> first_data     = {'c', 'a', 'c', 'h', 'e', '-', 'f', 'i', 'r', 's', 't'};
    const std::array<uint8_t, 16u> second_data    = {'c', 'a', 'c', 'h', 'e', '-', 's', 'e', 'c', 'o', 'n', 'd'};
    std::array<uint8_t, UTP_FRAME_STREAM_HEADER_SIZE + first_data.size()>  first_payload  = {};
    std::array<uint8_t, UTP_FRAME_STREAM_HEADER_SIZE + second_data.size()> second_payload = {};
    utp_packet_in_pool_t                                                   packet_pool    = {};
    utp_packet_in_t*                                                       first_packet   = nullptr;
    utp_packet_in_t*                                                       second_packet  = nullptr;
    utp_connection_t                                                       connection     = {};
    utp_packet_out_t*                                                      challenge;
    utp_packet_header_t                                                    header;
    uint64_t                                                               deadline;
    uint8_t                                                                attempt;

    {
        const utp_frame_stream_t first_frame  = {UTP_STREAM_FLAG_NONE, 0u, 0u, first_data.data(),
                                                 (uint16_t)first_data.size()};
        const utp_frame_stream_t second_frame = {UTP_STREAM_FLAG_NONE, 0u, (uint64_t)first_data.size(),
                                                 second_data.data(), (uint16_t)second_data.size()};

        REQUIRE(utp_frame_stream_encode(first_payload.data(), first_payload.size(), &first_frame) ==
                UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_frame_stream_encode(second_payload.data(), second_payload.size(), &second_frame) ==
                UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_packet_in_pool_init(&packet_pool, nullptr, 2u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &first_packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &second_packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &active_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    first_packet->length = (uint16_t)(UTP_PACKET_HEADER_SIZE + first_payload.size());
    header               = {11u, 77u, 1u, (uint16_t)first_payload.size(), UTP_PACKET_TYPE_CTRL, 0u};
    REQUIRE(utp_proto_encode_header(first_packet->data, first_packet->length, &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(first_packet->data + UTP_PACKET_HEADER_SIZE, first_payload.data(), first_payload.size());
    utp_connection_set_path_validation_buffer_capacity(&connection, first_packet->length);
    REQUIRE(utp_connection_on_plaintext_packet_in_received(&connection, first_packet, first_packet->length,
                                                           &candidate_peer, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.candidate_packet_head != nullptr);
    REQUIRE(connection.candidate_packet_bytes == first_packet->length);
    REQUIRE(first_packet->ref_count == 2u);
    utp_packet_in_release(first_packet);
    REQUIRE(first_packet->in_use);
    REQUIRE(first_packet->ref_count == 1u);

    second_packet->length = (uint16_t)(UTP_PACKET_HEADER_SIZE + second_payload.size());
    header                = {11u, 77u, 2u, (uint16_t)second_payload.size(), UTP_PACKET_TYPE_CTRL, 0u};
    REQUIRE(utp_proto_encode_header(second_packet->data, second_packet->length, &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(second_packet->data + UTP_PACKET_HEADER_SIZE, second_payload.data(), second_payload.size());
    REQUIRE(utp_connection_on_plaintext_packet_in_received(&connection, second_packet, second_packet->length,
                                                           &candidate_peer, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.candidate_packet_bytes == first_packet->length);
    REQUIRE(second_packet->ref_count == 1u);
    utp_packet_in_release(second_packet);
    REQUIRE_FALSE(second_packet->in_use);

    for (attempt = 0u; attempt < 3u; ++attempt) {
        challenge = utp_connection_next_packet_to_send(&connection);
        REQUIRE(challenge != nullptr);
        REQUIRE(utp_connection_on_packet_sent(&connection, challenge, 201u + attempt) == UTP_INTERNAL_ERROR_OK);
        deadline = utp_connection_path_validation_deadline(&connection);
        REQUIRE(deadline != 0u);
        REQUIRE(utp_connection_on_path_validation_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(connection.path_state == UTP_CONNECTION_PATH_STATE_VALIDATED);
    REQUIRE(utp_address_equal(&connection.peer, &active_peer));
    REQUIRE(connection.candidate_packet_head == nullptr);
    REQUIRE(connection.candidate_packet_tail == nullptr);
    REQUIRE(connection.candidate_packet_bytes == 0u);
    REQUIRE_FALSE(first_packet->in_use);
    utp_connection_cleanup(&connection);
    utp_packet_in_pool_cleanup(&packet_pool);
}

TEST_CASE("candidate path cleanup returns cached PacketIn capacity to the pool", "[connection][path][resource]")
{
    const utp_address_t           active_peer    = loopback_address(10021u);
    const utp_address_t           candidate_peer = loopback_address(10022u);
    const std::array<uint8_t, 8u> stream_data    = {'p', 'o', 'o', 'l', '-', 'h', 'o', 'l'};
    std::array<uint8_t, UTP_FRAME_STREAM_HEADER_SIZE + stream_data.size()> payload     = {};
    utp_packet_in_pool_t                                                   packet_pool = {};
    utp_packet_in_t*                                                       packet      = nullptr;
    utp_packet_in_t*                                                       reused      = nullptr;
    utp_connection_t                                                       connection  = {};
    utp_packet_out_t*                                                      challenge;
    utp_packet_header_t                                                    header;
    uint64_t                                                               deadline;
    uint8_t                                                                attempt;

    {
        const utp_frame_stream_t frame = {UTP_STREAM_FLAG_NONE, 0u, 0u, stream_data.data(),
                                          (uint16_t)stream_data.size()};

        REQUIRE(utp_frame_stream_encode(payload.data(), payload.size(), &frame) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_packet_in_pool_init(&packet_pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &active_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);

    packet->length = (uint16_t)(UTP_PACKET_HEADER_SIZE + payload.size());
    header         = {11u, 77u, 1u, (uint16_t)payload.size(), UTP_PACKET_TYPE_CTRL, 0u};
    REQUIRE(utp_proto_encode_header(packet->data, packet->length, &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet->data + UTP_PACKET_HEADER_SIZE, payload.data(), payload.size());
    utp_connection_set_path_validation_buffer_capacity(&connection, packet->length);
    REQUIRE(utp_connection_on_plaintext_packet_in_received(&connection, packet, packet->length, &candidate_peer,
                                                           100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet->ref_count == 2u);
    utp_packet_in_release(packet);
    REQUIRE(packet->ref_count == 1u);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &reused) == UTP_INTERNAL_ERROR_LIMIT);
    REQUIRE(reused == nullptr);

    for (attempt = 0u; attempt < 3u; ++attempt) {
        challenge = utp_connection_next_packet_to_send(&connection);
        REQUIRE(challenge != nullptr);
        REQUIRE(utp_connection_on_packet_sent(&connection, challenge, 101u + attempt) == UTP_INTERNAL_ERROR_OK);
        deadline = utp_connection_path_validation_deadline(&connection);
        REQUIRE(deadline != 0u);
        REQUIRE(utp_connection_on_path_validation_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(connection.candidate_packet_head == nullptr);
    REQUIRE_FALSE(packet->in_use);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &reused) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(reused == packet);
    utp_packet_in_release(reused);
    utp_connection_cleanup(&connection);
    utp_packet_in_pool_cleanup(&packet_pool);
}

TEST_CASE("candidate path CONNECTION_CLOSE bypasses the cache and closes immediately", "[connection][path][close]")
{
    const utp_address_t                active_peer    = loopback_address(10017u);
    const utp_address_t                candidate_peer = loopback_address(10018u);
    const std::array<uint8_t, 6u>      reason         = {'c', 'l', 'o', 's', 'e', '!'};
    const utp_frame_connection_close_t close          = {UINT16_C(42), reason.data(), (uint16_t)reason.size()};
    std::array<uint8_t, UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE + reason.size()> payload     = {};
    utp_packet_in_pool_t                                                        packet_pool = {};
    utp_packet_in_t*                                                            packet      = nullptr;
    utp_connection_t                                                            connection  = {};
    utp_packet_out_t*                                                           response;
    utp_packet_header_t                                                         header;

    REQUIRE(utp_frame_connection_close_encode(payload.data(), payload.size(), &close) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&packet_pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &active_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    packet->length = (uint16_t)(UTP_PACKET_HEADER_SIZE + payload.size());
    header         = {11u, 77u, 1u, (uint16_t)payload.size(), UTP_PACKET_TYPE_CONNECTION_CLOSE, 0u};
    REQUIRE(utp_proto_encode_header(packet->data, packet->length, &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet->data + UTP_PACKET_HEADER_SIZE, payload.data(), payload.size());
    REQUIRE(utp_connection_on_plaintext_packet_in_received(&connection, packet, packet->length, &candidate_peer,
                                                           100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.peer_close_received);
    REQUIRE(connection.peer_close_error_code == close.error_code);
    REQUIRE(connection.candidate_packet_head == nullptr);
    REQUIRE(connection.candidate_packet_bytes == 0u);
    REQUIRE(utp_connection_state(&connection) == UTP_CONNECTION_STATE_CLOSING);
    REQUIRE(connection.close_pending);
    utp_packet_in_release(packet);
    REQUIRE_FALSE(packet->in_use);
    response = utp_connection_next_packet_to_send(&connection);
    REQUIRE(response == &connection.close_packet);
    REQUIRE(response->packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE);
    utp_connection_cleanup(&connection);
    utp_packet_in_pool_cleanup(&packet_pool);
}

TEST_CASE("candidate PATH_CHALLENGE bypasses the cache and queues its response", "[connection][path]")
{
    const utp_address_t    active_peer    = loopback_address(10019u);
    const utp_address_t    candidate_peer = loopback_address(10020u);
    const utp_frame_path_t challenge_data = {{UINT8_C(0x91), UINT8_C(0x82), UINT8_C(0x73), UINT8_C(0x64), UINT8_C(0x55),
                                              UINT8_C(0x46), UINT8_C(0x37), UINT8_C(0x28)}};
    std::array<uint8_t, UTP_FRAME_PATH_SIZE> payload     = {};
    utp_packet_in_pool_t                     packet_pool = {};
    utp_packet_in_t*                         packet      = nullptr;
    utp_connection_t                         connection  = {};
    utp_packet_out_t*                        outbound;
    utp_packet_header_t                      header;
    utp_packet_view_t                        view = {};
    const uint8_t*                           frame;
    uint8_t                                  frame_type;
    size_t                                   frame_length;
    size_t                                   offset        = 0u;
    utp_frame_path_t                         response_data = {};

    REQUIRE(utp_frame_path_encode(payload.data(), payload.size(), UTP_FRAME_TYPE_PATH_CHALLENGE, &challenge_data) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_init(&packet_pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&packet_pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &active_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state = UTP_CONNECTION_STATE_CONNECTED;
    utp_send_control_set_connected(&connection.send_control, true);
    packet->length = (uint16_t)(UTP_PACKET_HEADER_SIZE + payload.size());
    header         = {11u, 77u, 1u, (uint16_t)payload.size(), UTP_PACKET_TYPE_CTRL, 0u};
    REQUIRE(utp_proto_encode_header(packet->data, packet->length, &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet->data + UTP_PACKET_HEADER_SIZE, payload.data(), payload.size());
    REQUIRE(utp_connection_on_plaintext_packet_in_received(&connection, packet, packet->length, &candidate_peer,
                                                           100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.path_state == UTP_CONNECTION_PATH_STATE_VALIDATING);
    REQUIRE(connection.candidate_packet_head == nullptr);
    utp_packet_in_release(packet);
    REQUIRE_FALSE(packet->in_use);

    outbound = utp_connection_next_packet_to_send(&connection);
    REQUIRE(outbound != nullptr);
    REQUIRE(outbound->has_destination);
    REQUIRE(utp_address_equal(&outbound->destination, &candidate_peer));
    REQUIRE(utp_packet_view_decode(&view, outbound->raw_data, outbound->data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_PATH_CHALLENGE);
    REQUIRE(utp_connection_on_packet_sent(&connection, outbound, 101u) == UTP_INTERNAL_ERROR_OK);

    outbound = utp_connection_next_packet_to_send(&connection);
    REQUIRE(outbound != nullptr);
    REQUIRE(outbound->has_destination);
    REQUIRE(utp_address_equal(&outbound->destination, &candidate_peer));
    offset = 0u;
    REQUIRE(utp_packet_view_decode(&view, outbound->raw_data, outbound->data_size) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(frame_type == UTP_FRAME_TYPE_PATH_RESPONSE);
    REQUIRE(utp_frame_path_decode(&response_data, frame, frame_length, UTP_FRAME_TYPE_PATH_RESPONSE) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(std::memcmp(response_data.data, challenge_data.data, sizeof(response_data.data)) == 0);
    utp_connection_cleanup(&connection);
    utp_packet_in_pool_cleanup(&packet_pool);
}

TEST_CASE("connection retries candidate path validation three times then keeps the active path", "[connection][path]")
{
    const utp_address_t           active_peer    = loopback_address(10015u);
    const utp_address_t           candidate_peer = loopback_address(10016u);
    const std::array<uint8_t, 1u> payload        = {UTP_FRAME_TYPE_PING};
    utp_connection_t              connection     = {};
    utp_packet_header_t           header         = {11u, 77u, 1u, (uint16_t)payload.size(), UTP_PACKET_TYPE_CTRL, 0u};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + payload.size()> packet = {};
    uint64_t                                                     deadline;
    uint8_t                                                      attempt;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &active_peer, 2u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(packet.data() + UTP_PACKET_HEADER_SIZE, payload.data(), payload.size());
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &candidate_peer, 100u) ==
            UTP_INTERNAL_ERROR_OK);

    for (attempt = 0u; attempt < 3u; ++attempt) {
        utp_packet_out_t* challenge = utp_connection_next_packet_to_send(&connection);

        REQUIRE(challenge != nullptr);
        REQUIRE(challenge->has_destination);
        REQUIRE(utp_address_equal(&challenge->destination, &candidate_peer));
        REQUIRE(utp_connection_on_packet_sent(&connection, challenge, 101u + attempt) == UTP_INTERNAL_ERROR_OK);
        deadline = utp_connection_path_validation_deadline(&connection);
        REQUIRE(deadline != 0u);
        REQUIRE(utp_connection_on_path_validation_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(utp_connection_path_validation_deadline(&connection) == 0u);
    REQUIRE(connection.path_state == UTP_CONNECTION_PATH_STATE_VALIDATED);
    REQUIRE(utp_address_equal(&connection.peer, &active_peer));
    REQUIRE(utp_connection_next_packet_to_send(&connection) == nullptr);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection keepalive probes are ACK-eliciting and abort after missed probes", "[connection][keepalive]")
{
    const utp_address_t                              active_peer    = loopback_address(10017u);
    const utp_address_t                              candidate_peer = loopback_address(10018u);
    const uint8_t                                    ping           = UTP_FRAME_TYPE_PING;
    utp_connection_t                                 connection     = {};
    utp_packet_header_t                              header         = {11u, 77u, 1u, 1u, UTP_PACKET_TYPE_CTRL, 0u};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 1u> packet         = {};
    uint64_t                                         deadline;
    uint8_t                                          probe;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &active_peer, 8u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    packet[UTP_PACKET_HEADER_SIZE] = ping;
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &active_peer, 100u) ==
            UTP_INTERNAL_ERROR_OK);
    deadline = utp_connection_keepalive_deadline(&connection);
    REQUIRE(deadline == 100u + UTP_CONNECTION_KEEPALIVE_INTERVAL_US);

    header.packet_number = 2u;
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &candidate_peer, 200u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_keepalive_deadline(&connection) == deadline);
    {
        utp_packet_out_t* challenge = utp_connection_next_packet_to_send(&connection);

        REQUIRE(challenge != nullptr);
        REQUIRE((challenge->po_flags & UTP_PO_PATH_VALIDATION) != 0u);
        utp_connection_on_packet_abandoned(&connection, challenge);
        utp_packet_out_pool_release(&connection.packet_pool, challenge);
    }

    for (probe = 0u; probe < UTP_CONNECTION_KEEPALIVE_MAX_PROBES; ++probe) {
        utp_packet_out_t* outgoing;
        utp_packet_view_t view = {};
        const uint8_t*    frame;
        uint8_t           frame_type;
        size_t            frame_length;
        size_t            offset = 0u;

        REQUIRE(utp_connection_on_keepalive_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
        outgoing = utp_connection_next_packet_to_send(&connection);
        REQUIRE(outgoing != nullptr);
        REQUIRE(outgoing->packet_type == UTP_PACKET_TYPE_CTRL);
        REQUIRE(outgoing->frame_types == UTP_FRAME_BIT(UTP_FRAME_TYPE_PING));
        REQUIRE((outgoing->local_flags & UTP_POL_NO_TRACK_ON_SEND) == 0u);
        REQUIRE(utp_packet_view_decode(&view, outgoing->raw_data, outgoing->data_size) == UTP_INTERNAL_ERROR_OK);
        REQUIRE(utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length) ==
                UTP_INTERNAL_ERROR_OK);
        REQUIRE(frame_type == UTP_FRAME_TYPE_PING);
        REQUIRE(frame_length == 1u);
        REQUIRE(offset == view.payload_length);
        REQUIRE(utp_connection_on_packet_sent(&connection, outgoing, deadline) == UTP_INTERNAL_ERROR_OK);
        deadline = utp_connection_keepalive_deadline(&connection);
        REQUIRE(deadline != 0u);
    }
    REQUIRE(utp_connection_on_keepalive_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_TIMEOUT);
    REQUIRE(utp_connection_state(&connection) == UTP_CONNECTION_STATE_DRAINING);
    REQUIRE(utp_connection_close_deadline(&connection) == deadline);
    REQUIRE(utp_connection_keepalive_deadline(&connection) == 0u);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection keeps a peer idle timeout safety margin", "[connection][keepalive]")
{
    const utp_address_t                peer   = loopback_address(10023u);
    const uint8_t                      ping   = UTP_FRAME_TYPE_PING;
    const utp_frame_transport_params_t params = {
        0u, 0u, 0u, 40u, UTP_TRANSPORT_PARAMS_FLAG_MAX_IDLE_TIMEOUT, 0u, 0u, 0u, 0u,
    };
    utp_connection_t                                 connection = {};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 1u> packet     = {};
    const utp_packet_header_t                        header     = {11u, 77u, 1u, 1u, UTP_PACKET_TYPE_CTRL, 0u};

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_apply_peer_transport_params(&connection, &params) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(packet.data(), packet.size(), &header) == UTP_INTERNAL_ERROR_OK);
    packet[UTP_PACKET_HEADER_SIZE] = ping;
    REQUIRE(utp_connection_on_packet_received(&connection, packet.data(), packet.size(), &peer, 100u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_keepalive_deadline(&connection) == 1100u);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection sends a ladder MTU probe and immediately backs off on EMSGSIZE", "[connection][mtu]")
{
    const utp_address_t peer       = loopback_address(10019u);
    utp_connection_t    connection = {};
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &peer, 4u, UINT16_MAX) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_mtu_deadline(&connection, UINT64_C(1000000)) == UINT64_C(1000000));
    packet = utp_connection_next_packet_to_send_at(&connection, UINT64_C(1000000));
    REQUIRE(packet != nullptr);
    REQUIRE((packet->po_flags & UTP_PO_MTU_PROBE) != 0u);
    REQUIRE(packet->data_size == 1422u);
    REQUIRE(packet->raw_data[UTP_PACKET_HEADER_SIZE] == UTP_FRAME_TYPE_PING);
    REQUIRE(packet->raw_data[UTP_PACKET_HEADER_SIZE + 1u] == UTP_FRAME_TYPE_PADDING);

    utp_connection_on_packet_send_error(&connection, packet, UTP_INTERNAL_ERROR_OVERFLOW, UINT64_C(1000000));
    REQUIRE(!utp_mtu_discovery_has_in_flight_probe(&connection.mtu_discovery));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&connection.mtu_discovery) == 1425u);
    utp_connection_on_packet_abandoned(&connection, packet);
    utp_packet_out_pool_release(&connection.packet_pool, packet);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection promotes a successfully acknowledged ladder MTU probe", "[connection][mtu]")
{
    const utp_address_t                                                     peer        = loopback_address(10020u);
    utp_ack_range_t                                                         ranges[]    = {{1u, 1u}};
    const utp_ack_info_t                                                    ack         = {1u, 0u, ranges, 1u, 1u};
    std::array<uint8_t, UTP_ACK_FRAME_HEADER_SIZE>                          ack_payload = {};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + UTP_ACK_FRAME_HEADER_SIZE> wire        = {};
    utp_connection_t                                                        connection  = {};
    utp_packet_out_t*                                                       packet;
    utp_packet_header_t                                                     header;
    size_t                                                                  ack_length = 0u;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &peer, 4u, UINT16_MAX) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send_at(&connection, UINT64_C(1000000));
    REQUIRE(packet != nullptr);
    REQUIRE(packet->packet_number == 1u);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, UINT64_C(1000000)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_mtu_discovery_has_in_flight_probe(&connection.mtu_discovery));
    REQUIRE(utp_ack_encode(ack_payload.data(), ack_payload.size(), &ack, 0u, &ack_length) == UTP_INTERNAL_ERROR_OK);
    header = {11u, 77u, 1u, (uint16_t)ack_length, UTP_PACKET_TYPE_CTRL, 0u};
    REQUIRE(utp_proto_encode_header(wire.data(), wire.size(), &header) == UTP_INTERNAL_ERROR_OK);
    std::memcpy(wire.data() + UTP_PACKET_HEADER_SIZE, ack_payload.data(), ack_length);
    REQUIRE(utp_connection_on_packet_received(&connection, wire.data(), UTP_PACKET_HEADER_SIZE + ack_length, &peer,
                                              UINT64_C(2000000)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(!utp_mtu_discovery_has_in_flight_probe(&connection.mtu_discovery));
    REQUIRE(connection.mtu_discovery.search_low_mtu == 1450u);
    REQUIRE(utp_mtu_discovery_path_mtu(&connection.mtu_discovery) == 1400u);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&connection.mtu_discovery) == 1492u);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection replaces a timed out MTU probe with a newly built retry", "[connection][mtu]")
{
    const utp_address_t peer       = loopback_address(10021u);
    utp_connection_t    connection = {};
    utp_packet_out_t*   first;
    utp_packet_out_t*   retry;
    uint16_t            first_size;
    const uint64_t      sent_at  = UINT64_C(1000000);
    const uint64_t      deadline = UINT64_C(3000000);

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &peer, 4u, UINT16_MAX) ==
            UTP_INTERNAL_ERROR_OK);
    first = utp_connection_next_packet_to_send_at(&connection, sent_at);
    REQUIRE(first != nullptr);
    REQUIRE((first->po_flags & UTP_PO_MTU_PROBE) != 0u);
    REQUIRE(first->packet_number == 1u);
    REQUIRE(first->data_size == 1422u);
    first_size = first->data_size;
    REQUIRE(utp_connection_on_packet_sent(&connection, first, sent_at) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&connection.send_control) == 1u);
    REQUIRE(utp_connection_mtu_deadline(&connection, sent_at) == deadline);

    REQUIRE(utp_connection_on_mtu_timeout(&connection, deadline) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_control_unacked_packet_count(&connection.send_control) == 0u);
    REQUIRE(!utp_mtu_discovery_has_in_flight_probe(&connection.mtu_discovery));
    REQUIRE(connection.mtu_discovery.retry_pending);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&connection.mtu_discovery) == 1450u);

    retry = utp_connection_next_packet_to_send_at(&connection, deadline + 1000u);
    REQUIRE(retry != nullptr);
    REQUIRE((retry->po_flags & UTP_PO_MTU_PROBE) != 0u);
    REQUIRE(retry->packet_number == 2u);
    REQUIRE(retry->data_size == first_size);
    REQUIRE(utp_connection_on_packet_sent(&connection, retry, deadline + 1000u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_on_mtu_timeout(&connection, UINT64_C(5001000)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(!connection.mtu_discovery.retry_pending);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&connection.mtu_discovery) == 1425u);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection feeds repeated large stream losses into MTU black-hole detection", "[connection][mtu]")
{
    const utp_address_t        peer        = loopback_address(10022u);
    std::array<uint8_t, 1336u> stream_data = {};
    std::array<uint8_t, 1352u> payload     = {};
    const utp_frame_stream_t stream = {UTP_STREAM_FLAG_NONE, 0u, 0u, stream_data.data(), (uint16_t)stream_data.size()};
    utp_connection_t         connection = {};
    utp_packet_out_t*        packet;
    uint8_t                  attempt;

    REQUIRE(utp_frame_stream_encode(payload.data(), payload.size(), &stream) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &peer, 4u, UINT16_MAX) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_packet(&connection, UTP_PACKET_TYPE_CTRL, payload.data(), payload.size(), true) ==
            UTP_INTERNAL_ERROR_OK);
    packet = utp_connection_next_packet_to_send(&connection);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->data_size == 1372u);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, 100u) == UTP_INTERNAL_ERROR_OK);

    for (attempt = 0u; attempt < 3u; ++attempt) {
        REQUIRE(utp_connection_on_retransmission_timeout(&connection, UINT64_C(1000) + attempt) ==
                UTP_INTERNAL_ERROR_OK);
        if (attempt < 2u) {
            packet = utp_connection_next_packet_to_send(&connection);
            REQUIRE(packet != nullptr);
            REQUIRE(packet->data_size == 1372u);
            REQUIRE(utp_connection_on_packet_sent(&connection, packet, UINT64_C(1100) + attempt) ==
                    UTP_INTERNAL_ERROR_OK);
        }
    }
    REQUIRE(utp_mtu_discovery_path_mtu(&connection.mtu_discovery) == UTP_MTU_DEFAULT_MIN);
    REQUIRE(connection.mtu_discovery.probe_phase == UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&connection.mtu_discovery) == UTP_MTU_DEFAULT_BASE);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection applies configured local ACK parameters to outgoing ACK frames", "[connection][ack][config]")
{
    const utp_address_t                              peer       = loopback_address(10024u);
    const uint8_t                                    ping       = UTP_FRAME_TYPE_PING;
    const utp_packet_header_t                        header     = {11u, 77u, 1u, 1u, UTP_PACKET_TYPE_CTRL, 0u};
    std::array<uint8_t, UTP_PACKET_HEADER_SIZE + 1u> incoming   = {};
    utp_connection_t                                 connection = {};
    utp_frame_transport_params_t                     params;
    const utp_frame_ack_frequency_t                  frequency = {25u, 4u, 3u};
    utp_packet_out_t*                                ack_packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_PASSIVE, 77u, 11u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    params                    = connection.local_transport_params;
    params.ack_delay_exponent = 3u;
    REQUIRE(utp_connection_set_local_transport_config(&connection, &params, &frequency, true, 0u, 1500u, 3u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_proto_encode_header(incoming.data(), incoming.size(), &header) == UTP_INTERNAL_ERROR_OK);
    incoming[UTP_PACKET_HEADER_SIZE] = ping;
    REQUIRE(utp_connection_on_packet_received(&connection, incoming.data(), incoming.size(), &peer, 100u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_connection_queue_ack(&connection, 1124u) == UTP_INTERNAL_ERROR_OK);
    ack_packet = utp_connection_next_packet_to_send_at(&connection, 1124u);
    REQUIRE(ack_packet != nullptr);
    REQUIRE(ack_packet->raw_data[UTP_PACKET_HEADER_SIZE] == UTP_FRAME_TYPE_ACK);
    REQUIRE(ack_packet->raw_data[UTP_PACKET_HEADER_SIZE + 2u] == 0u);
    REQUIRE(ack_packet->raw_data[UTP_PACKET_HEADER_SIZE + 3u] == 128u);
    utp_connection_cleanup(&connection);
}

TEST_CASE("connection queues dynamic ACK frequency as reliable control", "[connection][ack][control]")
{
    const utp_address_t peer       = loopback_address(10025u);
    utp_connection_t    connection = {};
    utp_packet_out_t*   packet;

    REQUIRE(utp_connection_init(&connection, UTP_CONNECTION_ROLE_ACTIVE, 77u, 11u, &peer, 4u, 1280u) ==
            UTP_INTERNAL_ERROR_OK);
    connection.state                    = UTP_CONNECTION_STATE_CONNECTED;
    connection.ack_loss_window_start_us = UINT64_C(3000001);
    connection.ack_loss_count           = 2u;
    utp_send_control_set_connected(&connection.send_control, true);
    REQUIRE(utp_connection_next_packet_to_send_at(&connection, UINT64_C(3000001)) == nullptr);
    connection.ack_loss_window_start_us = UINT64_C(6000001);
    packet                              = utp_connection_next_packet_to_send_at(&connection, UINT64_C(6000001));
    REQUIRE(packet != nullptr);
    REQUIRE((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK_FREQUENCY)) != 0u);
    REQUIRE((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK)) == 0u);
    REQUIRE(utp_connection_on_packet_sent(&connection, packet, UINT64_C(6000001)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(connection.send_control.peer_max_ack_delay_us == 6000u);
    REQUIRE(connection.ack_profile_last_sent_us == UINT64_C(6000001));
    utp_connection_cleanup(&connection);
}
