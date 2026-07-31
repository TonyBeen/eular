#include "connection/connection.h"

#include <string.h>

#define UTP_CONNECTION_RETRANSMITTABLE_FRAMES                                      \
    ((UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX) - 1u) &                                    \
     ~(UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING) | \
       UTP_FRAME_BIT(UTP_FRAME_TYPE_PING)))
#define UTP_CONNECTION_ACK_ELICITING_THRESHOLD        2u
#define UTP_CONNECTION_ACK_REORDER_THRESHOLD          1u
#define UTP_CONNECTION_MAX_ACK_DELAY_MS               25u
#define UTP_CONNECTION_DEFAULT_FLOW_WINDOW            (UTP_STREAM_DEFAULT_FLOW_WINDOW * 4u)
#define UTP_CONNECTION_FLOW_UPDATE_DIVISOR            10u
#define UTP_CONNECTION_FLOW_UPDATE_MIN_INTERVAL_US    UINT64_C(20000)
#define UTP_CONNECTION_FLOW_BLOCKED_MIN_INTERVAL_US   UINT64_C(50000)
#define UTP_CONNECTION_MAX_DATA_FRAME_SIZE            9u
#define UTP_CONNECTION_MAX_STREAM_DATA_FRAME_SIZE     13u
#define UTP_CONNECTION_DATA_BLOCKED_FRAME_SIZE        9u
#define UTP_CONNECTION_STREAM_DATA_BLOCKED_FRAME_SIZE 13u

static bool utp_connection_packet_type_is_valid(uint8_t type)
{
    return type >= UTP_PACKET_TYPE_INITIAL && type <= UTP_PACKET_TYPE_CONNECT;
}

static bool utp_connection_packet_is_ack_eliciting(const utp_packet_view_t* view)
{
    return view->frame_types != 0u &&
           (view->frame_types & ~(UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING))) != 0u;
}

