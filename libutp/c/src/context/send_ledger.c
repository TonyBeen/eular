#include "context/send_ledger.h"

#include <limits.h>
#include <string.h>

#include "proto/proto.h"

static uint64_t utp_send_ledger_packet_size(const utp_packet_out_t *packet) {
    return (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size : packet->data_size;
}

static bool utp_send_ledger_is_retransmittable(const utp_send_ledger_t *ledger, const utp_packet_out_t *packet) {
    return (packet->frame_types & ledger->retransmittable_frame_mask) != 0u;
}

static utp_internal_error_t utp_send_ledger_validate_ack(const utp_ack_info_t *ack,
                                                         uint64_t              largest_sent_packet_number) {
    uint64_t previous_low;
    size_t   index;

    if (ack == NULL || ack->ranges == NULL || ack->range_count == 0u || ack->range_count > UTP_ACK_MAX_RANGES ||
        ack->range_count > ack->range_capacity || ack->largest_acked == 0u ||
        ack->largest_acked > largest_sent_packet_number || largest_sent_packet_number > UTP_PACKET_NUMBER_MAX ||
        ack->ranges[0].high != ack->largest_acked) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    previous_low = ack->ranges[0].low;
    for (index = 0u; index < ack->range_count; ++index) {
        const utp_ack_range_t *range = &ack->ranges[index];

        if (range->low == 0u || range->low > range->high || range->high > largest_sent_packet_number ||
            (index != 0u && previous_low <= range->high)) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        previous_low = range->low;
    }
    return UTP_INTERNAL_ERROR_OK;
}

static bool utp_send_ledger_ack_contains(const utp_ack_info_t *ack, uint64_t packet_number) {
    size_t index;

    for (index = 0u; index < ack->range_count; ++index) {
        const utp_ack_range_t *range = &ack->ranges[index];

        if (packet_number <= range->high && packet_number >= range->low) {
            return true;
        }
    }
    return false;
}

utp_internal_error_t utp_send_ledger_init(utp_send_ledger_t *ledger, size_t packet_limit,
                                          uint32_t retransmittable_frame_mask) {
    if (ledger == NULL || packet_limit == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(ledger, 0, sizeof(*ledger));
    TAILQ_INIT(&ledger->unacked_packets);
    ledger->packet_limit               = packet_limit;
    ledger->retransmittable_frame_mask = retransmittable_frame_mask;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_send_ledger_cleanup(utp_send_ledger_t *ledger) {
    utp_packet_out_t *packet;

    if (ledger == NULL) {
        return;
    }
    while ((packet = TAILQ_FIRST(&ledger->unacked_packets)) != NULL) {
        TAILQ_REMOVE(&ledger->unacked_packets, packet, po_next);
        packet->po_flags &= (uint16_t)~UTP_PO_UNACKED;
    }
    memset(ledger, 0, sizeof(*ledger));
}

utp_internal_error_t utp_send_ledger_track(utp_send_ledger_t *ledger, utp_packet_out_t *packet) {
    uint64_t packet_size;
    bool     retransmittable;

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
    packet_size     = utp_send_ledger_packet_size(packet);
    retransmittable = utp_send_ledger_is_retransmittable(ledger, packet);
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

utp_internal_error_t utp_send_ledger_remove(utp_send_ledger_t *ledger, utp_packet_out_t *packet) {
    uint64_t packet_size;
    bool     retransmittable;

    if (ledger == NULL || packet == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & UTP_PO_UNACKED) == 0u) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    packet_size     = utp_send_ledger_packet_size(packet);
    retransmittable = utp_send_ledger_is_retransmittable(ledger, packet);
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

utp_internal_error_t utp_send_ledger_acknowledge(utp_send_ledger_t *ledger, const utp_ack_info_t *ack,
                                                 uint64_t                      largest_sent_packet_number,
                                                 struct utp_packet_out_tailq  *acknowledged_packets,
                                                 utp_send_ledger_ack_result_t *result) {
    utp_packet_out_t    *packet;
    utp_packet_out_t    *next;
    utp_internal_error_t error;

    if (ledger == NULL || acknowledged_packets == NULL || result == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    error = utp_send_ledger_validate_ack(ack, largest_sent_packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    for (packet = TAILQ_FIRST(&ledger->unacked_packets); packet != NULL; packet = next) {
        uint64_t packet_size;

        next = TAILQ_NEXT(packet, po_next);
        if (!utp_send_ledger_ack_contains(ack, packet->packet_number)) {
            continue;
        }
        packet_size = utp_send_ledger_packet_size(packet);
        error       = utp_send_ledger_remove(ledger, packet);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        TAILQ_INSERT_TAIL(acknowledged_packets, packet, po_next);
        result->acknowledged_bytes += packet_size;
        ++result->acknowledged_packet_count;
        if (packet->packet_number > result->largest_acknowledged_packet_number) {
            result->largest_acknowledged_packet_number = packet->packet_number;
            result->largest_acknowledged_sent_time_us  = packet->sent_time_us;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_packet_out_t *utp_send_ledger_find(const utp_send_ledger_t *ledger, uint64_t packet_number) {
    utp_packet_out_t *packet;

    if (ledger == NULL || packet_number == 0u) {
        return NULL;
    }
    TAILQ_FOREACH(packet, &ledger->unacked_packets, po_next) {
        if (packet->packet_number == packet_number) {
            return packet;
        }
    }
    return NULL;
}

size_t utp_send_ledger_packet_count(const utp_send_ledger_t *ledger) {
    return ledger == NULL ? 0u : ledger->packet_count;
}

uint64_t utp_send_ledger_bytes_in_flight(const utp_send_ledger_t *ledger) {
    return ledger == NULL ? 0u : ledger->bytes_in_flight;
}

size_t utp_send_ledger_retransmittable_packet_count(const utp_send_ledger_t *ledger) {
    return ledger == NULL ? 0u : ledger->retransmittable_packet_count;
}

uint64_t utp_send_ledger_retransmittable_bytes_in_flight(const utp_send_ledger_t *ledger) {
    return ledger == NULL ? 0u : ledger->retransmittable_bytes_in_flight;
}
