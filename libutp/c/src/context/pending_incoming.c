#include "context/pending_incoming.h"

#include <string.h>

#include "proto/proto.h"
#include "proto/wire.h"

#define UTP_PENDING_HANDSHAKE_BASE_DELAY_US UINT64_C(150000)
#define UTP_PENDING_HANDSHAKE_MAX_DELAY_US  UINT64_C(60000000)

static utp_internal_error_t utp_pending_incoming_find_handshake_done(const utp_packet_view_t* view,
                                                                     uint64_t*                ack_packet_number,
                                                                     uint32_t* handshake_delay_us, bool* found,
                                                                     bool* delay_found)
{
    size_t offset = 0u;

    *found       = false;
    *delay_found = false;
    while (offset < view->payload_length) {
        const uint8_t*       frame;
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
        } else if (frame_type == UTP_FRAME_TYPE_HANDSHAKE_DELAY) {
            utp_frame_handshake_delay_t delay;

            if (*delay_found) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            error = utp_frame_handshake_delay_decode(&delay, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            *handshake_delay_us = delay.delay_time_us;
            *delay_found        = true;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_init(utp_pending_incoming_t* pending, uint32_t local_cid, uint32_t peer_cid,
                                               const utp_address_t* peer, uint8_t* storage, size_t storage_capacity,
                                               size_t packet_limit)
{
    if (pending == NULL || peer == NULL || storage == NULL || local_cid == 0u || peer_cid == 0u ||
        storage_capacity < UTP_PACKET_HEADER_SIZE + 2u * sizeof(uint16_t) || packet_limit == 0u) {
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
    pending->first_handshake_packet_number        = 0u;
    pending->last_handshake_packet_number         = 0u;
    pending->next_packet_number                   = 1u;
    pending->latest_initial_packet_number         = 0u;
    pending->latest_initial_received_us           = 0u;
    pending->last_handshake_sent_us               = 0u;
    pending->handshake_rtt_sample_us              = 0u;
    pending->handshake_retransmission_deadline_us = 0u;
    pending->handshake_base_delay_us              = UTP_PENDING_HANDSHAKE_BASE_DELAY_US;
    pending->handshake_retransmission_count       = 0u;
    pending->tx_aead                              = (utp_crypto_aead_t){0};
    pending->rx_aead                              = (utp_crypto_aead_t){0};
    pending->crypto_type                          = 0u;
    pending->handshake_max_retries                = UINT8_MAX;
    pending->crypto_configured                    = false;
    pending->crypto_ready                         = false;
    utp_crypto_key_pair_clear(&pending->crypto_key_pair);
    pending->accepted                = false;
    pending->handshake_sent          = false;
    pending->handshake_write_pending = false;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_pending_incoming_reset(utp_pending_incoming_t* pending)
{
    if (pending != NULL) {
        utp_crypto_aead_cleanup(&pending->tx_aead);
        utp_crypto_aead_cleanup(&pending->rx_aead);
        utp_crypto_key_pair_clear(&pending->crypto_key_pair);
        pending->storage                              = NULL;
        pending->storage_capacity                     = 0u;
        pending->storage_length                       = 0u;
        pending->packet_limit                         = 0u;
        pending->packet_count                         = 0u;
        pending->local_cid                            = 0u;
        pending->peer_cid                             = 0u;
        pending->first_handshake_packet_number        = 0u;
        pending->last_handshake_packet_number         = 0u;
        pending->next_packet_number                   = 0u;
        pending->latest_initial_packet_number         = 0u;
        pending->latest_initial_received_us           = 0u;
        pending->last_handshake_sent_us               = 0u;
        pending->handshake_rtt_sample_us              = 0u;
        pending->handshake_retransmission_deadline_us = 0u;
        pending->handshake_base_delay_us              = 0u;
        pending->handshake_retransmission_count       = 0u;
        pending->crypto_type                          = 0u;
        pending->handshake_max_retries                = 0u;
        pending->crypto_configured                    = false;
        pending->crypto_ready                         = false;
        pending->accepted                             = false;
        pending->handshake_sent                       = false;
        pending->handshake_write_pending              = false;
    }
}

utp_internal_error_t utp_pending_incoming_record_initial(utp_pending_incoming_t* pending, uint64_t packet_number,
                                                         uint64_t received_at_us)
{
    if (pending == NULL || packet_number == 0u || received_at_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (packet_number > pending->latest_initial_packet_number) {
        pending->latest_initial_packet_number = packet_number;
        pending->latest_initial_received_us   = received_at_us;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_configure_crypto(utp_pending_incoming_t*   pending,
                                                           const utp_frame_crypto_t* peer_crypto)
{
    if (pending == NULL || peer_crypto == NULL || pending->crypto_configured || pending->local_cid == 0u ||
        pending->peer_cid == 0u || peer_crypto->crypto_type > UTP_FRAME_CRYPTO_TYPE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_internal_error_t error = utp_crypto_key_pair_generate(&pending->crypto_key_pair);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_create_directional_aead(&pending->crypto_key_pair, peer_crypto->ephemeral_public_key,
                                                   pending->peer_cid, pending->local_cid, peer_crypto->crypto_type,
                                                   false, &pending->tx_aead, &pending->rx_aead);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_key_pair_clear(&pending->crypto_key_pair);
        utp_crypto_aead_cleanup(&pending->tx_aead);
        utp_crypto_aead_cleanup(&pending->rx_aead);
        return error;
    }
    pending->crypto_type       = peer_crypto->crypto_type;
    pending->crypto_configured = true;
    pending->crypto_ready      = true;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_encode_crypto(const utp_pending_incoming_t* pending, uint8_t* buffer,
                                                        size_t capacity)
{
    if (pending == NULL || buffer == NULL || !pending->crypto_configured) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_frame_crypto_t crypto;
    crypto.crypto_type = pending->crypto_type;
    memcpy(crypto.ephemeral_public_key, pending->crypto_key_pair.public_key, sizeof(crypto.ephemeral_public_key));
    return utp_frame_crypto_encode(buffer, capacity, &crypto);
}

utp_internal_error_t utp_pending_incoming_decrypt_packet(const utp_pending_incoming_t* pending, uint8_t* packet,
                                                         size_t* packet_length)
{
    utp_packet_header_t  header;
    size_t               plaintext_length;
    utp_internal_error_t error;

    if (pending == NULL || packet == NULL || packet_length == NULL || !pending->crypto_ready ||
        *packet_length < UTP_PACKET_HEADER_SIZE + UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_proto_decode_header(&header, packet, *packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || *packet_length != UTP_PACKET_HEADER_SIZE + header.payload_length ||
        header.type == UTP_PACKET_TYPE_INITIAL || header.type == UTP_PACKET_TYPE_HANDSHAKE ||
        header.payload_length < UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    error = utp_crypto_aead_open(&pending->rx_aead, header.packet_number, packet + UTP_PACKET_HEADER_SIZE,
                                 header.payload_length, packet, UTP_PACKET_HEADER_SIZE, packet + UTP_PACKET_HEADER_SIZE,
                                 header.payload_length, &plaintext_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (plaintext_length > UINT16_MAX) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    header.payload_length = (uint16_t)plaintext_length;
    error                 = utp_proto_encode_header(packet, *packet_length, &header);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *packet_length = UTP_PACKET_HEADER_SIZE + plaintext_length;
    }
    return error;
}

utp_internal_error_t utp_pending_incoming_accept(utp_pending_incoming_t* pending)
{
    if (pending == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pending->accepted = true;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_set_handshake_policy(utp_pending_incoming_t* pending, uint16_t timeout_ms,
                                                               uint8_t max_retries)
{
    if (pending == NULL || timeout_ms == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pending->handshake_base_delay_us = (uint64_t)timeout_ms * UINT64_C(1000);
    pending->handshake_max_retries   = max_retries;
    return UTP_INTERNAL_ERROR_OK;
}

static uint64_t utp_pending_incoming_next_handshake_delay(const utp_pending_incoming_t* pending)
{
    uint64_t delay = pending->handshake_base_delay_us > UTP_PENDING_HANDSHAKE_MAX_DELAY_US
                         ? UTP_PENDING_HANDSHAKE_MAX_DELAY_US
                         : pending->handshake_base_delay_us;
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

utp_internal_error_t utp_pending_incoming_mark_handshake_sent(utp_pending_incoming_t* pending,
                                                              uint64_t handshake_packet_number, uint64_t now_us)
{
    if (pending == NULL || !pending->accepted || handshake_packet_number == 0u || now_us == 0u ||
        (pending->handshake_sent && handshake_packet_number <= pending->last_handshake_packet_number)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    uint64_t delay_us = utp_pending_incoming_next_handshake_delay(pending);
    if (delay_us > UINT64_MAX - now_us) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    if (!pending->handshake_sent) {
        pending->first_handshake_packet_number = handshake_packet_number;
    }
    pending->handshake_sent                       = true;
    pending->last_handshake_packet_number         = handshake_packet_number;
    pending->last_handshake_sent_us               = now_us;
    pending->handshake_retransmission_deadline_us = now_us + delay_us;
    if (pending->handshake_retransmission_count != UINT32_MAX) {
        ++pending->handshake_retransmission_count;
    }
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_pending_incoming_handshake_deadline(const utp_pending_incoming_t* pending)
{
    return pending == NULL || !pending->accepted || !pending->handshake_sent
               ? 0u
               : pending->handshake_retransmission_deadline_us;
}

utp_internal_error_t utp_pending_incoming_on_packet(utp_pending_incoming_t* pending, const uint8_t* packet,
                                                    size_t packet_length, size_t wire_packet_length,
                                                    const utp_address_t* peer, uint64_t received_at_us,
                                                    utp_pending_incoming_result_t* result)
{
    if (pending == NULL || packet == NULL || peer == NULL || result == NULL || received_at_us == 0u ||
        !pending->accepted || wire_packet_length < packet_length || !utp_address_equal(&pending->peer, peer)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_packet_view_t    view;
    utp_internal_error_t error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + view.payload_length ||
        view.header.packet_number == 0u || view.header.dcid != pending->local_cid ||
        view.header.scid != pending->peer_cid) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    uint64_t ack_packet_number;
    uint32_t handshake_delay_us;
    bool     has_handshake_done;
    bool     has_handshake_delay;
    error = utp_pending_incoming_find_handshake_done(&view, &ack_packet_number, &handshake_delay_us,
                                                     &has_handshake_done, &has_handshake_delay);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (pending->handshake_sent && has_handshake_done && ack_packet_number >= pending->first_handshake_packet_number &&
        ack_packet_number <= pending->last_handshake_packet_number) {
        if (!has_handshake_delay) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (ack_packet_number == pending->last_handshake_packet_number && pending->last_handshake_sent_us != 0u &&
            received_at_us > pending->last_handshake_sent_us &&
            received_at_us - pending->last_handshake_sent_us > (uint64_t)handshake_delay_us) {
            pending->handshake_rtt_sample_us =
                received_at_us - pending->last_handshake_sent_us - (uint64_t)handshake_delay_us;
        }
        *result = UTP_PENDING_INCOMING_PROMOTE;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (packet_length > UINT16_MAX || wire_packet_length > UINT16_MAX ||
        pending->packet_count >= pending->packet_limit ||
        packet_length > pending->storage_capacity - pending->storage_length ||
        2u * sizeof(uint16_t) > pending->storage_capacity - pending->storage_length - packet_length) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    size_t            required = pending->storage_length + 2u * sizeof(uint16_t) + packet_length;
    utp_wire_writer_t writer;
    error = utp_wire_writer_init(&writer, pending->storage + pending->storage_length,
                                 pending->storage_capacity - pending->storage_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, (uint16_t)packet_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, (uint16_t)wire_packet_length);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    memcpy(pending->storage + pending->storage_length + 2u * sizeof(uint16_t), packet, packet_length);
    pending->storage_length = required;
    ++pending->packet_count;
    *result = UTP_PENDING_INCOMING_BUFFERED;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_pending_incoming_replay(const utp_pending_incoming_t*  pending,
                                                 utp_pending_incoming_replay_fn replay, void* user_data)
{
    utp_wire_reader_t reader;
    size_t            offset = 0u;

    if (pending == NULL || replay == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    while (offset < pending->storage_length) {
        uint16_t             packet_length;
        uint16_t             wire_packet_length;
        utp_internal_error_t error;

        error = utp_wire_reader_init(&reader, pending->storage + offset, pending->storage_length - offset);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u16(&reader, &packet_length);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_read_u16(&reader, &wire_packet_length);
        }
        if (error != UTP_INTERNAL_ERROR_OK ||
            (size_t)packet_length > pending->storage_length - offset - 2u * sizeof(uint16_t) ||
            wire_packet_length < packet_length) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        error = replay(pending->storage + offset + 2u * sizeof(uint16_t), packet_length, wire_packet_length, user_data);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        offset += 2u * sizeof(uint16_t) + packet_length;
    }
    return UTP_INTERNAL_ERROR_OK;
}
