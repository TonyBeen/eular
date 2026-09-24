#define CATCH_CONFIG_MAIN

#include <cstring>

#include <catch2/catch.hpp>

extern "C" {
#include "context/send_ledger.h"
#include "proto/ack.h"
}

namespace {

utp_packet_out_t make_packet(uint64_t packet_number, uint16_t data_size)
{
    utp_packet_out_t packet = {};

    packet.packet_number = packet_number;
    packet.data_size     = data_size;
    return packet;
}

}  // namespace

TEST_CASE("send ledger tracks packet and retransmittable byte totals", "[send_ledger]")
{
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

TEST_CASE("send ledger rejects duplicate packets and preserves bounded state", "[send_ledger]")
{
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

TEST_CASE("send ledger validates packet number and counters before mutation", "[send_ledger]")
{
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

TEST_CASE("send ledger validates ACK ranges without changing tracked packets", "[send_ledger]")
{
    utp_ack_range_t      valid_ranges[]   = {{6u, 6u}, {2u, 2u}};
    utp_ack_range_t      invalid_ranges[] = {{2u, 2u}};
    const utp_ack_info_t valid            = {6u, 5u, valid_ranges, 2u, 2u};
    const utp_ack_info_t invalid          = {3u, 0u, invalid_ranges, 1u, 1u};

    REQUIRE(utp_send_ledger_validate_ack(&valid, 6u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_ledger_validate_ack(&invalid, 2u) == UTP_INTERNAL_ERROR_PROTOCOL);
}
