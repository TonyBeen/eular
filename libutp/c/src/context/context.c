#include "context/context.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "proto/frame.h"
#include "proto/proto.h"
#include "socket/address.h"
#include "util/allocator.h"
#include "util/error.h"
#include "util/time.h"

typedef struct utp_context_replay {
    utp_connection_t    *connection;
    const utp_address_t *peer;
    uint64_t             now_us;
} utp_context_replay_t;

static uint64_t utp_context_now_us(void) {
    uint64_t now_us = utp_clock_now_us(NULL);

    return now_us == 0u ? 1u : now_us;
}

static void utp_context_endpoint_from_address(utp_endpoint_t *endpoint, const utp_address_t *address) {
    endpoint->family   = address->family;
    endpoint->port     = address->port;
    endpoint->scope_id = address->scope_id;
    memcpy(endpoint->address, address->address, sizeof(endpoint->address));
}

static bool utp_context_encryption_mode_is_valid(utp_encryption_mode_t encryption) {
    return encryption == UTP_ENCRYPTION_NONE || encryption == UTP_ENCRYPTION_AES_GCM_128 ||
           encryption == UTP_ENCRYPTION_AES_GCM_256;
}

static bool utp_context_cid_in_use(const utp_context_t *context, uint32_t cid) {
    size_t index;

    if (cid == 0u) {
        return true;
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (context->connections[index].used && context->connections[index].connection.local_cid == cid) {
            return true;
        }
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used && context->pending_incoming[index].pending.local_cid == cid) {
            return true;
        }
    }
    return false;
}

static utp_internal_error_t utp_context_alloc_cid(utp_context_t *context, uint32_t *out_cid) {
    uint32_t candidate;
    uint32_t attempts;

    if (context == NULL || out_cid == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    candidate = context->next_cid == 0u ? 1u : context->next_cid;
    for (attempts = 0u; attempts < UINT32_MAX; ++attempts) {
        if (!utp_context_cid_in_use(context, candidate)) {
            *out_cid = candidate;
            ++candidate;
            context->next_cid = candidate == 0u ? 1u : candidate;
            return UTP_INTERNAL_ERROR_OK;
        }
        ++candidate;
        if (candidate == 0u) {
            candidate = 1u;
        }
    }
    return UTP_INTERNAL_ERROR_LIMIT;
}

static utp_context_connection_slot_t *utp_context_find_connection_slot(utp_context_t *context, uint32_t local_cid) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (context->connections[index].used && context->connections[index].connection.local_cid == local_cid) {
            return &context->connections[index];
        }
    }
    return NULL;
}

static utp_context_connection_slot_t *utp_context_find_connection_by_peer(utp_context_t       *context,
                                                                          const utp_address_t *peer) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (context->connections[index].used && utp_address_equal(&context->connections[index].connection.peer, peer)) {
            return &context->connections[index];
        }
    }
    return NULL;
}

static utp_context_connection_slot_t *utp_context_alloc_connection_slot(utp_context_t *context) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (!context->connections[index].used) {
            context->connections[index].connection.local_cid = 0u;
            context->connections[index].connection.peer_cid  = 0u;
            context->connections[index].used                 = true;
            context->connections[index].connected_reported   = false;
            context->connections[index].closed_reported      = false;
            return &context->connections[index];
        }
    }
    return NULL;
}

static void utp_context_release_connection_slot(utp_context_connection_slot_t *slot) {
    if (slot != NULL && slot->used) {
        if (slot->connection.local_cid != 0u) {
            utp_connection_cleanup(&slot->connection);
        }
        slot->connected_reported = false;
        slot->closed_reported    = false;
        slot->used               = false;
    }
}

static utp_context_pending_slot_t *utp_context_find_pending_slot(utp_context_t *context, uint32_t local_cid) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used && context->pending_incoming[index].pending.local_cid == local_cid) {
            return &context->pending_incoming[index];
        }
    }
    return NULL;
}

static utp_context_pending_slot_t *utp_context_find_pending_by_peer(utp_context_t *context, uint32_t peer_cid,
                                                                    const utp_address_t *peer) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used && context->pending_incoming[index].pending.peer_cid == peer_cid &&
            utp_address_equal(&context->pending_incoming[index].pending.peer, peer)) {
            return &context->pending_incoming[index];
        }
    }
    return NULL;
}

