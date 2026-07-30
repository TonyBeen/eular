#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>
#include <cstring>

extern "C" {
#include "context/send_ledger.h"
#include "proto/ack.h"
}

namespace {

utp_packet_out_t make_packet(uint64_t packet_number, uint16_t data_size) {
    utp_packet_out_t packet = {};

    packet.packet_number = packet_number;
    packet.data_size     = data_size;
    return packet;
}

}  // namespace

TEST_CASE("send ledger tracks packet and retransmittable byte totals", "[send_ledger]") {
    utp_send_ledger_t ledger = {};
    utp_packet_out_t  plain  = make_packet(1u, 1200u);
    utp_packet_out_t  cipher = make_packet(2u, 1216u);

    plain.frame_types        = UINT32_C(0x01);
    cipher.po_flags          = UTP_PO_ENCRYPTED;
    cipher.encrypt_data_size = 1232u;

    REQUIRE(utp_send_ledger_init(&ledger, 4u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &plain) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &cipher) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_send_ledger_packet_count(&ledger) == 2u);
    REQUIRE(utp_send_ledger_bytes_in_flight(&ledger) == 2432u);
    REQUIRE(utp_send_ledger_retransmittable_packet_count(&ledger) == 1u);
    REQUIRE(utp_send_ledger_retransmittable_bytes_in_flight(&ledger) == 1200u);
    REQUIRE((plain.po_flags & UTP_PO_UNACKED) != 0u);
    REQUIRE((cipher.po_flags & UTP_PO_UNACKED) != 0u);

    REQUIRE(utp_send_ledger_remove(&ledger, &plain) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &plain) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_retransmittable_packet_count(&ledger) == 1u);
    REQUIRE(utp_send_ledger_retransmittable_bytes_in_flight(&ledger) == 1200u);

    utp_send_ledger_cleanup(&ledger);
    REQUIRE((plain.po_flags & UTP_PO_UNACKED) == 0u);
    REQUIRE((cipher.po_flags & UTP_PO_UNACKED) == 0u);
}

TEST_CASE("send ledger rejects duplicate packets and preserves bounded state", "[send_ledger]") {
    utp_send_ledger_t ledger = {};
    utp_packet_out_t  first  = make_packet(1u, 100u);
    utp_packet_out_t  second = make_packet(2u, 200u);

    REQUIRE(utp_send_ledger_init(&ledger, 1u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &first) == UTP_INTERNAL_ERROR_STATE);
    REQUIRE(utp_send_ledger_track(&ledger, &second) == UTP_INTERNAL_ERROR_LIMIT);
    REQUIRE(utp_send_ledger_packet_count(&ledger) == 1u);
    REQUIRE(utp_send_ledger_bytes_in_flight(&ledger) == 100u);
    REQUIRE((second.po_flags & UTP_PO_UNACKED) == 0u);

    REQUIRE(utp_send_ledger_remove(&ledger, &second) == UTP_INTERNAL_ERROR_NOT_FOUND);
    REQUIRE(utp_send_ledger_remove(&ledger, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_packet_count(&ledger) == 0u);
    REQUIRE(utp_send_ledger_bytes_in_flight(&ledger) == 0u);

    utp_send_ledger_cleanup(&ledger);
}

TEST_CASE("send ledger validates packet number and counters before mutation", "[send_ledger]") {
    utp_send_ledger_t ledger  = {};
    utp_packet_out_t  invalid = make_packet(0u, 100u);
    utp_packet_out_t  valid   = make_packet(1u, 100u);

    REQUIRE(utp_send_ledger_init(nullptr, 1u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_send_ledger_init(&ledger, 0u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_send_ledger_init(&ledger, 1u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &invalid) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_send_ledger_packet_count(&ledger) == 0u);
    REQUIRE(utp_send_ledger_track(&ledger, &valid) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_find(&ledger, 1u) == &valid);
    REQUIRE(utp_send_ledger_find(&ledger, 2u) == nullptr);

    utp_send_ledger_cleanup(&ledger);
}

TEST_CASE("send ledger acknowledges sparse packet ranges into a caller-owned queue", "[send_ledger]") {
    utp_send_ledger_t            ledger = {};
    utp_packet_out_t             first  = make_packet(2u, 100u);
    utp_packet_out_t             second = make_packet(4u, 200u);
    utp_packet_out_t             third  = make_packet(6u, 300u);
    struct utp_packet_out_tailq  acknowledged;
    utp_ack_range_t              ranges[] = {{6u, 6u}, {2u, 2u}};
    const utp_ack_info_t         ack      = {6u, 5u, ranges, 2u, 2u};
    utp_send_ledger_ack_result_t result   = {};
    const utp_packet_out_t      *packet;

    first.sent_time_us  = 100u;
    second.sent_time_us = 200u;
    third.sent_time_us  = 300u;
    TAILQ_INIT(&acknowledged);

    REQUIRE(utp_send_ledger_init(&ledger, 4u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &second) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &third) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_send_ledger_acknowledge(&ledger, &ack, 6u, &acknowledged, &result) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(result.acknowledged_packet_count == 2u);
    REQUIRE(result.acknowledged_bytes == 400u);
    REQUIRE(result.largest_acknowledged_sent_time_us == 300u);
    REQUIRE(utp_send_ledger_packet_count(&ledger) == 1u);
    REQUIRE(utp_send_ledger_find(&ledger, 4u) == &second);
    REQUIRE(TAILQ_FIRST(&acknowledged) == &first);
    packet = TAILQ_NEXT(TAILQ_FIRST(&acknowledged), po_next);
    REQUIRE(packet == &third);
    REQUIRE((first.po_flags & UTP_PO_UNACKED) == 0u);
    REQUIRE((third.po_flags & UTP_PO_UNACKED) == 0u);

    utp_send_ledger_cleanup(&ledger);
}

TEST_CASE("send ledger rejects invalid ACK before changing tracked packets", "[send_ledger]") {
    utp_send_ledger_t            ledger = {};
    utp_packet_out_t             packet = make_packet(2u, 100u);
    struct utp_packet_out_tailq  acknowledged;
    utp_ack_range_t              ranges[] = {{2u, 2u}};
    const utp_ack_info_t         ack      = {3u, 0u, ranges, 1u, 1u};
    utp_send_ledger_ack_result_t result   = {};

    TAILQ_INIT(&acknowledged);
    REQUIRE(utp_send_ledger_init(&ledger, 2u, UINT32_C(0x01)) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_track(&ledger, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_acknowledge(&ledger, &ack, 2u, &acknowledged, &result) == UTP_INTERNAL_ERROR_PROTOCOL);
    REQUIRE(utp_send_ledger_packet_count(&ledger) == 1u);
    REQUIRE(TAILQ_EMPTY(&acknowledged));
    REQUIRE((packet.po_flags & UTP_PO_UNACKED) != 0u);

    utp_send_ledger_cleanup(&ledger);
}
