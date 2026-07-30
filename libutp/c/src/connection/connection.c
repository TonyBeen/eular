#include "connection/connection.h"

#include <string.h>

#define UTP_CONNECTION_RETRANSMITTABLE_FRAMES                                      \
    ((UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX) - 1u) &                                    \
     ~(UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING) | \
       UTP_FRAME_BIT(UTP_FRAME_TYPE_PING)))
#define UTP_CONNECTION_ACK_ELICITING_THRESHOLD 2u
#define UTP_CONNECTION_ACK_REORDER_THRESHOLD   1u
#define UTP_CONNECTION_MAX_ACK_DELAY_MS        25u

static bool utp_connection_packet_type_is_valid(uint8_t type) {
    return type >= UTP_PACKET_TYPE_INITIAL && type <= UTP_PACKET_TYPE_CONNECT;
}

static bool utp_connection_packet_is_ack_eliciting(const utp_packet_view_t *view) {
    return view->frame_types != 0u &&
           (view->frame_types & ~(UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING))) != 0u;
}

static utp_internal_error_t utp_connection_find_handshake_done(const utp_packet_view_t    *view,
                                                               utp_frame_handshake_done_t *done, bool *found) {
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
            error = utp_frame_handshake_done_decode(done, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            *found = true;
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_encode_header(utp_connection_t *connection, utp_packet_out_t *packet,
                                                         uint8_t packet_type) {
    const utp_packet_header_t header = {
        connection->local_cid, connection->peer_cid,
        packet->packet_number, (uint16_t)(packet->data_size - UTP_PACKET_HEADER_SIZE),
        packet_type,           0u,
    };

    return utp_proto_encode_header(packet->raw_data, packet->alloc_size, &header);
}

static void utp_connection_release_queue(utp_connection_t *connection, struct utp_packet_out_tailq *packets) {
    utp_packet_out_t *packet;

    while ((packet = TAILQ_FIRST(packets)) != NULL) {
        TAILQ_REMOVE(packets, packet, po_next);
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
}

utp_internal_error_t utp_connection_init(utp_connection_t *connection, utp_connection_role_t role, uint32_t local_cid,
                                         uint32_t peer_cid, const utp_address_t *peer, size_t packet_limit,
                                         uint16_t packet_capacity) {
    const utp_packet_out_bucket_config_t bucket = {packet_capacity, packet_limit};
    utp_internal_error_t                 error;

    if (connection == NULL || peer == NULL ||
        (role != UTP_CONNECTION_ROLE_ACTIVE && role != UTP_CONNECTION_ROLE_PASSIVE) || local_cid == 0u ||
        (role == UTP_CONNECTION_ROLE_PASSIVE && peer_cid == 0u) || packet_limit == 0u ||
        packet_capacity < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(connection, 0, sizeof(*connection));
    error = utp_packet_out_pool_init(&connection->packet_pool, NULL, packet_limit, &bucket, 1u);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_send_control_init(&connection->send_control, packet_limit, UTP_CONNECTION_RETRANSMITTABLE_FRAMES, 16u,
                                  (uint64_t)UTP_CONNECTION_MAX_ACK_DELAY_MS * UINT64_C(1000));
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_receive_history_init(&connection->receive_history, NULL, UTP_CONNECTION_MAX_RECEIVE_RANGES);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_ack_scheduler_init(&connection->ack_scheduler, UTP_CONNECTION_ACK_ELICITING_THRESHOLD,
                                       UTP_CONNECTION_ACK_REORDER_THRESHOLD, UTP_CONNECTION_MAX_ACK_DELAY_MS);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_connection_cleanup(connection);
        return error;
    }
    connection->peer            = *peer;
    connection->local_cid       = local_cid;
    connection->peer_cid        = peer_cid;
    connection->packet_capacity = packet_capacity;
    connection->role            = role;
    connection->state = role == UTP_CONNECTION_ROLE_PASSIVE ? UTP_CONNECTION_STATE_CONNECTED : UTP_CONNECTION_STATE_NEW;
    utp_send_control_set_connected(&connection->send_control, role == UTP_CONNECTION_ROLE_PASSIVE);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_connection_cleanup(utp_connection_t *connection) {
    if (connection == NULL) {
        return;
    }
    utp_send_control_cleanup(&connection->send_control);
    utp_receive_history_cleanup(&connection->receive_history);
    utp_packet_out_pool_cleanup(&connection->packet_pool);
    memset(connection, 0, sizeof(*connection));
}

utp_internal_error_t utp_connection_queue_packet(utp_connection_t *connection, uint8_t packet_type,
                                                 const uint8_t *payload, size_t payload_length, bool track_on_send) {
    utp_packet_out_t    *packet;
    utp_internal_error_t error;
    uint64_t             packet_number;
    uint32_t             frame_types;
    size_t               packet_length;

    if (connection == NULL || !utp_connection_packet_type_is_valid(packet_type) ||
        (payload == NULL && payload_length != 0u) || connection->state == UTP_CONNECTION_STATE_CLOSING ||
        connection->state == UTP_CONNECTION_STATE_CLOSED || payload_length > UINT16_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((connection->state == UTP_CONNECTION_STATE_NEW &&
         (connection->role != UTP_CONNECTION_ROLE_ACTIVE || packet_type != UTP_PACKET_TYPE_INITIAL)) ||
        (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT && packet_type == UTP_PACKET_TYPE_INITIAL)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    packet_length = UTP_PACKET_HEADER_SIZE + payload_length;
    if (packet_length > connection->packet_capacity) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    error = utp_frame_scan(payload, payload_length, &frame_types);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_packet_out_pool_acquire(&connection->packet_pool, (uint16_t)packet_length, &packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
    if (error == UTP_INTERNAL_ERROR_OK) {
        packet->packet_number = packet_number;
        packet->data_size     = (uint16_t)packet_length;
        packet->frame_types   = frame_types;
        if (payload_length != 0u) {
            memcpy(packet->raw_data + UTP_PACKET_HEADER_SIZE, payload, payload_length);
        }
        if (packet_type == UTP_PACKET_TYPE_INITIAL || packet_type == UTP_PACKET_TYPE_HANDSHAKE) {
            packet->po_flags |= UTP_PO_HELLO;
        }
        error = utp_connection_encode_header(connection, packet, packet_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_send_control_schedule_packet(&connection->send_control, packet, track_on_send);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
    return error;
}

utp_packet_out_t *utp_connection_next_packet_to_send(utp_connection_t *connection) {
    utp_packet_out_t *packet;
    utp_packet_view_t view;
    uint64_t          packet_number;

    if (connection == NULL) {
        return NULL;
    }
    packet = utp_send_control_next_lost(&connection->send_control);
    if (packet == NULL) {
        return utp_send_control_next_scheduled(&connection->send_control);
    }
    if (utp_packet_view_decode(&view, packet->raw_data, packet->data_size) != UTP_INTERNAL_ERROR_OK ||
        utp_send_control_allocate_packet_number(&connection->send_control, &packet_number) != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
        return NULL;
    }
    packet->packet_number = packet_number;
    if (utp_connection_encode_header(connection, packet, view.header.type) != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
        return NULL;
    }
    return packet;
}

utp_internal_error_t utp_connection_on_packet_sent(utp_connection_t *connection, utp_packet_out_t *packet,
                                                   uint64_t now_us) {
    utp_packet_view_t          view;
    utp_frame_handshake_done_t done;
    utp_internal_error_t       error;
    bool                       has_handshake_done;
    bool                       tracked;

    if (connection == NULL || packet == NULL || now_us == 0u || packet->raw_data == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_packet_view_decode(&view, packet->raw_data, packet->data_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    tracked              = (packet->local_flags & UTP_POL_NO_TRACK_ON_SEND) == 0u;
    packet->sent_time_us = now_us;
    error                = utp_send_control_on_packet_sent(&connection->send_control, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    connection->tx_bytes += packet->data_size;
    if (view.header.type == UTP_PACKET_TYPE_INITIAL && connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        connection->state == UTP_CONNECTION_STATE_NEW) {
        connection->state = UTP_CONNECTION_STATE_INITIAL_SENT;
    } else if (view.header.type == UTP_PACKET_TYPE_CONNECTION_CLOSE ||
               (packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE)) != 0u) {
        connection->state = UTP_CONNECTION_STATE_CLOSING;
        utp_send_control_set_connected(&connection->send_control, false);
    }
    error = utp_connection_find_handshake_done(&view, &done, &has_handshake_done);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (has_handshake_done && connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        connection->state == UTP_CONNECTION_STATE_CONNECTED) {
        if (done.ack_handshake_packet_number != connection->peer_handshake_packet_number) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
    }
    if (!tracked) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_connection_on_packet_received(utp_connection_t *connection, const uint8_t *packet,
                                                       size_t packet_length, const utp_address_t *peer,
                                                       uint64_t now_us) {
    utp_packet_view_t    view;
    utp_internal_error_t error;
    uint64_t             largest_before;
    size_t               offset;
    bool                 handshake_done;

    if (connection == NULL || packet == NULL || peer == NULL || now_us == 0u ||
        !utp_address_equal(&connection->peer, peer)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + view.payload_length ||
        !utp_connection_packet_type_is_valid(view.header.type) || view.header.packet_number == 0u ||
        view.header.dcid != connection->local_cid) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (connection->role == UTP_CONNECTION_ROLE_ACTIVE && connection->state == UTP_CONNECTION_STATE_INITIAL_SENT &&
        view.header.type == UTP_PACKET_TYPE_HANDSHAKE && connection->peer_cid == 0u) {
        if (view.header.scid == 0u) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        connection->peer_cid = view.header.scid;
    } else if (view.header.scid != connection->peer_cid) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    largest_before = utp_receive_history_largest(&connection->receive_history);
    error          = utp_receive_history_insert(&connection->receive_history, view.header.packet_number, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    offset         = 0u;
    handshake_done = false;
    while (offset < view.payload_length) {
        const uint8_t *frame;
        uint8_t        frame_type;
        size_t         frame_length;

        error = utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (frame_type == UTP_FRAME_TYPE_ACK) {
            utp_ack_range_t               ranges[UTP_CONNECTION_MAX_RECEIVE_RANGES];
            utp_ack_info_t                ack = {0u, 0u, ranges, 0u, UTP_CONNECTION_MAX_RECEIVE_RANGES};
            utp_send_control_ack_result_t result;
            struct utp_packet_out_tailq   acknowledged;
            size_t                        consumed;

            TAILQ_INIT(&acknowledged);
            error = utp_ack_decode(&ack, frame, frame_length, 0u, &consumed);
            if (error == UTP_INTERNAL_ERROR_OK && consumed != frame_length) {
                error = UTP_INTERNAL_ERROR_PROTOCOL;
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_send_control_on_ack(&connection->send_control, &ack, now_us, &acknowledged, &result);
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            utp_connection_release_queue(connection, &acknowledged);
        } else if (frame_type == UTP_FRAME_TYPE_HANDSHAKE_DONE) {
            handshake_done = true;
        } else if (frame_type == UTP_FRAME_TYPE_CONNECTION_CLOSE) {
            connection->state = UTP_CONNECTION_STATE_CLOSING;
            utp_send_control_set_connected(&connection->send_control, false);
        }
    }
    (void)utp_ack_scheduler_on_packet(&connection->ack_scheduler, view.header.packet_number, largest_before,
                                      utp_connection_packet_is_ack_eliciting(&view), handshake_done, now_us);
    connection->rx_bytes += packet_length;
    if (view.header.type == UTP_PACKET_TYPE_HANDSHAKE && connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT ||
         connection->state == UTP_CONNECTION_STATE_CONNECTED)) {
        connection->peer_handshake_packet_number = view.header.packet_number;
        if (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT) {
            connection->state = UTP_CONNECTION_STATE_CONNECTED;
            utp_send_control_set_connected(&connection->send_control, true);
        }
    } else if (handshake_done && connection->role == UTP_CONNECTION_ROLE_PASSIVE &&
               connection->state != UTP_CONNECTION_STATE_CONNECTED) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_connection_state_t utp_connection_state(const utp_connection_t *connection) {
    return connection == NULL ? UTP_CONNECTION_STATE_CLOSED : connection->state;
}

bool utp_connection_is_connected(const utp_connection_t *connection) {
    return connection != NULL && connection->state == UTP_CONNECTION_STATE_CONNECTED;
}