static utp_context_pending_slot_t *utp_context_alloc_pending_slot(utp_context_t *context) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (!context->pending_incoming[index].used) {
            context->pending_incoming[index].used   = true;
            context->pending_incoming[index].queued = false;
            return &context->pending_incoming[index];
        }
    }
    return NULL;
}

static void utp_context_release_pending_slot(utp_context_pending_slot_t *slot) {
    if (slot != NULL && slot->used) {
        utp_pending_incoming_reset(&slot->pending);
        slot->queued = false;
        slot->used   = false;
    }
}

static utp_internal_error_t utp_context_send_raw(utp_context_t *context, const utp_address_t *peer,
                                                 const uint8_t *packet, size_t packet_length) {
    size_t sent_length = 0u;

    return utp_udp_socket_send_to(&context->udp_socket, packet, packet_length, peer, &sent_length);
}

static utp_internal_error_t utp_context_resolve_packet_slice(const utp_packet_out_t       *packet,
                                                             const utp_packet_out_slice_t *slice,
                                                             utp_udp_send_slice_t         *out_slice) {
    if (packet == NULL || slice == NULL || out_slice == NULL || slice->length == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    out_slice->length = slice->length;
    if (slice->source == UTP_PACKET_OUT_SLICE_RAW_OFFSET) {
        if (packet->raw_data == NULL || slice->offset > packet->alloc_size ||
            slice->length > packet->alloc_size - slice->offset) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        out_slice->data = packet->raw_data + slice->offset;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (slice->source == UTP_PACKET_OUT_SLICE_EXTERNAL) {
        if (slice->data == NULL) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
        out_slice->data = slice->data;
        return UTP_INTERNAL_ERROR_OK;
    }
    return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
}

static utp_internal_error_t utp_context_send_packet(utp_context_t *context, const utp_address_t *peer,
                                                    const utp_packet_out_t *packet) {
    utp_udp_send_slice_t slices[UTP_PACKET_OUT_MAX_SLICES];
    size_t               sent_length  = 0u;
    size_t               total_length = 0u;
    uint8_t              index;

    if (packet == NULL || packet->raw_data == NULL || packet->data_size == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (packet->slice_count == 0u) {
        return utp_context_send_raw(context, peer, packet->raw_data, packet->data_size);
    }
    if (packet->slice_count > UTP_PACKET_OUT_MAX_SLICES) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < packet->slice_count; ++index) {
        utp_internal_error_t error = utp_context_resolve_packet_slice(packet, &packet->slices[index], &slices[index]);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (slices[index].length > SIZE_MAX - total_length) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        total_length += slices[index].length;
    }
    if (total_length != packet->data_size) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    return utp_udp_socket_send_to_slices(&context->udp_socket, slices, packet->slice_count, peer, &sent_length);
}

static utp_internal_error_t utp_context_flush_connection(utp_context_t *context, utp_connection_t *connection) {
    for (;;) {
        utp_packet_out_t    *packet = utp_connection_next_packet_to_send(connection);
        utp_internal_error_t error;

        if (packet == NULL) {
            return UTP_INTERNAL_ERROR_OK;
        }
        error = utp_context_send_packet(context, &connection->peer, packet);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_on_packet_sent(connection, packet, utp_context_now_us());
        } else {
            utp_packet_out_pool_release(&connection->packet_pool, packet);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
}

static utp_internal_error_t utp_context_encode_version_frame(uint8_t *buffer, size_t capacity, size_t *out_length) {
    const utp_frame_version_t version = {UTP_PROTOCOL_VERSION};
    utp_internal_error_t      error;

    if (out_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *out_length = 0u;
    error       = utp_frame_version_encode(buffer, capacity, &version);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = UTP_FRAME_VERSION_SIZE;
    }
    return error;
}

static utp_internal_error_t utp_context_send_pending_packet(utp_context_t *context, utp_pending_incoming_t *pending,
                                                            uint8_t packet_type, const uint8_t *payload,
                                                            size_t payload_length, uint64_t packet_number) {
    uint8_t                   packet[UTP_PACKET_MTU_FLOOR];
    const utp_packet_header_t header = {pending->local_cid,       pending->peer_cid, packet_number,
                                        (uint16_t)payload_length, packet_type,       0u};
    utp_internal_error_t      error;

    if (payload_length > sizeof(packet) - UTP_PACKET_HEADER_SIZE || payload_length > UINT16_MAX) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    error = utp_proto_encode_header(packet, sizeof(packet), &header);
    if (error == UTP_INTERNAL_ERROR_OK && payload_length != 0u) {
        memcpy(packet + UTP_PACKET_HEADER_SIZE, payload, payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_send_raw(context, &pending->peer, packet, UTP_PACKET_HEADER_SIZE + payload_length);
    }
    return error;
}

static utp_internal_error_t utp_context_send_pending_handshake(utp_context_t *context, utp_pending_incoming_t *pending,
                                                               uint64_t *out_packet_number) {
    uint8_t              payload[UTP_FRAME_VERSION_SIZE];
    size_t               payload_length;
    uint64_t             packet_number;
    utp_internal_error_t error;

    if (out_packet_number == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_context_encode_version_frame(payload, sizeof(payload), &payload_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    packet_number = pending->last_handshake_packet_number + 1u;
    if (packet_number == 0u) {
        packet_number = 1u;
    }
    error = utp_context_send_pending_packet(context, pending, UTP_PACKET_TYPE_HANDSHAKE, payload, payload_length,
                                            packet_number);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_packet_number = packet_number;
    }
    return error;
}

static void utp_context_send_pending_close(utp_context_t *context, utp_pending_incoming_t *pending,
                                           uint16_t error_code) {
    uint8_t                            payload[UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE];
    const utp_frame_connection_close_t close = {error_code, NULL, 0u};
    uint64_t                           packet_number;

    packet_number = pending->last_handshake_packet_number + 1u;
    if (packet_number == 0u) {
        packet_number = 1u;
    }
    if (utp_frame_connection_close_encode(payload, sizeof(payload), &close) == UTP_INTERNAL_ERROR_OK) {
        (void)utp_context_send_pending_packet(context, pending, UTP_PACKET_TYPE_CONNECTION_CLOSE, payload,
                                              sizeof(payload), packet_number);
    }
}

static void utp_context_report_connected(utp_context_t *context, utp_context_connection_slot_t *slot) {
    if (!slot->connected_reported && utp_connection_is_connected(&slot->connection)) {
        slot->connected_reported = true;
        if (context->on_connected != NULL) {
            context->on_connected(&slot->connection, context->on_connected_user_data);
        }
    }
}

static void utp_context_report_closed(utp_context_t *context, utp_context_connection_slot_t *slot) {
    if (!slot->closed_reported && utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_CLOSING) {
        slot->closed_reported = true;
        if (context->on_connection_closed != NULL) {
            context->on_connection_closed(&slot->connection, context->on_connection_closed_user_data);
        }
    }
}

static void utp_context_report_connect_error(utp_context_t *context, utp_status_t status, const char *message,
                                             const utp_address_t *peer, const utp_connect_options_t *options,
                                             utp_connect_attempt_type_t type) {
    utp_connect_attempt_info_t attempt;

    if (context->on_connect_error == NULL) {
        return;
    }
    memset(&attempt, 0, sizeof(attempt));
    if (peer != NULL) {
        utp_context_endpoint_from_address(&attempt.remote, peer);
    }
    if (options != NULL) {
        attempt.timeout_ms = options->timeout_ms == 0u ? 3000u : options->timeout_ms;
        attempt.retries    = options->retries;
        attempt.encryption = options->encryption;
    }
    attempt.type = type;
    context->on_connect_error(status, message == NULL ? utp_status_string(status) : message, &attempt,
                              context->on_connect_error_user_data);
}

static utp_internal_error_t utp_context_send_handshake_done(utp_context_t *context, utp_connection_t *connection) {
    uint8_t                          payload[UTP_FRAME_HANDSHAKE_DONE_SIZE];
    const utp_frame_handshake_done_t done = {connection->peer_handshake_packet_number};
    utp_internal_error_t             error;

    error = utp_frame_handshake_done_encode(payload, sizeof(payload), &done);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), false);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_flush_connection(context, connection);
    }
    return error;
}

static utp_internal_error_t utp_context_queue_ack_if_due(utp_connection_t *connection, uint64_t now_us) {
    const uint64_t deadline = utp_connection_ack_deadline(connection);

    if (utp_connection_ack_pending_count(connection) == 0u || (deadline != 0u && deadline > now_us)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    return utp_connection_queue_ack(connection, now_us);
}

static void utp_context_timer_callback(uint32_t events, void *user_data);

static void utp_context_take_deadline(uint64_t *deadline, uint64_t candidate) {
    if (candidate != 0u && (*deadline == 0u || candidate < *deadline)) {
        *deadline = candidate;
    }
}

static uint64_t utp_context_next_deadline(const utp_context_t *context, uint64_t now_us) {
    uint64_t deadline = 0u;
    size_t   index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        const utp_connection_t *connection = &context->connections[index].connection;

        if (!context->connections[index].used) {
            continue;
        }
        if (utp_connection_ack_pending_count(connection) != 0u) {
            const uint64_t ack_deadline = utp_connection_ack_deadline(connection);

            utp_context_take_deadline(&deadline, ack_deadline == 0u ? now_us : ack_deadline);
        }
        utp_context_take_deadline(&deadline, utp_connection_retransmission_deadline(connection));
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used) {
            utp_context_take_deadline(
                &deadline, utp_pending_incoming_handshake_deadline(&context->pending_incoming[index].pending));
        }
    }
    return deadline;
}

static utp_internal_error_t utp_context_refresh_timer(utp_context_t *context, uint64_t now_us) {
    uint64_t deadline;
    uint64_t delay_us;

    if (context == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    deadline = utp_context_next_deadline(context, now_us);
    if (deadline == 0u) {
        utp_event_remove(&context->timer_event);
        return UTP_INTERNAL_ERROR_OK;
    }
    delay_us = deadline <= now_us ? 0u : deadline - now_us;
    if (context->timer_event.active) {
        return utp_event_reset_timer(&context->timer_event, delay_us);
    }
    return utp_event_add_timer(&context->event_loop, &context->timer_event, delay_us, false, utp_context_timer_callback,
                               context);
}

static utp_internal_error_t utp_context_replay_pending_packet(const uint8_t *packet, size_t packet_length,
                                                              void *user_data) {
    utp_context_replay_t *replay = user_data;

    return utp_connection_on_packet_received(replay->connection, packet, packet_length, replay->peer, replay->now_us);
}

static utp_internal_error_t utp_context_promote_pending(utp_context_t              *context,
                                                        utp_context_pending_slot_t *pending_slot, const uint8_t *packet,
                                                        size_t packet_length, const utp_address_t *peer) {
    utp_context_connection_slot_t *slot;
    utp_context_replay_t           replay;
    uint64_t                       now_us;
    utp_internal_error_t           error;

    slot = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_PASSIVE, pending_slot->pending.local_cid,
                                pending_slot->pending.peer_cid, peer, UTP_CONTEXT_PACKET_LIMIT, UTP_PACKET_MTU_FLOOR);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(slot);
        return error;
    }
    now_us = utp_context_now_us();
    error  = utp_connection_on_packet_received(&slot->connection, packet, packet_length, peer, now_us);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_queue_ack_if_due(&slot->connection, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        replay.connection = &slot->connection;
        replay.peer       = peer;
        replay.now_us     = now_us;
        error = utp_pending_incoming_replay(&pending_slot->pending, utp_context_replay_pending_packet, &replay);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(slot);
        return error;
    }
    utp_context_release_pending_slot(pending_slot);
    utp_context_report_connected(context, slot);
    return utp_context_flush_connection(context, &slot->connection);
}