static utp_internal_error_t utp_connection_find_handshake_done(const utp_packet_view_t*    view,
                                                               utp_frame_handshake_done_t* done, bool* found)
{
    size_t offset = 0u;

    *found = false;
    while (offset < view->payload_length) {
        const uint8_t*       frame;
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

static utp_internal_error_t utp_connection_encode_header(utp_connection_t* connection, utp_packet_out_t* packet,
                                                         uint8_t packet_type)
{
    const utp_packet_header_t header = {
        connection->local_cid, connection->peer_cid,
        packet->packet_number, (uint16_t)(packet->data_size - UTP_PACKET_HEADER_SIZE),
        packet_type,           0u,
    };

    return utp_proto_encode_header(packet->raw_data, packet->alloc_size, &header);
}

static void utp_connection_release_queue(utp_connection_t* connection, struct utp_packet_out_tailq* packets)
{
    utp_packet_out_t* packet;

    while ((packet = TAILQ_FIRST(packets)) != NULL) {
        if ((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u && packet->stream_data_size != 0u) {
            utp_stream_t* stream = utp_connection_find_stream(connection, packet->stream_id);

            if (stream != NULL) {
                (void)utp_stream_on_packet_acked_range(stream, packet->stream_offset, packet->stream_data_size);
            }
        }
        TAILQ_REMOVE(packets, packet, po_next);
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
}

static uint32_t utp_connection_local_stream_initiator_bit(const utp_connection_t* connection)
{
    return connection->role == UTP_CONNECTION_ROLE_ACTIVE ? UTP_STREAM_CLIENT_INITIATED : UTP_STREAM_SERVER_INITIATED;
}

static uint32_t utp_connection_peer_stream_initiator_bit(const utp_connection_t* connection)
{
    return connection->role == UTP_CONNECTION_ROLE_ACTIVE ? UTP_STREAM_SERVER_INITIATED : UTP_STREAM_CLIENT_INITIATED;
}

static bool utp_connection_stream_is_peer_initiated(const utp_connection_t* connection, uint32_t stream_id)
{
    return (stream_id & UINT32_C(1)) == utp_connection_peer_stream_initiator_bit(connection);
}

static bool utp_connection_take_pending_peer_max_stream_data(utp_connection_t* connection, uint32_t stream_id,
                                                             uint64_t* out_max_stream_data)
{
    size_t index;

    if (connection == NULL || out_max_stream_data == NULL) {
        return false;
    }
    for (index = 0u; index < connection->peer_max_stream_data_count; ++index) {
        if (connection->peer_max_stream_data_ids[index] == stream_id) {
            --connection->peer_max_stream_data_count;
            *out_max_stream_data = connection->peer_max_stream_data_values[index];
            if (index != connection->peer_max_stream_data_count) {
                connection->peer_max_stream_data_ids[index] =
                    connection->peer_max_stream_data_ids[connection->peer_max_stream_data_count];
                connection->peer_max_stream_data_values[index] =
                    connection->peer_max_stream_data_values[connection->peer_max_stream_data_count];
            }
            return true;
        }
    }
    return false;
}

static utp_internal_error_t utp_connection_store_pending_peer_max_stream_data(utp_connection_t* connection,
                                                                              uint32_t          stream_id,
                                                                              uint64_t          maximum_stream_data)
{
    size_t index;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < connection->peer_max_stream_data_count; ++index) {
        if (connection->peer_max_stream_data_ids[index] == stream_id) {
            if (maximum_stream_data > connection->peer_max_stream_data_values[index]) {
                connection->peer_max_stream_data_values[index] = maximum_stream_data;
            }
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    if (connection->peer_max_stream_data_count >= UTP_CONNECTION_MAX_STREAMS) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    index                                          = connection->peer_max_stream_data_count;
    connection->peer_max_stream_data_ids[index]    = stream_id;
    connection->peer_max_stream_data_values[index] = maximum_stream_data;
    connection->peer_max_stream_data_count         = index + 1u;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_stream_t* utp_connection_alloc_stream(utp_connection_t* connection, uint32_t stream_id)
{
    size_t index;

    for (index = 0u; index < UTP_CONNECTION_MAX_STREAMS; ++index) {
        if (!connection->streams[index].used) {
            uint64_t pending_max_stream_data;

            utp_stream_init(&connection->streams[index], stream_id);
            if (utp_connection_take_pending_peer_max_stream_data(connection, stream_id, &pending_max_stream_data)) {
                utp_stream_update_peer_max_stream_data(&connection->streams[index], pending_max_stream_data);
            }
            return &connection->streams[index];
        }
    }
    return NULL;
}

static utp_internal_error_t utp_connection_get_or_create_peer_stream(utp_connection_t* connection, uint32_t stream_id,
                                                                     utp_stream_t** out_stream)
{
    utp_stream_t* stream;

    if (out_stream == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream = utp_connection_find_stream(connection, stream_id);
    if (stream != NULL) {
        *out_stream = stream;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (!utp_connection_stream_is_peer_initiated(connection, stream_id)) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    stream = utp_connection_alloc_stream(connection, stream_id);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    *out_stream = stream;
    return UTP_INTERNAL_ERROR_OK;
}

static bool utp_connection_flow_update_due(uint64_t base_window, uint64_t consumed, uint64_t advertised,
                                           uint64_t last_sent_us, uint64_t now_us, uint64_t* out_target)
{
    uint64_t target;
    uint64_t threshold;
    bool     due_by_time;

    if (out_target == NULL || consumed > UINT64_MAX - base_window) {
        return false;
    }
    target      = base_window + consumed;
    *out_target = target;
    if (target <= advertised) {
        return false;
    }
    threshold = base_window / UTP_CONNECTION_FLOW_UPDATE_DIVISOR;
    if (threshold == 0u) {
        threshold = 1u;
    }
    due_by_time = last_sent_us == 0u || (now_us != 0u && now_us > last_sent_us &&
                                         now_us - last_sent_us >= UTP_CONNECTION_FLOW_UPDATE_MIN_INTERVAL_US);
    return target - advertised >= threshold || due_by_time;
}

static bool utp_connection_flow_blocked_due(uint64_t last_sent_us, uint64_t now_us)
{
    return last_sent_us == 0u || (now_us != 0u && now_us > last_sent_us &&
                                  now_us - last_sent_us >= UTP_CONNECTION_FLOW_BLOCKED_MIN_INTERVAL_US);
}

static utp_internal_error_t utp_connection_queue_max_data(utp_connection_t* connection, uint64_t maximum_data,
                                                          uint64_t now_us)
{
    uint8_t                    payload[UTP_CONNECTION_MAX_DATA_FRAME_SIZE];
    const utp_frame_max_data_t frame = {maximum_data};
    utp_internal_error_t       error;

    error = utp_frame_max_data_encode(payload, sizeof(payload), &frame);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), true);
    if (error == UTP_INTERNAL_ERROR_OK && now_us != 0u) {
        connection->last_max_data_sent_us = now_us;
    }
    return error;
}

static utp_internal_error_t utp_connection_queue_max_stream_data(utp_connection_t* connection, uint32_t stream_id,
                                                                 uint64_t maximum_stream_data, uint64_t now_us)
{
    uint8_t                           payload[UTP_CONNECTION_MAX_STREAM_DATA_FRAME_SIZE];
    const utp_frame_max_stream_data_t frame = {stream_id, maximum_stream_data};
    utp_internal_error_t              error;
    utp_stream_t*                     stream;

    error = utp_frame_max_stream_data_encode(payload, sizeof(payload), &frame);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), true);
    if (error == UTP_INTERNAL_ERROR_OK && now_us != 0u) {
        stream = utp_connection_find_stream(connection, stream_id);
        if (stream != NULL) {
            stream->last_max_stream_data_sent_us = now_us;
        }
    }
    return error;
}

static utp_internal_error_t utp_connection_queue_data_blocked(utp_connection_t* connection, uint64_t data_limit,
                                                              uint64_t now_us)
{
    uint8_t                        payload[UTP_CONNECTION_DATA_BLOCKED_FRAME_SIZE];
    const utp_frame_data_blocked_t frame = {data_limit};
    utp_internal_error_t           error;

    error = utp_frame_data_blocked_encode(payload, sizeof(payload), &frame);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), true);
    if (error == UTP_INTERNAL_ERROR_OK && now_us != 0u) {
        connection->last_data_blocked_sent_us = now_us;
    }
    return error;
}

static utp_internal_error_t utp_connection_queue_stream_data_blocked(utp_connection_t* connection, uint32_t stream_id,
                                                                     uint64_t stream_data_limit, uint64_t now_us)
{
    uint8_t                               payload[UTP_CONNECTION_STREAM_DATA_BLOCKED_FRAME_SIZE];
    const utp_frame_stream_data_blocked_t frame = {stream_id, stream_data_limit};
    utp_internal_error_t                  error;
    utp_stream_t*                         stream;

    error = utp_frame_stream_data_blocked_encode(payload, sizeof(payload), &frame);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), true);
    if (error == UTP_INTERNAL_ERROR_OK && now_us != 0u) {
        stream = utp_connection_find_stream(connection, stream_id);
        if (stream != NULL) {
            stream->last_stream_data_blocked_sent_us = now_us;
        }
    }
    return error;
}

