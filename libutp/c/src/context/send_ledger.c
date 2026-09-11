#include "context/send_ledger.h"

#include <assert.h>
#include <limits.h>

#include "proto/proto.h"

static uint64_t utp_send_ledger_packet_size(const utp_packet_out_t* packet)
{
    assert(packet != NULL);
    return (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size : packet->data_size;
}

static bool utp_send_ledger_is_retransmittable(const utp_send_ledger_t* ledger, const utp_packet_out_t* packet)
{
    assert(ledger != NULL);
    assert(packet != NULL);
    return (packet->frame_types & ledger->retransmittable_frame_mask) != 0u;
}

utp_internal_error_t utp_send_ledger_validate_ack(const utp_ack_info_t* ack, uint64_t largest_sent_packet_number)
{
    if (ack == NULL || ack->ranges == NULL || ack->range_count == 0u || ack->range_count > UTP_ACK_MAX_RANGES ||
        ack->range_count > ack->range_capacity || ack->largest_acked == 0u ||
        ack->largest_acked > largest_sent_packet_number || largest_sent_packet_number > UTP_PACKET_NUMBER_MAX ||
        ack->ranges[0].high != ack->largest_acked) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    uint64_t previous_low = ack->ranges[0].low;
    for (size_t index = 0u; index < ack->range_count; ++index) {
        const utp_ack_range_t* range = &ack->ranges[index];

        if (range->low == 0u || range->low > range->high || range->high > largest_sent_packet_number ||
            (index != 0u && previous_low <= range->high)) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        previous_low = range->low;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_ledger_init(utp_send_ledger_t* ledger, size_t packet_limit,
                                          uint32_t retransmittable_frame_mask)
{
    if (ledger == NULL || packet_limit == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    TAILQ_INIT(&ledger->unacked_packets);
    ledger->bytes_in_flight                 = 0u;
    ledger->retransmittable_bytes_in_flight = 0u;
    ledger->packet_count                    = 0u;
    ledger->retransmittable_packet_count    = 0u;
    ledger->packet_limit                    = packet_limit;
    ledger->retransmittable_frame_mask      = retransmittable_frame_mask;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_send_ledger_cleanup(utp_send_ledger_t* ledger)
{
    utp_packet_out_t* packet;

    if (ledger == NULL) {
        return;
    }
    while ((packet = TAILQ_FIRST(&ledger->unacked_packets)) != NULL) {
        TAILQ_REMOVE(&ledger->unacked_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_UNACKED;
    }
    TAILQ_INIT(&ledger->unacked_packets);
    ledger->bytes_in_flight                 = 0u;
    ledger->retransmittable_bytes_in_flight = 0u;
    ledger->packet_count                    = 0u;
    ledger->retransmittable_packet_count    = 0u;
    ledger->packet_limit                    = 0u;
    ledger->retransmittable_frame_mask      = 0u;
}

utp_internal_error_t utp_send_ledger_track(utp_send_ledger_t* ledger, utp_packet_out_t* packet)
{
    if (ledger == NULL || packet == NULL || ledger->packet_limit == 0u || packet->packet_number == 0u ||
        packet->packet_number > UTP_PACKET_NUMBER_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & UTP_PO_UNACKED) != 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (ledger->packet_count >= ledger->packet_limit) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    uint64_t packet_size     = utp_send_ledger_packet_size(packet);
    bool     retransmittable = utp_send_ledger_is_retransmittable(ledger, packet);
    if (packet_size > UINT64_MAX - ledger->bytes_in_flight ||
        (retransmittable && packet_size > UINT64_MAX - ledger->retransmittable_bytes_in_flight)) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }

    TAILQ_INSERT_TAIL(&ledger->unacked_packets, packet, po_next);
    packet->po_flags        |= UTP_PO_UNACKED;
    ledger->bytes_in_flight += packet_size;
    ++ledger->packet_count;
    if (retransmittable) {
        ledger->retransmittable_bytes_in_flight += packet_size;
        ++ledger->retransmittable_packet_count;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_send_ledger_remove(utp_send_ledger_t* ledger, utp_packet_out_t* packet)
{
    if (ledger == NULL || packet == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & UTP_PO_UNACKED) == 0u) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    uint64_t packet_size     = utp_send_ledger_packet_size(packet);
    bool     retransmittable = utp_send_ledger_is_retransmittable(ledger, packet);
    if (ledger->packet_count == 0u || packet_size > ledger->bytes_in_flight ||
        (retransmittable &&
         (ledger->retransmittable_packet_count == 0u || packet_size > ledger->retransmittable_bytes_in_flight))) {
        return UTP_INTERNAL_ERROR_STATE;
    }

    TAILQ_REMOVE(&ledger->unacked_packets, packet, po_next);
    packet->po_flags        &= (uint16_t)~UTP_PO_UNACKED;
    ledger->bytes_in_flight -= packet_size;
    --ledger->packet_count;
    if (retransmittable) {
        ledger->retransmittable_bytes_in_flight -= packet_size;
        --ledger->retransmittable_packet_count;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t* utp_send_ledger_find(const utp_send_ledger_t* ledger, uint64_t packet_number)
{
    if (ledger == NULL || packet_number == 0u) {
        return NULL;
    }
    utp_packet_out_t* packet;
    TAILQ_FOREACH(packet, &ledger->unacked_packets, po_next)
    {
        if (packet->packet_number == packet_number) {
            return packet;
        }
    }
    return NULL;
}

size_t utp_send_ledger_packet_count(const utp_send_ledger_t* ledger)
{
    return ledger == NULL ? 0u : ledger->packet_count;
}

uint64_t utp_send_ledger_bytes_in_flight(const utp_send_ledger_t* ledger)
{
    return ledger == NULL ? 0u : ledger->bytes_in_flight;
}

size_t utp_send_ledger_retransmittable_packet_count(const utp_send_ledger_t* ledger)
{
    return ledger == NULL ? 0u : ledger->retransmittable_packet_count;
}

uint64_t utp_send_ledger_retransmittable_bytes_in_flight(const utp_send_ledger_t* ledger)
{
    return ledger == NULL ? 0u : ledger->retransmittable_bytes_in_flight;
}