static utp_internal_error_t utp_context_on_connection_packet(utp_context_t                 *context,
                                                             utp_context_connection_slot_t *slot,
                                                             const utp_packet_view_t *view, const uint8_t *packet,
                                                             size_t packet_length, const utp_address_t *peer) {
    utp_internal_error_t error;
    uint64_t             now_us;

    now_us = utp_context_now_us();
    error  = utp_connection_on_packet_received(&slot->connection, packet, packet_length, peer, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE && view->header.type == UTP_PACKET_TYPE_HANDSHAKE) {
        error = utp_context_send_handshake_done(context, &slot->connection);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    error = utp_context_queue_ack_if_due(&slot->connection, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_context_report_connected(context, slot);
    utp_context_report_closed(context, slot);
    return utp_context_flush_connection(context, &slot->connection);
}

static utp_internal_error_t utp_context_on_pending_packet(utp_context_t *context, utp_context_pending_slot_t *slot,
                                                          const uint8_t *packet, size_t packet_length,
                                                          const utp_address_t *peer) {
    utp_pending_incoming_result_t result;
    utp_internal_error_t          error;

    if (!slot->pending.accepted) {
        return UTP_INTERNAL_ERROR_OK;
    }
    error = utp_pending_incoming_on_packet(&slot->pending, packet, packet_length, peer, &result);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (result == UTP_PENDING_INCOMING_PROMOTE) {
        error = utp_context_promote_pending(context, slot, packet, packet_length, peer);
    }
    return error;
}

static utp_internal_error_t utp_context_on_initial_packet(utp_context_t *context, const utp_packet_view_t *view,
                                                          const utp_address_t *peer) {
    utp_context_pending_slot_t *slot;
    utp_new_connection_info_t   info;
    uint32_t                    local_cid;
    bool                        accepted;
    utp_internal_error_t        error;

    if (view->header.scid == 0u || view->header.dcid != 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    slot = utp_context_find_pending_by_peer(context, view->header.scid, peer);
    if (slot != NULL) {
        if (slot->pending.handshake_sent) {
            uint64_t packet_number = 0u;
            uint64_t now_us        = utp_context_now_us();

            error = utp_context_send_pending_handshake(context, &slot->pending, &packet_number);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_pending_incoming_mark_handshake_sent(&slot->pending, packet_number, now_us);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_context_refresh_timer(context, now_us);
            }
            return error;
        }
        return UTP_INTERNAL_ERROR_OK;
    }
    slot = utp_context_alloc_pending_slot(context);
    if (slot == NULL) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    error = utp_context_alloc_cid(context, &local_cid);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_pending_incoming_init(&slot->pending, local_cid, view->header.scid, peer, slot->storage,
                                          sizeof(slot->storage), UTP_CONTEXT_PENDING_PACKET_LIMIT);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_pending_slot(slot);
        return error;
    }
    slot->queued = true;
    memset(&info, 0, sizeof(info));
    utp_context_endpoint_from_address(&info.remote, peer);
    info.local_cid  = slot->pending.local_cid;
    info.peer_cid   = slot->pending.peer_cid;
    info.encryption = UTP_ENCRYPTION_NONE;
    accepted =
        context->on_new_connection == NULL || context->on_new_connection(&info, context->on_new_connection_user_data);
    if (!accepted) {
        utp_context_send_pending_close(context, &slot->pending, (uint16_t)(-UTP_STATUS_CANCELLED));
        utp_context_release_pending_slot(slot);
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_context_dispatch_packet(utp_context_t *context, const uint8_t *packet,
                                                        size_t packet_length, const utp_address_t *peer) {
    utp_packet_view_t              view;
    utp_context_connection_slot_t *connection_slot;
    utp_context_pending_slot_t    *pending_slot;
    utp_internal_error_t           error;

    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + view.payload_length ||
        view.header.packet_number == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    connection_slot = utp_context_find_connection_slot(context, view.header.dcid);
    if (connection_slot != NULL) {
        return utp_context_on_connection_packet(context, connection_slot, &view, packet, packet_length, peer);
    }
    pending_slot = utp_context_find_pending_slot(context, view.header.dcid);
    if (pending_slot != NULL) {
        return utp_context_on_pending_packet(context, pending_slot, packet, packet_length, peer);
    }
    if (view.header.dcid == 0u && view.header.type == UTP_PACKET_TYPE_INITIAL) {
        return utp_context_on_initial_packet(context, &view, peer);
    }
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_context_on_udp_readable(uint32_t events, void *user_data) {
    utp_context_t *context = user_data;

    if ((events & UTP_EVENT_READABLE) == 0u || context == NULL) {
        return;
    }
    for (;;) {
        size_t               received_length = 0u;
        utp_address_t        peer;
        utp_internal_error_t error = utp_udp_socket_recv_from(
            &context->udp_socket, context->udp_read_buffer, sizeof(context->udp_read_buffer), &received_length, &peer);

        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            return;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_dispatch_packet(context, context->udp_read_buffer, received_length, &peer);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_refresh_timer(context, utp_context_now_us());
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, error, "udp packet handling failed");
            return;
        }
    }
}

static utp_internal_error_t utp_context_process_connection_timers(utp_context_t *context, uint64_t now_us) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        utp_context_connection_slot_t *slot = &context->connections[index];
        uint64_t                       deadline;
        utp_internal_error_t           error;

        if (!slot->used) {
            continue;
        }
        error = utp_context_queue_ack_if_due(&slot->connection, now_us);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_flush_connection(context, &slot->connection);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        deadline = utp_connection_retransmission_deadline(&slot->connection);
        if (deadline != 0u && deadline <= now_us) {
            error = utp_connection_on_retransmission_timeout(&slot->connection, now_us);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_context_flush_connection(context, &slot->connection);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_connection_ensure_retransmission_deadline(&slot->connection, now_us);
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_context_process_pending_timers(utp_context_t *context, uint64_t now_us) {
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        utp_context_pending_slot_t *slot = &context->pending_incoming[index];
        uint64_t                    deadline;

        if (!slot->used) {
            continue;
        }
        deadline = utp_pending_incoming_handshake_deadline(&slot->pending);
        if (deadline != 0u && deadline <= now_us) {
            uint64_t             packet_number = 0u;
            utp_internal_error_t error;

            error = utp_context_send_pending_handshake(context, &slot->pending, &packet_number);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_pending_incoming_mark_handshake_sent(&slot->pending, packet_number, now_us);
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_context_timer_callback(uint32_t events, void *user_data) {
    utp_context_t       *context = user_data;
    uint64_t             now_us;
    utp_internal_error_t error;

    if (context == NULL || (events & UTP_EVENT_TIMEOUT) == 0u) {
        return;
    }
    now_us = utp_context_now_us();
    error  = utp_context_process_connection_timers(context, now_us);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_process_pending_timers(context, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_refresh_timer(context, utp_context_now_us());
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context timer handling failed");
    }
}

utp_status_t utp_context_create(const utp_context_options_t *options, utp_context_t **out_context) {
    utp_context_t       *context;
    utp_internal_error_t error;
    char                 fragment[32];
    int32_t              fragment_length;

    if (out_context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    *out_context = NULL;
    if (options == NULL || options->event_base == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    context = utp_allocator_alloc(NULL, sizeof(*context));
    if (context == NULL) {
        return UTP_STATUS_NOMEM;
    }
    for (uint32_t index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        context->connections[index].used = false;
    }
    for (uint32_t index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        context->pending_incoming[index].used   = false;
        context->pending_incoming[index].queued = false;
    }
    utp_event_init(&context->udp_event);
    utp_event_init(&context->timer_event);
    utp_udp_socket_init(&context->udp_socket);
    context->on_connected                   = NULL;
    context->on_connected_user_data         = NULL;
    context->on_connect_error               = NULL;
    context->on_connect_error_user_data     = NULL;
    context->on_new_connection              = NULL;
    context->on_new_connection_user_data    = NULL;
    context->on_connection_closed           = NULL;
    context->on_connection_closed_user_data = NULL;
    context->next_cid                       = (uint32_t)options->context_id;
    context->next_cid                       = context->next_cid == 0u ? 1u : context->next_cid;
    context->logger.sink                    = options->log_sink;
    fragment_length = snprintf(fragment, sizeof(fragment), "context %" PRIu64, options->context_id);
    if (fragment_length < 0 || (size_t)fragment_length >= sizeof(fragment)) {
        utp_allocator_free(NULL, context);
        return UTP_STATUS_OVERFLOW;
    }
    error = utp_log_tag_init(&context->tag, fragment, (size_t)fragment_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_event_loop_init(&context->event_loop, options->event_base, &context->logger, &context->tag);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context initialization failed");
        utp_allocator_free(NULL, context);
        return utp_internal_error_to_status(error);
    }
    *out_context = context;
    return UTP_STATUS_OK;
}

void utp_context_destroy(utp_context_t *context) {
    if (context != NULL) {
        size_t index;

        utp_event_remove(&context->timer_event);
        utp_event_remove(&context->udp_event);
        utp_udp_socket_close(&context->udp_socket);
        for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
            utp_context_release_connection_slot(&context->connections[index]);
        }
        for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
            utp_context_release_pending_slot(&context->pending_incoming[index]);
        }
        utp_event_loop_close(&context->event_loop);
        utp_allocator_free(NULL, context);
    }
}

utp_status_t utp_context_bind(utp_context_t *context, const char *address, uint16_t port, const char *ifname,
                              uint16_t *out_port) {
    utp_address_t        requested;
    utp_address_t        local;
    utp_internal_error_t error;

    if (out_port != NULL) {
        *out_port = 0u;
    }
    if (context == NULL || address == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_OPEN;
    }
    error = utp_address_parse(&requested, address, port);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_udp_socket_open(&context->udp_socket, requested.family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_udp_socket_bind(&context->udp_socket, &requested, ifname, &local);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_event_add_udp(&context->event_loop, &context->udp_event, &context->udp_socket, UTP_EVENT_READABLE,
                                  true, utp_context_on_udp_readable, context);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_event_remove(&context->udp_event);
        utp_udp_socket_close(&context->udp_socket);
        return utp_internal_error_to_status(error);
    }
    if (out_port != NULL) {
        *out_port = local.port;
    }
    return UTP_STATUS_OK;
}

void utp_context_set_on_connected(utp_context_t *context, utp_on_connected_fn callback, void *user_data) {
    if (context != NULL) {
        context->on_connected           = callback;
        context->on_connected_user_data = user_data;
    }
}

void utp_context_set_on_connect_error(utp_context_t *context, utp_on_connect_error_fn callback, void *user_data) {
    if (context != NULL) {
        context->on_connect_error           = callback;
        context->on_connect_error_user_data = user_data;
    }
}

void utp_context_set_on_new_connection(utp_context_t *context, utp_on_new_connection_fn callback, void *user_data) {
    if (context != NULL) {
        context->on_new_connection           = callback;
        context->on_new_connection_user_data = user_data;
    }
}

void utp_context_set_on_connection_closed(utp_context_t *context, utp_on_connection_closed_fn callback,
                                          void *user_data) {
    if (context != NULL) {
        context->on_connection_closed           = callback;
        context->on_connection_closed_user_data = user_data;
    }
}

utp_status_t utp_context_connect(utp_context_t *context, const utp_connect_options_t *options) {
    utp_address_t                  peer;
    utp_context_connection_slot_t *slot;
    utp_context_connection_slot_t *existing;
    uint8_t                        payload[UTP_FRAME_VERSION_SIZE];
    size_t                         payload_length;
    uint32_t                       local_cid;
    utp_internal_error_t           error;
    utp_status_t                   status;

    if (context == NULL || options == NULL || options->address == NULL || options->port == 0u ||
        !utp_context_encryption_mode_is_valid(options->encryption)) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (options->encryption != UTP_ENCRYPTION_NONE) {
        return UTP_STATUS_UNSUPPORTED;
    }
    if (!utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_NOT_BOUND;
    }
    error = utp_address_parse(&peer, options->address, options->port);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    existing = utp_context_find_connection_by_peer(context, &peer);
    if (existing != NULL) {
        return utp_connection_is_connected(&existing->connection) ? UTP_STATUS_SOCKET_CONNECTED
                                                                  : UTP_STATUS_IN_PROGRESS;
    }
    slot = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        return UTP_STATUS_LIMIT;
    }
    error = utp_context_alloc_cid(context, &local_cid);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_ACTIVE, local_cid, 0u, &peer,
                                    UTP_CONTEXT_PACKET_LIMIT, UTP_PACKET_MTU_FLOOR);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_encode_version_frame(payload, sizeof(payload), &payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_INITIAL, payload, payload_length, true);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_flush_connection(context, &slot->connection);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_refresh_timer(context, utp_context_now_us());
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        status = utp_internal_error_to_status(error);
        utp_context_report_connect_error(context, status, "active connect failed", &peer, options,
                                         UTP_CONNECT_ATTEMPT_NORMAL);
        utp_context_release_connection_slot(slot);
        return status;
    }
    return UTP_STATUS_OK;
}

utp_status_t utp_context_accept(utp_context_t *context) {
    size_t               index;
    utp_internal_error_t error;
    uint64_t             packet_number;
    uint64_t             now_us;

    if (context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (!utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_NOT_BOUND;
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        utp_context_pending_slot_t *slot = &context->pending_incoming[index];

        if (!slot->used || !slot->queued) {
            continue;
        }
        slot->queued = false;
        error        = utp_pending_incoming_accept(&slot->pending);
        now_us       = utp_context_now_us();
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_send_pending_handshake(context, &slot->pending, &packet_number);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_pending_incoming_mark_handshake_sent(&slot->pending, packet_number, now_us);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_refresh_timer(context, now_us);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_context_send_pending_close(context, &slot->pending, (uint16_t)(-UTP_STATUS_IO));
            utp_context_report_connect_error(context, utp_internal_error_to_status(error), "passive accept failed",
                                             &slot->pending.peer, NULL, UTP_CONNECT_ATTEMPT_PASSIVE);
            utp_context_release_pending_slot(slot);
            return utp_internal_error_to_status(error);
        }
        return UTP_STATUS_OK;
    }
    return UTP_STATUS_WOULD_BLOCK;
}
