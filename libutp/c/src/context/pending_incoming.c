#include "context/pending_incoming.h"

#include <string.h>

#include "proto/wire.h"

#define UTP_PENDING_HANDSHAKE_BASE_DELAY_US UINT64_C(150000)
#define UTP_PENDING_HANDSHAKE_MAX_DELAY_US  UINT64_C(60000000)

static utp_internal_error_t utp_pending_incoming_find_handshake_done(const utp_packet_view_t *view,
                                                                     uint64_t *ack_packet_number, bool *found) {
    size_t offset = 0u;

    *found = false;
    while (offset < view->payload_length) {
        const uint8_t       *frame;
        uint8_t              frame_type;
        size_t               frame_length;
        utp_internal_error_t error = utp_packet_view_next_frame(view, &offset, &frame_type, &frame, &frame_length);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (frame_type == UTP_FRAME_TYPE_HANDSHAKE_DONE) {
            utp_frame_handshake_done_t done;

            error = utp_frame_handshake_done_decode(&done, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            *ack_packet_number = done.ack_handshake_packet_number;
            *found             = true;
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_init(utp_pending_incoming_t *pending, uint32_t local_cid, uint32_t peer_cid,
                                               const utp_address_t *peer, uint8_t *storage, size_t storage_capacity,
                                               size_t packet_limit) {
    if (pending == NULL || peer == NULL || storage == NULL || local_cid == 0u || peer_cid == 0u ||
        storage_capacity < UTP_PACKET_HEADER_SIZE + sizeof(uint16_t) || packet_limit == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pending->peer                                 = *peer;
    pending->storage                              = storage;
    pending->storage_capacity                     = storage_capacity;
    pending->storage_length                       = 0u;
    pending->packet_limit                         = packet_limit;
    pending->packet_count                         = 0u;
    pending->local_cid                            = local_cid;
    pending->peer_cid                             = peer_cid;
    pending->last_handshake_packet_number         = 0u;
    pending->handshake_retransmission_deadline_us = 0u;
    pending->handshake_retransmission_count       = 0u;
    pending->accepted                             = false;
    pending->handshake_sent                       = false;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_pending_incoming_reset(utp_pending_incoming_t *pending) {
    if (pending != NULL) {
        pending->storage                              = NULL;
        pending->storage_capacity                     = 0u;
        pending->storage_length                       = 0u;
        pending->packet_limit                         = 0u;
        pending->packet_count                         = 0u;
        pending->local_cid                            = 0u;
        pending->peer_cid                             = 0u;
        pending->last_handshake_packet_number         = 0u;
        pending->handshake_retransmission_deadline_us = 0u;
        pending->handshake_retransmission_count       = 0u;
        pending->accepted                             = false;
        pending->handshake_sent                       = false;
    }
}

utp_internal_error_t utp_pending_incoming_accept(utp_pending_incoming_t *pending) {
    if (pending == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pending->accepted = true;
    return UTP_INTERNAL_ERROR_OK;
}

static uint64_t utp_pending_incoming_next_handshake_delay(const utp_pending_incoming_t *pending) {
    uint64_t delay = UTP_PENDING_HANDSHAKE_BASE_DELAY_US;
    uint32_t exponent;

    exponent = pending->handshake_retransmission_count > 8u ? 8u : pending->handshake_retransmission_count;
    while (exponent-- != 0u) {
        if (delay > UTP_PENDING_HANDSHAKE_MAX_DELAY_US / 2u) {
            return UTP_PENDING_HANDSHAKE_MAX_DELAY_US;
        }
        delay *= 2u;
    }
    return delay;
}

utp_internal_error_t utp_pending_incoming_mark_handshake_sent(utp_pending_incoming_t *pending,
                                                              uint64_t handshake_packet_number, uint64_t now_us) {
    uint64_t delay_us;

    if (pending == NULL || !pending->accepted || handshake_packet_number == 0u || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    delay_us = utp_pending_incoming_next_handshake_delay(pending);
    if (delay_us > UINT64_MAX - now_us) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    pending->handshake_sent                       = true;
    pending->last_handshake_packet_number         = handshake_packet_number;
    pending->handshake_retransmission_deadline_us = now_us + delay_us;
    if (pending->handshake_retransmission_count != UINT32_MAX) {
        ++pending->handshake_retransmission_count;
    }
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_pending_incoming_handshake_deadline(const utp_pending_incoming_t *pending) {
    return pending == NULL || !pending->accepted || !pending->handshake_sent
               ? 0u
               : pending->handshake_retransmission_deadline_us;
}

utp_internal_error_t utp_pending_incoming_on_packet(utp_pending_incoming_t *pending, const uint8_t *packet,
                                                    size_t packet_length, const utp_address_t *peer,
                                                    utp_pending_incoming_result_t *result) {
    utp_packet_view_t    view;
    utp_internal_error_t error;
    uint64_t             ack_packet_number;
    bool                 has_handshake_done;
    size_t               required;
    utp_wire_writer_t    writer;

    if (pending == NULL || packet == NULL || peer == NULL || result == NULL || !pending->accepted ||
        !utp_address_equal(&pending->peer, peer)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + view.payload_length ||
        view.header.packet_number == 0u || view.header.dcid != pending->local_cid ||
        view.header.scid != pending->peer_cid) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_pending_incoming_find_handshake_done(&view, &ack_packet_number, &has_handshake_done);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (pending->handshake_sent && has_handshake_done && ack_packet_number == pending->last_handshake_packet_number) {
        *result = UTP_PENDING_INCOMING_PROMOTE;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (packet_length > UINT16_MAX || pending->packet_count >= pending->packet_limit ||
        packet_length > pending->storage_capacity - pending->storage_length ||
        sizeof(uint16_t) > pending->storage_capacity - pending->storage_length - packet_length) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    required = pending->storage_length + sizeof(uint16_t) + packet_length;
    error    = utp_wire_writer_init(&writer, pending->storage + pending->storage_length,
                                    pending->storage_capacity - pending->storage_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, (uint16_t)packet_length);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    memcpy(pending->storage + pending->storage_length + sizeof(uint16_t), packet, packet_length);
    pending->storage_length = required;
    ++pending->packet_count;
    *result = UTP_PENDING_INCOMING_BUFFERED;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_replay(const utp_pending_incoming_t  *pending,
                                                 utp_pending_incoming_replay_fn replay, void *user_data) {
    utp_wire_reader_t reader;
    size_t            offset = 0u;

    if (pending == NULL || replay == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    while (offset < pending->storage_length) {
        uint16_t             packet_length;
        utp_internal_error_t error;

        error = utp_wire_reader_init(&reader, pending->storage + offset, pending->storage_length - offset);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u16(&reader, &packet_length);
        }
        if (error != UTP_INTERNAL_ERROR_OK ||
            (size_t)packet_length > pending->storage_length - offset - sizeof(uint16_t)) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        error = replay(pending->storage + offset + sizeof(uint16_t), packet_length, user_data);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        offset += sizeof(uint16_t) + packet_length;
    }
    return UTP_INTERNAL_ERROR_OK;
}