static utp_internal_error_t utp_connection_queue_pending_flow_control(utp_connection_t* connection, uint64_t now_us,
                                                                      bool* queued)
{
    uint64_t target;
    size_t   index;

    if (queued == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *queued = false;
    if (!utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (utp_connection_flow_update_due(UTP_CONNECTION_DEFAULT_FLOW_WINDOW, connection->local_stream_data_consumed_total,
                                       connection->local_max_data_advertised, connection->last_max_data_sent_us, now_us,
                                       &target)) {
        utp_internal_error_t error = utp_connection_queue_max_data(connection, target, now_us);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        connection->local_max_data_advertised = target;
        *queued                               = true;
        return UTP_INTERNAL_ERROR_OK;
    }
    for (index = 0u; index < UTP_CONNECTION_MAX_STREAMS; ++index) {
        utp_stream_t* stream = &connection->streams[index];

        if (!stream->used) {
            continue;
        }
        if (utp_connection_flow_update_due(UTP_STREAM_DEFAULT_FLOW_WINDOW, stream->recv_offset,
                                           stream->local_max_stream_data_advertised,
                                           stream->last_max_stream_data_sent_us, now_us, &target)) {
            utp_internal_error_t error =
                utp_connection_queue_max_stream_data(connection, stream->stream_id, target, now_us);

            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            stream->local_max_stream_data_advertised = target;
            *queued                                  = true;
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_on_stream_bytes_consumed(utp_connection_t* connection, size_t bytes)
{
    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (bytes == 0u) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if ((uint64_t)bytes > UINT64_MAX - connection->local_stream_data_consumed_total) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    connection->local_stream_data_consumed_total += (uint64_t)bytes;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_queue_next_stream_packet(utp_connection_t* connection, uint64_t now_us,
                                                                    bool* queued)
{
    uint8_t              stream_header[UTP_FRAME_STREAM_HEADER_SIZE];
    utp_packet_out_t*    packet = NULL;
    const uint8_t*       stream_data;
    size_t               index;
    size_t               stream_header_length;
    size_t               packet_length;
    size_t               raw_length;
    size_t               max_data_length;
    uint32_t             stream_data_size;
    uint64_t             stream_offset;
    uint64_t             packet_number;
    bool                 fin;
    utp_internal_error_t error;

    if (queued == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *queued = false;
    if (!utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    for (index = 0u; index < UTP_CONNECTION_MAX_STREAMS; ++index) {
        utp_stream_t* stream = &connection->streams[index];

        if (!utp_stream_has_send_work(stream)) {
            continue;
        }
        if (connection->packet_capacity < UTP_PACKET_HEADER_SIZE + UTP_FRAME_STREAM_HEADER_SIZE) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        if (connection->stream_data_sent_total > connection->peer_max_data) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (stream->send_buffer_length != stream->send_in_flight_bytes &&
            connection->stream_data_sent_total == connection->peer_max_data) {
            if (utp_connection_flow_blocked_due(connection->last_data_blocked_sent_us, now_us)) {
                error = utp_connection_queue_data_blocked(connection, connection->peer_max_data, now_us);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    return error;
                }
                *queued = true;
            }
            return UTP_INTERNAL_ERROR_OK;
        }
        if (stream->send_buffer_length != stream->send_in_flight_bytes &&
            stream->next_send_offset >= stream->peer_max_stream_data) {
            if (utp_connection_flow_blocked_due(stream->last_stream_data_blocked_sent_us, now_us)) {
                error = utp_connection_queue_stream_data_blocked(connection, stream->stream_id,
                                                                 stream->peer_max_stream_data, now_us);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    return error;
                }
                *queued = true;
            }
            return UTP_INTERNAL_ERROR_OK;
        }
        max_data_length = (size_t)(connection->packet_capacity - UTP_PACKET_HEADER_SIZE - UTP_FRAME_STREAM_HEADER_SIZE);
        if (max_data_length > connection->peer_max_data - connection->stream_data_sent_total) {
            max_data_length = (size_t)(connection->peer_max_data - connection->stream_data_sent_total);
        }
        packet = NULL;
        error  = utp_stream_build_frame_view_limited(stream, stream_header, sizeof(stream_header), max_data_length,
                                                     &stream_header_length, &stream_data, &stream_data_size,
                                                     &stream_offset, &fin);
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            continue;
        }
        packet_length = UTP_PACKET_HEADER_SIZE + stream_header_length + (size_t)stream_data_size;
        raw_length    = UTP_PACKET_HEADER_SIZE + stream_header_length;
        if (packet_length > connection->packet_capacity || raw_length > UINT16_MAX) {
            error = UTP_INTERNAL_ERROR_LIMIT;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_packet_out_pool_acquire(&connection->packet_pool, (uint16_t)raw_length, &packet);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            packet->packet_number    = packet_number;
            packet->data_size        = (uint16_t)packet_length;
            packet->packet_type      = UTP_PACKET_TYPE_CTRL;
            packet->frame_types      = UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM);
            packet->stream_id        = stream->stream_id;
            packet->stream_offset    = stream_offset;
            packet->stream_data_size = stream_data_size;
            memcpy(packet->raw_data + UTP_PACKET_HEADER_SIZE, stream_header, stream_header_length);
            packet->slices[0].source = UTP_PACKET_OUT_SLICE_RAW_OFFSET;
            packet->slices[0].offset = 0u;
            packet->slices[0].length = (uint16_t)raw_length;
            packet->slices[0].data   = NULL;
            packet->slice_count      = 1u;
            if (stream_data_size != 0u) {
                packet->slices[1].source = UTP_PACKET_OUT_SLICE_EXTERNAL;
                packet->slices[1].offset = 0u;
                packet->slices[1].length = (uint16_t)stream_data_size;
                packet->slices[1].data   = stream_data;
                packet->slice_count      = 2u;
            }
            error = utp_connection_encode_header(connection, packet, UTP_PACKET_TYPE_CTRL);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_send_control_schedule_packet(&connection->send_control, packet, true);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_stream_commit_built_frame(stream, stream_data_size, fin);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            if ((uint64_t)stream_data_size > UINT64_MAX - connection->stream_data_sent_total) {
                error = UTP_INTERNAL_ERROR_OVERFLOW;
            } else {
                connection->stream_data_sent_total += (uint64_t)stream_data_size;
            }
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            *queued = true;
        } else if (packet != NULL) {
            utp_packet_out_pool_release(&connection->packet_pool, packet);
        }
        return error;
    }
    return UTP_INTERNAL_ERROR_OK;
}

static uint64_t utp_connection_calculate_retransmission_delay(utp_connection_t* connection)
{
    switch (utp_send_control_retransmission_mode(&connection->send_control)) {
    case UTP_SEND_CONTROL_RETRANSMISSION_HANDSHAKE:
        return utp_send_control_calculate_handshake_delay(&connection->send_control);
    case UTP_SEND_CONTROL_RETRANSMISSION_LOSS:
        return 1u;
    case UTP_SEND_CONTROL_RETRANSMISSION_TLP:
        return utp_send_control_calculate_tlp_delay(&connection->send_control);
    case UTP_SEND_CONTROL_RETRANSMISSION_RTO:
        return utp_send_control_calculate_rto(&connection->send_control);
    }
    return 0u;
}

utp_internal_error_t utp_connection_init(utp_connection_t* connection, utp_connection_role_t role, uint32_t local_cid,
                                         uint32_t peer_cid, const utp_address_t* peer, size_t packet_limit,
                                         uint16_t packet_capacity)
{
    const utp_packet_out_bucket_config_t bucket = {packet_capacity, packet_limit};
    utp_internal_error_t                 error;
    size_t                               index;

    if (connection == NULL || peer == NULL ||
        (role != UTP_CONNECTION_ROLE_ACTIVE && role != UTP_CONNECTION_ROLE_PASSIVE) || local_cid == 0u ||
        (role == UTP_CONNECTION_ROLE_PASSIVE && peer_cid == 0u) || packet_limit == 0u ||
        packet_capacity < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < UTP_CONNECTION_MAX_STREAMS; ++index) {
        connection->streams[index].used = false;
    }
    for (index = 0u; index < UTP_STREAM_TYPES; ++index) {
        connection->next_stream_id[index] = 0u;
    }
    connection->rx_bytes                     = 0u;
    connection->tx_bytes                     = 0u;
    connection->peer_handshake_packet_number = 0u;
    connection->retransmission_deadline_us   = 0u;
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
    connection->peer                             = *peer;
    connection->local_cid                        = local_cid;
    connection->peer_cid                         = peer_cid;
    connection->peer_max_data                    = UTP_CONNECTION_DEFAULT_FLOW_WINDOW;
    connection->local_max_data_advertised        = UTP_CONNECTION_DEFAULT_FLOW_WINDOW;
    connection->stream_data_sent_total           = 0u;
    connection->local_stream_data_received_total = 0u;
    connection->local_stream_data_consumed_total = 0u;
    connection->last_max_data_sent_us            = 0u;
    connection->last_data_blocked_sent_us        = 0u;
    connection->packet_capacity                  = packet_capacity;
    connection->recv_reassembly_memory_bytes     = 0u;
    connection->recv_reassembly_fragment_count   = 0u;
    connection->role                             = role;
    connection->peer_max_stream_data_count       = 0u;
    connection->state = role == UTP_CONNECTION_ROLE_PASSIVE ? UTP_CONNECTION_STATE_CONNECTED : UTP_CONNECTION_STATE_NEW;
    {
        uint32_t local_bit = utp_connection_local_stream_initiator_bit(connection);

        connection->next_stream_id[local_bit]                             = local_bit;
        connection->next_stream_id[local_bit | UTP_STREAM_UNIDIRECTIONAL] = local_bit | UTP_STREAM_UNIDIRECTIONAL;
    }
    utp_send_control_set_connected(&connection->send_control, role == UTP_CONNECTION_ROLE_PASSIVE);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_connection_cleanup(utp_connection_t* connection)
{
    size_t index;

    if (connection == NULL) {
        return;
    }
    utp_send_control_cleanup(&connection->send_control);
    utp_receive_history_cleanup(&connection->receive_history);
    utp_packet_out_pool_cleanup(&connection->packet_pool);
    for (index = 0u; index < UTP_CONNECTION_MAX_STREAMS; ++index) {
        if (connection->streams[index].used) {
            utp_stream_reset(&connection->streams[index]);
        }
        connection->streams[index].used = false;
    }
    for (index = 0u; index < UTP_STREAM_TYPES; ++index) {
        connection->next_stream_id[index] = 0u;
    }
    connection->local_cid                        = 0u;
    connection->peer_cid                         = 0u;
    connection->peer_max_data                    = 0u;
    connection->local_max_data_advertised        = 0u;
    connection->stream_data_sent_total           = 0u;
    connection->local_stream_data_received_total = 0u;
    connection->local_stream_data_consumed_total = 0u;
    connection->last_max_data_sent_us            = 0u;
    connection->last_data_blocked_sent_us        = 0u;
    connection->packet_capacity                  = 0u;
    connection->recv_reassembly_memory_bytes     = 0u;
    connection->recv_reassembly_fragment_count   = 0u;
    connection->rx_bytes                         = 0u;
    connection->tx_bytes                         = 0u;
    connection->peer_handshake_packet_number     = 0u;
    connection->retransmission_deadline_us       = 0u;
    connection->role                             = UTP_CONNECTION_ROLE_ACTIVE;
    connection->peer_max_stream_data_count       = 0u;
    connection->state                            = UTP_CONNECTION_STATE_CLOSED;
}

utp_internal_error_t utp_connection_queue_packet(utp_connection_t* connection, uint8_t packet_type,
                                                 const uint8_t* payload, size_t payload_length, bool track_on_send)
{
    utp_packet_out_t*    packet;
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
        packet->packet_type   = packet_type;
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

utp_packet_out_t* utp_connection_next_packet_to_send_at(utp_connection_t* connection, uint64_t now_us)
{
    utp_packet_out_t* packet;
    uint64_t          packet_number;
    bool              queued_flow_control  = false;
    bool              queued_stream_packet = false;

    if (connection == NULL) {
        return NULL;
    }
    packet = utp_send_control_next_lost(&connection->send_control);
    if (packet == NULL) {
        packet = utp_send_control_next_scheduled(&connection->send_control);
        if (packet == NULL &&
            utp_connection_queue_pending_flow_control(connection, now_us, &queued_flow_control) ==
                UTP_INTERNAL_ERROR_OK &&
            queued_flow_control) {
            packet = utp_send_control_next_scheduled(&connection->send_control);
        }
        if (packet == NULL &&
            utp_connection_queue_next_stream_packet(connection, now_us, &queued_stream_packet) ==
                UTP_INTERNAL_ERROR_OK &&
            queued_stream_packet) {
            packet = utp_send_control_next_scheduled(&connection->send_control);
        }
        return packet;
    }
    if (!utp_connection_packet_type_is_valid(packet->packet_type) ||
        utp_send_control_allocate_packet_number(&connection->send_control, &packet_number) != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
        return NULL;
    }
    packet->packet_number = packet_number;
    if (utp_connection_encode_header(connection, packet, packet->packet_type) != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
        return NULL;
    }
    return packet;
}

utp_packet_out_t* utp_connection_next_packet_to_send(utp_connection_t* connection)
{
    return utp_connection_next_packet_to_send_at(connection, 0u);
}

utp_internal_error_t utp_connection_on_packet_sent(utp_connection_t* connection, utp_packet_out_t* packet,
                                                   uint64_t now_us)
{
    utp_packet_view_t          view;
    utp_frame_handshake_done_t done;
    utp_internal_error_t       error;
    bool                       has_handshake_done;
    bool                       tracked;

    if (connection == NULL || packet == NULL || now_us == 0u || packet->raw_data == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    tracked              = (packet->local_flags & UTP_POL_NO_TRACK_ON_SEND) == 0u;
    packet->sent_time_us = now_us;
    error                = utp_send_control_on_packet_sent(&connection->send_control, packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    connection->tx_bytes += packet->data_size;
    if (packet->packet_type == UTP_PACKET_TYPE_INITIAL && connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        connection->state == UTP_CONNECTION_STATE_NEW) {
        connection->state = UTP_CONNECTION_STATE_INITIAL_SENT;
    } else if (packet->packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE ||
               (packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE)) != 0u) {
        connection->state = UTP_CONNECTION_STATE_CLOSING;
        utp_send_control_set_connected(&connection->send_control, false);
    }
    has_handshake_done = false;
    if ((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_HANDSHAKE_DONE)) != 0u) {
        error = utp_packet_view_decode(&view, packet->raw_data, packet->data_size);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        error = utp_connection_find_handshake_done(&view, &done, &has_handshake_done);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if (has_handshake_done && connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        connection->state == UTP_CONNECTION_STATE_CONNECTED) {
        if (done.ack_handshake_packet_number != connection->peer_handshake_packet_number) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
    }
    if (!tracked) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    } else if (utp_send_control_unacked_packet_count(&connection->send_control) != 0u &&
               connection->retransmission_deadline_us == 0u) {
        error = utp_connection_ensure_retransmission_deadline(connection, now_us);
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_on_packet_received_internal(utp_connection_t* connection,
                                                                       const uint8_t* packet, size_t packet_length,
                                                                       utp_packet_in_t*     packet_in,
                                                                       const utp_address_t* peer, uint64_t now_us)
{
    utp_packet_view_t    view;
    utp_internal_error_t error;
    uint64_t             largest_before;
    size_t               offset;
    bool                 handshake_done;
    bool                 ack_progress;

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
    ack_progress   = false;
    while (offset < view.payload_length) {
        const uint8_t* frame;
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
            if (result.ledger.acknowledged_packet_count != 0u) {
                ack_progress = true;
            }
            utp_connection_release_queue(connection, &acknowledged);
        } else if (frame_type == UTP_FRAME_TYPE_HANDSHAKE_DONE) {
            handshake_done = true;
        } else if (frame_type == UTP_FRAME_TYPE_CONNECTION_CLOSE) {
            connection->state = UTP_CONNECTION_STATE_CLOSING;
            utp_send_control_set_connected(&connection->send_control, false);
        } else if (frame_type == UTP_FRAME_TYPE_STREAM) {
            utp_frame_stream_t        stream_frame;
            utp_stream_t*             stream;
            utp_stream_recv_account_t recv_account;
            uint64_t                  frame_end;
            uint64_t                  stream_delta = 0u;

            error = utp_frame_stream_decode(&stream_frame, frame, frame_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_connection_get_or_create_peer_stream(connection, stream_frame.stream_id, &stream);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                if ((uint64_t)stream_frame.data_length > UINT64_MAX - stream_frame.offset) {
                    error = UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL;
                } else {
                    frame_end = stream_frame.offset + (uint64_t)stream_frame.data_length;
                    if (frame_end > stream->local_max_stream_data_advertised) {
                        error = UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL;
                    } else if (frame_end > stream->local_max_stream_offset_received) {
                        stream_delta = frame_end - stream->local_max_stream_offset_received;
                        if (stream_delta > UINT64_MAX - connection->local_stream_data_received_total) {
                            error = UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL;
                        } else if (connection->local_stream_data_received_total + stream_delta >
                                   connection->local_max_data_advertised) {
                            error = UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL;
                        }
                    }
                }
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                recv_account.connection_memory_bytes   = &connection->recv_reassembly_memory_bytes;
                recv_account.connection_fragment_count = &connection->recv_reassembly_fragment_count;
                recv_account.connection_memory_limit   = UTP_CONNECTION_RECV_REASSEMBLY_MEMORY_LIMIT;
                recv_account.connection_fragment_limit = UTP_CONNECTION_RECV_REASSEMBLY_FRAGMENT_LIMIT;
                error = utp_stream_on_frame_packet_accounted(stream, &stream_frame, packet_in, &recv_account);
            }
            if (error == UTP_INTERNAL_ERROR_OK && stream_delta != 0u) {
                stream->local_max_stream_offset_received = stream_frame.offset + (uint64_t)stream_frame.data_length;
                connection->local_stream_data_received_total += stream_delta;
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        } else if (frame_type == UTP_FRAME_TYPE_RESET_STREAM) {
            utp_frame_reset_stream_t reset;
            utp_stream_t*            stream;

            error = utp_frame_reset_stream_decode(&reset, frame, frame_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_connection_get_or_create_peer_stream(connection, reset.stream_id, &stream);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                utp_stream_reset(stream);
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        } else if (frame_type == UTP_FRAME_TYPE_MAX_DATA) {
            utp_frame_max_data_t max_data;

            error = utp_frame_max_data_decode(&max_data, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (max_data.maximum_data > connection->peer_max_data) {
                connection->peer_max_data = max_data.maximum_data;
            }
        } else if (frame_type == UTP_FRAME_TYPE_MAX_STREAM_DATA) {
            utp_frame_max_stream_data_t max_stream_data;
            utp_stream_t*               stream;

            error = utp_frame_max_stream_data_decode(&max_stream_data, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            stream = utp_connection_find_stream(connection, max_stream_data.stream_id);
            if (stream != NULL) {
                utp_stream_update_peer_max_stream_data(stream, max_stream_data.maximum_stream_data);
            } else {
                error = utp_connection_store_pending_peer_max_stream_data(connection, max_stream_data.stream_id,
                                                                          max_stream_data.maximum_stream_data);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    return error;
                }
            }
        } else if (frame_type == UTP_FRAME_TYPE_DATA_BLOCKED) {
            utp_frame_data_blocked_t blocked;

            error = utp_frame_data_blocked_decode(&blocked, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            error = utp_connection_queue_max_data(connection, connection->local_max_data_advertised, now_us);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        } else if (frame_type == UTP_FRAME_TYPE_STREAM_DATA_BLOCKED) {
            utp_frame_stream_data_blocked_t blocked;
            utp_stream_t*                   stream;
            uint64_t                        advertised;

            error = utp_frame_stream_data_blocked_decode(&blocked, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            stream     = utp_connection_find_stream(connection, blocked.stream_id);
            advertised = stream == NULL ? UTP_STREAM_DEFAULT_FLOW_WINDOW : stream->local_max_stream_data_advertised;
            error      = utp_connection_queue_max_stream_data(connection, blocked.stream_id, advertised, now_us);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
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
    if (ack_progress) {
        connection->retransmission_deadline_us = 0u;
        error                                  = utp_connection_ensure_retransmission_deadline(connection, now_us);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_connection_on_packet_received(utp_connection_t* connection, const uint8_t* packet,
                                                       size_t packet_length, const utp_address_t* peer, uint64_t now_us)
{
    return utp_connection_on_packet_received_internal(connection, packet, packet_length, NULL, peer, now_us);
}

utp_internal_error_t utp_connection_on_packet_in_received(utp_connection_t* connection, utp_packet_in_t* packet,
                                                          const utp_address_t* peer, uint64_t now_us)
{
    if (packet == NULL || packet->data == NULL || !packet->in_use) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return utp_connection_on_packet_received_internal(connection, packet->data, packet->length, packet, peer, now_us);
}

utp_internal_error_t utp_connection_queue_ack(utp_connection_t* connection, uint64_t now_us)
{
    uint8_t payload[UTP_ACK_FRAME_HEADER_SIZE + (UTP_CONNECTION_MAX_RECEIVE_RANGES - 1u) * UTP_ACK_FRAME_RANGE_SIZE];
    utp_ack_range_t      ranges[UTP_CONNECTION_MAX_RECEIVE_RANGES];
    utp_ack_info_t       ack = {0u, 0u, ranges, 0u, UTP_CONNECTION_MAX_RECEIVE_RANGES};
    size_t               payload_length;
    utp_internal_error_t error;

    if (connection == NULL || now_us == 0u || utp_ack_scheduler_pending_count(&connection->ack_scheduler) == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_ack_from_receive_history(&ack, &connection->receive_history, now_us, UTP_CONNECTION_MAX_RECEIVE_RANGES);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_ack_encode(payload, sizeof(payload), &ack, 0u, &payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, payload_length, false);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_ack_scheduler_on_ack_sent(&connection->ack_scheduler);
    }
    return error;
}

uint32_t utp_connection_ack_pending_count(const utp_connection_t* connection)
{
    return connection == NULL ? 0u : utp_ack_scheduler_pending_count(&connection->ack_scheduler);
}

uint64_t utp_connection_ack_deadline(const utp_connection_t* connection)
{
    return connection == NULL ? 0u : utp_ack_scheduler_deadline(&connection->ack_scheduler);
}

utp_internal_error_t utp_connection_ensure_retransmission_deadline(utp_connection_t* connection, uint64_t now_us)
{
    uint64_t delay_us;

    if (connection == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (utp_send_control_unacked_packet_count(&connection->send_control) == 0u) {
        connection->retransmission_deadline_us = 0u;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (connection->retransmission_deadline_us != 0u) {
        return UTP_INTERNAL_ERROR_OK;
    }
    delay_us = utp_connection_calculate_retransmission_delay(connection);
    if (delay_us == 0u || delay_us > UINT64_MAX - now_us) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    connection->retransmission_deadline_us = now_us + delay_us;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_connection_on_retransmission_timeout(utp_connection_t* connection, uint64_t now_us)
{
    utp_packet_out_t*    discarded;
    utp_internal_error_t error;

    if (connection == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection->retransmission_deadline_us = 0u;
    error                                  = utp_send_control_on_retransmission_timeout(&connection->send_control);
    while ((discarded = utp_send_control_next_discarded(&connection->send_control)) != NULL) {
        utp_packet_out_pool_release(&connection->packet_pool, discarded);
    }
    return error;
}

uint64_t utp_connection_retransmission_deadline(const utp_connection_t* connection)
{
    return connection == NULL ? 0u : connection->retransmission_deadline_us;
}

utp_internal_error_t utp_connection_create_stream(utp_connection_t* connection, bool bidirectional,
                                                  uint32_t* out_stream_id)
{
    uint32_t      slot;
    uint32_t      stream_id;
    utp_stream_t* stream;

    if (connection == NULL || out_stream_id == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (!utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    slot       = utp_connection_local_stream_initiator_bit(connection);
    slot      |= bidirectional ? 0u : UTP_STREAM_UNIDIRECTIONAL;
    stream_id  = connection->next_stream_id[slot];
    if (stream_id > UINT32_MAX - UTP_STREAM_TYPES) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    stream = utp_connection_alloc_stream(connection, stream_id);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    connection->next_stream_id[slot] = stream_id + UTP_STREAM_TYPES;
    *out_stream_id                   = stream_id;
    return UTP_INTERNAL_ERROR_OK;
}

utp_stream_t* utp_connection_find_stream(utp_connection_t* connection, uint32_t stream_id)
{
    size_t index;

    if (connection == NULL) {
        return NULL;
    }
    for (index = 0u; index < UTP_CONNECTION_MAX_STREAMS; ++index) {
        if (connection->streams[index].used && connection->streams[index].stream_id == stream_id) {
            return &connection->streams[index];
        }
    }
    return NULL;
}

utp_internal_error_t utp_connection_stream_write(utp_connection_t* connection, uint32_t stream_id, const uint8_t* data,
                                                 size_t length, bool fin)
{
    utp_stream_t* stream;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream = utp_connection_find_stream(connection, stream_id);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    return utp_stream_write(stream, data, length, fin);
}

utp_internal_error_t utp_connection_stream_read(utp_connection_t* connection, uint32_t stream_id, uint8_t* buffer,
                                                size_t capacity, size_t* out_length, bool* out_fin)
{
    utp_stream_t*        stream;
    utp_internal_error_t error;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream = utp_connection_find_stream(connection, stream_id);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    error = utp_stream_read(stream, buffer, capacity, out_length, out_fin);
    if (error == UTP_INTERNAL_ERROR_OK && *out_length != 0u) {
        error = utp_connection_on_stream_bytes_consumed(connection, *out_length);
    }
    return error;
}

utp_internal_error_t utp_connection_stream_acquire_read_view(utp_connection_t* connection, uint32_t stream_id,
                                                             utp_stream_read_view_t* out_view)
{
    utp_stream_t* stream;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream = utp_connection_find_stream(connection, stream_id);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    return utp_stream_acquire_read_view(stream, out_view);
}

utp_internal_error_t utp_connection_stream_commit_read_view(utp_connection_t* connection, uint32_t stream_id,
                                                            uint64_t offset, size_t length)
{
    utp_stream_t*        stream;
    utp_internal_error_t error;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream = utp_connection_find_stream(connection, stream_id);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    error = utp_stream_commit_read_view(stream, offset, length);
    if (error == UTP_INTERNAL_ERROR_OK && length != 0u) {
        error = utp_connection_on_stream_bytes_consumed(connection, length);
    }
    return error;
}

utp_connection_state_t utp_connection_state(const utp_connection_t* connection)
{
    return connection == NULL ? UTP_CONNECTION_STATE_CLOSED : connection->state;
}

bool utp_connection_is_connected(const utp_connection_t* connection)
{
    return connection != NULL && connection->state == UTP_CONNECTION_STATE_CONNECTED;
}
