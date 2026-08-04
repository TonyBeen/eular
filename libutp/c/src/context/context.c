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
    utp_context_t*       context;
    utp_connection_t*    connection;
    const utp_address_t* peer;
    uint64_t             now_us;
} utp_context_replay_t;

static void utp_context_report_connection_error(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                utp_status_t status, uint16_t peer_error_code, const uint8_t* reason,
                                                size_t reason_length, bool peer_initiated);
static void utp_context_report_connect_error(utp_context_t* context, utp_status_t status, const char* message,
                                             const utp_connect_attempt_info_t* attempt);
static utp_internal_error_t utp_context_flush_connection(utp_context_t* context, utp_context_connection_slot_t* slot);
static utp_internal_error_t utp_context_refresh_timer(utp_context_t* context, uint64_t now_us);
static void                 utp_context_on_udp_writable(uint32_t events, void* user_data);

static uint64_t             utp_context_now_us(void)
{
    uint64_t now_us = utp_clock_now_us(NULL);

    return now_us == 0u ? 1u : now_us;
}

static void utp_context_endpoint_from_address(utp_endpoint_t* endpoint, const utp_address_t* address)
{
    endpoint->family   = address->family;
    endpoint->port     = address->port;
    endpoint->scope_id = address->scope_id;
    memcpy(endpoint->address, address->address, sizeof(endpoint->address));
}

static bool utp_context_encryption_mode_is_valid(utp_encryption_mode_t encryption)
{
    return encryption == UTP_ENCRYPTION_NONE || encryption == UTP_ENCRYPTION_AES_GCM_128 ||
           encryption == UTP_ENCRYPTION_AES_GCM_256;
}

static bool utp_context_cid_in_use(const utp_context_t* context, uint32_t cid)
{
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

static utp_internal_error_t utp_context_alloc_cid(utp_context_t* context, uint32_t* out_cid)
{
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

static utp_context_connection_slot_t* utp_context_find_connection_slot(utp_context_t* context, uint32_t local_cid)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (context->connections[index].used && context->connections[index].connection.local_cid == local_cid) {
            return &context->connections[index];
        }
    }
    return NULL;
}

static utp_context_connection_slot_t* utp_context_find_connection_by_peer(utp_context_t*       context,
                                                                          const utp_address_t* peer)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (context->connections[index].used && utp_address_equal(&context->connections[index].connection.peer, peer)) {
            return &context->connections[index];
        }
    }
    return NULL;
}

static utp_context_connection_slot_t* utp_context_alloc_connection_slot(utp_context_t* context)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        if (!context->connections[index].used) {
            context->connections[index].connection.local_cid      = 0u;
            context->connections[index].connection.peer_cid       = 0u;
            context->connections[index].connect_deadline_us       = 0u;
            context->connections[index].connect_retries_remaining = 0;
            context->connections[index].used                      = true;
            context->connections[index].connected_reported        = false;
            context->connections[index].connection_error_reported = false;
            context->connections[index].connect_pending           = false;
            return &context->connections[index];
        }
    }
    return NULL;
}

static void utp_context_release_connection_slot(utp_context_connection_slot_t* slot)
{
    if (slot != NULL && slot->used) {
        if (slot->connection.local_cid != 0u) {
            utp_connection_cleanup(&slot->connection);
        }
        slot->connected_reported        = false;
        slot->connection_error_reported = false;
        slot->connect_deadline_us       = 0u;
        slot->connect_retries_remaining = 0;
        slot->connect_pending           = false;
        slot->used                      = false;
    }
}

static bool utp_context_is_peer_protocol_error(utp_internal_error_t error)
{
    return error == UTP_INTERNAL_ERROR_PROTOCOL || error == UTP_INTERNAL_ERROR_STREAM_FLOW_CONTROL ||
           error == UTP_INTERNAL_ERROR_STREAM_LIMIT;
}

static utp_internal_error_t utp_context_close_on_peer_protocol_error(utp_context_t*                 context,
                                                                     utp_context_connection_slot_t* slot,
                                                                     utp_internal_error_t           error)
{
    const utp_status_t status     = utp_internal_error_to_status(error);
    const char*        reason     = utp_status_string(status);
    const uint16_t     close_code = status < 0 && status >= -((int32_t)UINT16_MAX) ? (uint16_t)(-status) : UINT16_C(1);
    utp_internal_error_t close_error;

    utp_context_report_connection_error(context, slot, status, 0u, (const uint8_t*)reason, strlen(reason), false);
    close_error = utp_connection_queue_close(&slot->connection, close_code);
    if (close_error != UTP_INTERNAL_ERROR_OK) {
        return close_error;
    }
    return utp_context_flush_connection(context, slot);
}

static utp_context_pending_slot_t* utp_context_find_pending_slot(utp_context_t* context, uint32_t local_cid)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used && context->pending_incoming[index].pending.local_cid == local_cid) {
            return &context->pending_incoming[index];
        }
    }
    return NULL;
}

static utp_context_pending_slot_t* utp_context_find_pending_by_peer(utp_context_t* context, uint32_t peer_cid,
                                                                    const utp_address_t* peer)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used && context->pending_incoming[index].pending.peer_cid == peer_cid &&
            utp_address_equal(&context->pending_incoming[index].pending.peer, peer)) {
            return &context->pending_incoming[index];
        }
    }
    return NULL;
}

static utp_context_pending_slot_t* utp_context_alloc_pending_slot(utp_context_t* context)
{
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

static void utp_context_release_pending_slot(utp_context_pending_slot_t* slot)
{
    if (slot != NULL && slot->used) {
        utp_pending_incoming_reset(&slot->pending);
        slot->queued = false;
        slot->used   = false;
    }
}

static utp_internal_error_t utp_context_send_raw(utp_context_t* context, const utp_address_t* peer,
                                                 const uint8_t* packet, size_t packet_length)
{
    size_t sent_length = 0u;

    return utp_udp_socket_send_to(&context->udp_socket, packet, packet_length, peer, &sent_length);
}

static utp_internal_error_t utp_context_resolve_packet_slice(const utp_packet_out_t*       packet,
                                                             const utp_packet_out_slice_t* slice,
                                                             utp_udp_send_slice_t*         out_slice)
{
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

static utp_internal_error_t utp_context_send_packet(utp_context_t* context, const utp_address_t* peer,
                                                    const utp_packet_out_t* packet)
{
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

static void utp_context_send_destroy_close(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    utp_connection_t*    connection;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || !slot->used || !utp_udp_socket_is_open(&context->udp_socket)) {
        return;
    }
    connection = &slot->connection;
    error      = utp_connection_prepare_destroy_close(connection);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return;
    }
    (void)utp_context_send_packet(context, &connection->peer, &connection->close_packet);
}

static void utp_context_report_terminal_send_error(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                   utp_internal_error_t error, const char* reason)
{
    const utp_status_t status        = utp_internal_error_to_status(error);
    const size_t       reason_length = reason == NULL ? 0u : strlen(reason);

    if (slot->connect_pending && !utp_connection_is_connected(&slot->connection)) {
        utp_context_report_connect_error(context, status, reason, &slot->connect_attempt);
    } else {
        utp_context_report_connection_error(context, slot, status, 0u, (const uint8_t*)reason, reason_length, false);
    }
    utp_context_release_connection_slot(slot);
}

static bool utp_context_has_pending_udp_write(const utp_context_t* context)
{
    size_t index;

    if (context == NULL) {
        return false;
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        const utp_context_connection_slot_t* slot = &context->connections[index];

        if (slot->used && slot->connection.udp_write_pending) {
            return true;
        }
    }
    return false;
}

static utp_internal_error_t utp_context_enable_udp_write_event(utp_context_t* context)
{
    if (context == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (context->udp_write_event.active) {
        return UTP_INTERNAL_ERROR_OK;
    }
    return utp_event_add_udp(&context->event_loop, &context->udp_write_event, &context->udp_socket, UTP_EVENT_WRITABLE,
                             true, utp_context_on_udp_writable, context);
}

static void utp_context_disable_udp_write_event_if_idle(utp_context_t* context)
{
    if (context != NULL && !utp_context_has_pending_udp_write(context)) {
        utp_event_remove(&context->udp_write_event);
    }
}

static utp_internal_error_t utp_context_flush_connection(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    utp_connection_t* connection;

    if (context == NULL || slot == NULL || !slot->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection = &slot->connection;
    utp_send_control_pacer_tick_in(&connection->send_control, utp_context_now_us());
    for (;;) {
        utp_packet_out_t*    packet = utp_connection_next_packet_to_send_at(connection, utp_context_now_us());
        utp_internal_error_t error;

        if (packet == NULL) {
            connection->udp_write_pending = false;
            utp_send_control_pacer_tick_out(&connection->send_control);
            return UTP_INTERNAL_ERROR_OK;
        }
        error = utp_context_send_packet(context, packet->has_destination ? &packet->destination : &connection->peer,
                                        packet);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_on_packet_sent(connection, packet, utp_context_now_us());
        } else if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            if (utp_connection_is_close_packet(connection, packet)) {
                connection->udp_write_pending = true;
                error                         = utp_context_enable_udp_write_event(context);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    utp_send_control_pacer_tick_out(&connection->send_control);
                    return UTP_INTERNAL_ERROR_OK;
                }
                utp_send_control_pacer_tick_out(&connection->send_control);
                utp_context_report_terminal_send_error(context, slot, error, "failed to wait for udp writable");
                return error;
            }
            error = utp_send_control_reschedule_packet(&connection->send_control, packet);
            if (error == UTP_INTERNAL_ERROR_OK) {
                connection->udp_write_pending = true;
                error                         = utp_context_enable_udp_write_event(context);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                utp_send_control_pacer_tick_out(&connection->send_control);
                return UTP_INTERNAL_ERROR_OK;
            }
            utp_send_control_pacer_tick_out(&connection->send_control);
            utp_context_report_terminal_send_error(context, slot, error, "failed to wait for udp writable");
            return error;
        } else {
            const bool close_packet = utp_connection_is_close_packet(connection, packet);

            if (!close_packet) {
                utp_connection_on_packet_send_error(connection, packet, error, utp_context_now_us());
                utp_connection_on_packet_abandoned(connection, packet);
                utp_packet_out_pool_release(&connection->packet_pool, packet);
            }
            if (error == UTP_INTERNAL_ERROR_NOBUFS || close_packet) {
                const char* reason =
                    error == UTP_INTERNAL_ERROR_NOBUFS ? "udp send ENOBUFS" : "send connection close failed";

                utp_send_control_pacer_tick_out(&connection->send_control);
                utp_context_report_terminal_send_error(context, slot, error, reason);
                return error;
            }
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_send_control_pacer_tick_out(&connection->send_control);
            return error;
        }
    }
}

utp_internal_error_t utp_context_flush_public_connection(utp_context_t* context, utp_connection_t* connection)
{
    size_t index;

    if (context == NULL || connection == NULL || connection->context != context) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        utp_context_connection_slot_t* slot = &context->connections[index];
        utp_internal_error_t           error;

        if (!slot->used || &slot->connection != connection) {
            continue;
        }
        error = utp_context_flush_connection(context, slot);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        return utp_context_refresh_timer(context, utp_context_now_us());
    }
    return UTP_INTERNAL_ERROR_NOT_FOUND;
}

static utp_internal_error_t utp_context_encode_version_frame(uint8_t* buffer, size_t capacity, size_t* out_length)
{
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

static utp_internal_error_t utp_context_send_pending_packet(utp_context_t* context, utp_pending_incoming_t* pending,
                                                            uint8_t packet_type, const uint8_t* payload,
                                                            size_t payload_length, uint64_t packet_number)
{
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

static utp_internal_error_t utp_context_send_pending_handshake(utp_context_t* context, utp_pending_incoming_t* pending,
                                                               uint64_t* out_packet_number)
{
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

static void utp_context_send_pending_close(utp_context_t* context, utp_pending_incoming_t* pending, uint16_t error_code)
{
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

static void utp_context_report_connected(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (!slot->connected_reported && utp_connection_is_connected(&slot->connection)) {
        slot->connect_pending     = false;
        slot->connect_deadline_us = 0u;
        slot->connected_reported  = true;
        if (context->on_connected != NULL) {
            context->on_connected(&slot->connection, context->on_connected_user_data);
        }
    }
}

static void utp_context_report_connection_error(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                utp_status_t status, uint16_t peer_error_code, const uint8_t* reason,
                                                size_t reason_length, bool peer_initiated)
{
    if (!slot->connection_error_reported) {
        const utp_connection_error_info_t info = {
            status, peer_error_code, reason, reason_length, peer_initiated,
        };

        slot->connection_error_reported = true;
        if (context->on_connection_error != NULL) {
            context->on_connection_error(&slot->connection, &info, context->on_connection_error_user_data);
        }
    }
}

static void utp_context_report_connect_error(utp_context_t* context, utp_status_t status, const char* message,
                                             const utp_connect_attempt_info_t* attempt)
{
    if (context->on_connect_error == NULL) {
        return;
    }
    context->on_connect_error(status, message == NULL ? utp_status_string(status) : message, attempt,
                              context->on_connect_error_user_data);
}

static uint64_t utp_context_connect_deadline(uint64_t now_us, uint32_t timeout_ms)
{
    const uint64_t timeout_us = (uint64_t)timeout_ms * UINT64_C(1000);

    return timeout_us > UINT64_MAX - now_us ? UINT64_MAX : now_us + timeout_us;
}

static utp_internal_error_t utp_context_start_connect_attempt(utp_context_t*                 context,
                                                              utp_context_connection_slot_t* slot,
                                                              const utp_address_t* peer, uint64_t now_us)
{
    uint8_t              payload[UTP_FRAME_VERSION_SIZE];
    size_t               payload_length;
    uint32_t             local_cid;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || peer == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_context_alloc_cid(context, &local_cid);
    if (error == UTP_INTERNAL_ERROR_OK && slot->connection.local_cid != 0u) {
        utp_connection_cleanup(&slot->connection);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_ACTIVE, local_cid, 0u, peer,
                                    UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connection.context = context;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_connection_set_mtu_config(&slot->connection, &context->mtu_config);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_set_stream_scheduler_mode(&slot->connection, (uint8_t)context->stream_scheduler_mode);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_encode_version_frame(payload, sizeof(payload), &payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_INITIAL, payload, payload_length, true);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_flush_connection(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connect_deadline_us = utp_context_connect_deadline(now_us, slot->connect_attempt.timeout_ms);
    }
    return error;
}

static void utp_context_fail_pending_connect(utp_context_t* context, utp_context_connection_slot_t* slot,
                                             utp_status_t status, const char* message)
{
    utp_context_report_connect_error(context, status, message, &slot->connect_attempt);
    utp_context_release_connection_slot(slot);
}

static utp_internal_error_t utp_context_send_handshake_done(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    uint8_t                    payload[UTP_FRAME_HANDSHAKE_DONE_SIZE];
    utp_connection_t*          connection;
    utp_frame_handshake_done_t done;
    utp_internal_error_t       error;

    if (context == NULL || slot == NULL || !slot->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection                       = &slot->connection;
    done.ack_handshake_packet_number = connection->peer_handshake_packet_number;

    error = utp_frame_handshake_done_encode(payload, sizeof(payload), &done);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, payload, sizeof(payload), false);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_flush_connection(context, slot);
    }
    return error;
}

static utp_internal_error_t utp_context_queue_ack_if_due(utp_connection_t* connection, uint64_t now_us)
{
    const uint64_t deadline = utp_connection_ack_deadline(connection);

    if (utp_connection_ack_pending_count(connection) == 0u || (deadline != 0u && deadline > now_us)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    /* Keep the ACK pending until flush chooses whether it can share a STREAM packet. */
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_context_timer_callback(uint32_t events, void* user_data);

static void utp_context_take_deadline(uint64_t* deadline, uint64_t candidate)
{
    if (candidate != 0u && (*deadline == 0u || candidate < *deadline)) {
        *deadline = candidate;
    }
}

static uint64_t utp_context_next_deadline(const utp_context_t* context, uint64_t now_us)
{
    uint64_t deadline = 0u;
    size_t   index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        const utp_connection_t* connection = &context->connections[index].connection;

        if (!context->connections[index].used) {
            continue;
        }
        if (connection->state == UTP_CONNECTION_STATE_CLOSING || connection->state == UTP_CONNECTION_STATE_DRAINING) {
            utp_context_take_deadline(&deadline, utp_connection_close_deadline(connection));
            if (context->connections[index].connect_pending && !utp_connection_is_connected(connection)) {
                utp_context_take_deadline(&deadline, now_us);
            }
            continue;
        }
        if (utp_connection_ack_pending_count(connection) != 0u) {
            const uint64_t ack_deadline = utp_connection_ack_deadline(connection);

            utp_context_take_deadline(&deadline, ack_deadline == 0u ? now_us : ack_deadline);
        }
        utp_context_take_deadline(&deadline, utp_connection_retransmission_deadline(connection));
        utp_context_take_deadline(&deadline, utp_connection_close_deadline(connection));
        utp_context_take_deadline(&deadline, utp_connection_keepalive_deadline(connection));
        utp_context_take_deadline(&deadline, utp_connection_mtu_deadline(connection, now_us));
        utp_context_take_deadline(&deadline, utp_connection_pacing_deadline(connection));
        utp_context_take_deadline(&deadline, utp_connection_path_validation_deadline(connection));
        if (context->connections[index].connect_pending) {
            utp_context_take_deadline(&deadline, context->connections[index].connect_deadline_us);
        }
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        if (context->pending_incoming[index].used) {
            utp_context_take_deadline(
                &deadline, utp_pending_incoming_handshake_deadline(&context->pending_incoming[index].pending));
        }
    }
    return deadline;
}

static utp_internal_error_t utp_context_refresh_timer(utp_context_t* context, uint64_t now_us)
{
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

static utp_internal_error_t utp_context_replay_pending_packet(const uint8_t* packet, size_t packet_length,
                                                              void* user_data)
{
    utp_context_replay_t* replay = user_data;
    utp_packet_in_t*      packet_in;
    utp_internal_error_t  error;

    if (replay == NULL || replay->context == NULL || packet == NULL || packet_length > UINT16_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_packet_in_pool_acquire(&replay->context->packet_in_pool, &packet_in);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    memcpy(packet_in->data, packet, packet_length);
    packet_in->length = (uint16_t)packet_length;
    error = utp_connection_on_packet_in_received(replay->connection, packet_in, replay->peer, replay->now_us);
    utp_packet_in_release(packet_in);
    return error;
}

static utp_internal_error_t utp_context_promote_pending(utp_context_t*              context,
                                                        utp_context_pending_slot_t* pending_slot, const uint8_t* packet,
                                                        size_t packet_length, utp_packet_in_t* packet_in,
                                                        const utp_address_t* peer)
{
    utp_context_connection_slot_t* slot;
    utp_context_replay_t           replay;
    uint64_t                       now_us;
    utp_internal_error_t           error;

    slot = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_PASSIVE, pending_slot->pending.local_cid,
                                pending_slot->pending.peer_cid, peer, UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connection.context = context;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_connection_set_mtu_config(&slot->connection, &context->mtu_config);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_set_stream_scheduler_mode(&slot->connection, (uint8_t)context->stream_scheduler_mode);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(slot);
        return error;
    }
    now_us = utp_context_now_us();
    if (packet_in != NULL) {
        error = utp_connection_on_packet_in_received(&slot->connection, packet_in, peer, now_us);
    } else {
        error = utp_connection_on_packet_received(&slot->connection, packet, packet_length, peer, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_queue_ack_if_due(&slot->connection, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        replay.context    = context;
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
    return utp_context_flush_connection(context, slot);
}

static utp_internal_error_t utp_context_on_connection_packet(utp_context_t*                 context,
                                                             utp_context_connection_slot_t* slot,
                                                             const utp_packet_view_t* view, const uint8_t* packet,
                                                             size_t packet_length, utp_packet_in_t* packet_in,
                                                             const utp_address_t* peer)
{
    utp_internal_error_t error;
    uint64_t             now_us;

    now_us = utp_context_now_us();
    if (packet_in != NULL) {
        error = utp_connection_on_packet_in_received(&slot->connection, packet_in, peer, now_us);
    } else {
        error = utp_connection_on_packet_received(&slot->connection, packet, packet_length, peer, now_us);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        if (utp_context_is_peer_protocol_error(error)) {
            return utp_context_close_on_peer_protocol_error(context, slot, error);
        }
        return error;
    }
    if (slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE && view->header.type == UTP_PACKET_TYPE_HANDSHAKE &&
        slot->connection.peer_handshake_packet_number == view->header.packet_number) {
        error = utp_context_send_handshake_done(context, slot);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    error = utp_context_queue_ack_if_due(&slot->connection, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_context_report_connected(context, slot);
    if (!slot->connect_pending && slot->connection.peer_close_received && !slot->connection.local_close_started) {
        const utp_status_t status = slot->connection.peer_close_error_code == 0u ? UTP_STATUS_OK : UTP_STATUS_CLOSED;

        utp_context_report_connection_error(context, slot, status, slot->connection.peer_close_error_code,
                                            slot->connection.peer_close_reason,
                                            slot->connection.peer_close_reason_length, true);
        slot->connection.peer_close_reason        = NULL;
        slot->connection.peer_close_reason_length = 0u;
    }
    return utp_context_flush_connection(context, slot);
}

static utp_internal_error_t utp_context_on_pending_packet(utp_context_t* context, utp_context_pending_slot_t* slot,
                                                          const uint8_t* packet, size_t packet_length,
                                                          utp_packet_in_t* packet_in, const utp_address_t* peer)
{
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
        error = utp_context_promote_pending(context, slot, packet, packet_length, packet_in, peer);
    }
    return error;
}

static utp_internal_error_t utp_context_on_initial_packet(utp_context_t* context, const utp_packet_view_t* view,
                                                          const utp_address_t* peer)
{
    utp_context_pending_slot_t* slot;
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

static utp_internal_error_t utp_context_dispatch_packet(utp_context_t* context, const uint8_t* packet,
                                                        size_t packet_length, utp_packet_in_t* packet_in,
                                                        const utp_address_t* peer)
{
    utp_packet_view_t              view;
    utp_context_connection_slot_t* connection_slot;
    utp_context_pending_slot_t*    pending_slot;
    utp_internal_error_t           error;

    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + view.payload_length ||
        view.header.packet_number == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    connection_slot = utp_context_find_connection_slot(context, view.header.dcid);
    if (connection_slot != NULL) {
        return utp_context_on_connection_packet(context, connection_slot, &view, packet, packet_length, packet_in,
                                                peer);
    }
    pending_slot = utp_context_find_pending_slot(context, view.header.dcid);
    if (pending_slot != NULL) {
        return utp_context_on_pending_packet(context, pending_slot, packet, packet_length, packet_in, peer);
    }
    if (view.header.dcid == 0u && view.header.type == UTP_PACKET_TYPE_INITIAL) {
        return utp_context_on_initial_packet(context, &view, peer);
    }
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_context_on_udp_readable(uint32_t events, void* user_data)
{
    utp_context_t* context = user_data;

    if ((events & UTP_EVENT_READABLE) == 0u || context == NULL) {
        return;
    }
    for (;;) {
        size_t               received_length = 0u;
        utp_address_t        peer;
        utp_packet_in_t*     packet_in = NULL;
        utp_internal_error_t error;

        error = utp_packet_in_pool_acquire(&context->packet_in_pool, &packet_in);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_udp_socket_recv_from(&context->udp_socket, packet_in->data, packet_in->capacity,
                                             &received_length, &peer);
            if (error == UTP_INTERNAL_ERROR_OK) {
                packet_in->length = (uint16_t)received_length;
                error = utp_context_dispatch_packet(context, packet_in->data, packet_in->length, packet_in, &peer);
            }
            utp_packet_in_release(packet_in);
        }

        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            return;
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

static void utp_context_on_udp_writable(uint32_t events, void* user_data)
{
    utp_context_t* context = user_data;
    size_t         index;

    if ((events & UTP_EVENT_WRITABLE) == 0u || context == NULL) {
        return;
    }
    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        utp_context_connection_slot_t* slot = &context->connections[index];
        utp_internal_error_t           error;

        if (!slot->used || !slot->connection.udp_write_pending) {
            continue;
        }
        error = utp_context_flush_connection(context, slot);
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, error, "udp close retry failed");
        }
    }
    utp_context_disable_udp_write_event_if_idle(context);
    {
        const utp_internal_error_t error = utp_context_refresh_timer(context, utp_context_now_us());

        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, error, "context timer refresh failed");
        }
    }
}

static utp_internal_error_t utp_context_process_connection_timers(utp_context_t* context, uint64_t now_us)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
        utp_context_connection_slot_t* slot = &context->connections[index];
        uint64_t                       deadline;
        utp_internal_error_t           error;

        if (!slot->used) {
            continue;
        }
        if (slot->connect_pending && !utp_connection_is_connected(&slot->connection) &&
            ((utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_CLOSING ||
              utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_DRAINING) ||
             (slot->connect_deadline_us != 0u && slot->connect_deadline_us <= now_us))) {
            const bool closed_during_handshake =
                utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_CLOSING ||
                utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_DRAINING;

            if (slot->connect_retries_remaining > 0) {
                const utp_address_t peer = slot->connection.peer;

                --slot->connect_retries_remaining;
                error = utp_context_start_connect_attempt(context, slot, &peer, now_us);
                if (error != UTP_INTERNAL_ERROR_OK && slot->used) {
                    utp_context_fail_pending_connect(context, slot, utp_internal_error_to_status(error),
                                                     "connect retry failed");
                }
            } else {
                utp_context_fail_pending_connect(
                    context, slot, closed_during_handshake ? UTP_STATUS_CANCELLED : UTP_STATUS_TIMEOUT,
                    closed_during_handshake ? "connection closed during handshake" : "connect timeout");
            }
            continue;
        }
        if (utp_connection_close_deadline(&slot->connection) != 0u &&
            utp_connection_close_deadline(&slot->connection) <= now_us) {
            utp_context_release_connection_slot(slot);
            continue;
        }
        error = utp_connection_on_keepalive_timeout(&slot->connection, now_us);
        if (error == UTP_INTERNAL_ERROR_TIMEOUT) {
            static const uint8_t timeout_reason[] = "keepalive timeout";

            utp_context_report_connection_error(context, slot, UTP_STATUS_TIMEOUT, 0u, timeout_reason,
                                                sizeof(timeout_reason) - 1u, false);
            utp_context_release_connection_slot(slot);
            continue;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_on_mtu_timeout(&slot->connection, now_us);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_on_path_validation_timeout(&slot->connection, now_us);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_queue_ack_if_due(&slot->connection, now_us);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_flush_connection(context, slot);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        deadline = utp_connection_retransmission_deadline(&slot->connection);
        if (deadline != 0u && deadline <= now_us) {
            error = utp_connection_on_retransmission_timeout(&slot->connection, now_us);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_context_flush_connection(context, slot);
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

static utp_internal_error_t utp_context_process_pending_timers(utp_context_t* context, uint64_t now_us)
{
    size_t index;

    for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
        utp_context_pending_slot_t* slot = &context->pending_incoming[index];
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

static void utp_context_timer_callback(uint32_t events, void* user_data)
{
    utp_context_t*       context = user_data;
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

utp_status_t utp_context_create(const utp_context_options_t* options, utp_context_t** out_context)
{
    utp_context_t*       context;
    utp_internal_error_t error;
    char                 fragment[32];
    int32_t              fragment_length;

    if (out_context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    *out_context = NULL;
    if (options == NULL || options->event_base == NULL || options->stream_scheduler_mode > UTP_STREAM_SCHEDULER_DRR) {
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
    utp_event_init(&context->udp_write_event);
    utp_event_init(&context->timer_event);
    utp_udp_socket_init(&context->udp_socket);
    context->packet_in_pool.allocator            = NULL;
    context->packet_in_pool.packets              = NULL;
    context->packet_in_pool.storage              = NULL;
    context->packet_in_pool.packet_capacity      = 0u;
    context->packet_in_pool.buffer_capacity      = 0u;
    context->on_connected                        = NULL;
    context->on_connected_user_data              = NULL;
    context->on_connect_error                    = NULL;
    context->on_connect_error_user_data          = NULL;
    context->on_new_connection                   = NULL;
    context->on_new_connection_user_data         = NULL;
    context->on_connection_error                 = NULL;
    context->on_connection_error_user_data       = NULL;
    context->next_cid                            = (uint32_t)options->context_id;
    context->next_cid                            = context->next_cid == 0u ? 1u : context->next_cid;
    context->stream_scheduler_mode               = options->stream_scheduler_mode;
    context->mtu_config.enabled                  = options->enable_dplpmtud;
    context->mtu_config.mtu_min                  = options->mtu_min;
    context->mtu_config.mtu_max                  = options->mtu_max;
    context->mtu_config.mtu_base                 = options->mtu_base;
    context->mtu_config.probe_interval_seconds   = options->mtu_probe_interval;
    context->mtu_config.probe_step               = options->mtu_probe_step;
    context->mtu_config.probe_timeout_ms         = options->mtu_probe_timeout;
    context->mtu_config.probe_retries            = options->mtu_probe_retries;
    context->mtu_config.blackhole_loss_threshold = options->mtu_blackhole_loss_threshold;
    context->mtu_config.blackhole_loss_window_ms = options->mtu_blackhole_loss_window_ms;
    context->mtu_config.blackhole_cooldown_ms    = options->mtu_blackhole_cooldown_ms;
    context->logger.sink                         = options->log_sink;
    fragment_length = snprintf(fragment, sizeof(fragment), "context %" PRIu64, options->context_id);
    if (fragment_length < 0 || (size_t)fragment_length >= sizeof(fragment)) {
        utp_allocator_free(NULL, context);
        return UTP_STATUS_OVERFLOW;
    }
    error = utp_log_tag_init(&context->tag, fragment, (size_t)fragment_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_event_loop_init(&context->event_loop, options->event_base, &context->logger, &context->tag);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_packet_in_pool_init(&context->packet_in_pool, NULL, UTP_CONTEXT_PACKET_IN_LIMIT,
                                        UTP_CONTEXT_PACKET_IN_CAPACITY);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context initialization failed");
        utp_packet_in_pool_cleanup(&context->packet_in_pool);
        utp_event_loop_close(&context->event_loop);
        utp_allocator_free(NULL, context);
        return utp_internal_error_to_status(error);
    }
    *out_context = context;
    return UTP_STATUS_OK;
}

void utp_context_destroy(utp_context_t* context)
{
    if (context != NULL) {
        size_t index;

        utp_event_remove(&context->timer_event);
        utp_event_remove(&context->udp_write_event);
        utp_event_remove(&context->udp_event);
        for (index = 0u; index < UTP_CONTEXT_MAX_CONNECTIONS; ++index) {
            utp_context_send_destroy_close(context, &context->connections[index]);
            utp_context_release_connection_slot(&context->connections[index]);
        }
        for (index = 0u; index < UTP_CONTEXT_MAX_PENDING_INCOMING; ++index) {
            utp_context_release_pending_slot(&context->pending_incoming[index]);
        }
        utp_udp_socket_close(&context->udp_socket);
        utp_packet_in_pool_cleanup(&context->packet_in_pool);
        utp_event_loop_close(&context->event_loop);
        utp_allocator_free(NULL, context);
    }
}

utp_status_t utp_context_bind(utp_context_t* context, const char* address, uint16_t port, const char* ifname,
                              uint16_t* out_port)
{
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

void utp_context_set_on_connected(utp_context_t* context, utp_on_connected_fn callback, void* user_data)
{
    if (context != NULL) {
        context->on_connected           = callback;
        context->on_connected_user_data = user_data;
    }
}

void utp_context_set_on_connect_error(utp_context_t* context, utp_on_connect_error_fn callback, void* user_data)
{
    if (context != NULL) {
        context->on_connect_error           = callback;
        context->on_connect_error_user_data = user_data;
    }
}

void utp_context_set_on_new_connection(utp_context_t* context, utp_on_new_connection_fn callback, void* user_data)
{
    if (context != NULL) {
        context->on_new_connection           = callback;
        context->on_new_connection_user_data = user_data;
    }
}

void utp_context_set_on_connection_error(utp_context_t* context, utp_on_connection_error_fn callback, void* user_data)
{
    if (context != NULL) {
        context->on_connection_error           = callback;
        context->on_connection_error_user_data = user_data;
    }
}

utp_status_t utp_context_connect(utp_context_t* context, const utp_connect_options_t* options)
{
    utp_address_t                  peer;
    utp_context_connection_slot_t* slot;
    utp_context_connection_slot_t* existing;
    utp_internal_error_t           error;
    utp_status_t                   status;
    uint64_t                       now_us;

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
    utp_context_endpoint_from_address(&slot->connect_attempt.remote, &peer);
    slot->connect_attempt.timeout_ms            = options->timeout_ms == 0u ? 3000u : options->timeout_ms;
    slot->connect_attempt.retries               = options->retries;
    slot->connect_attempt.encryption            = options->encryption;
    slot->connect_attempt.type                  = UTP_CONNECT_ATTEMPT_NORMAL;
    slot->connect_attempt.session_token_size    = 0u;
    slot->connect_attempt.resumption_state_size = 0u;
    slot->connect_attempt.early_data_size       = 0u;
    slot->connect_attempt.early_fin             = false;
    slot->connect_retries_remaining             = options->retries < 0 ? 0 : options->retries;
    slot->connect_pending                       = true;
    now_us                                      = utp_context_now_us();
    error                                       = utp_context_start_connect_attempt(context, slot, &peer, now_us);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_refresh_timer(context, now_us);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        status = utp_internal_error_to_status(error);
        if (slot->used) {
            utp_context_fail_pending_connect(context, slot, status, "active connect failed");
        }
        return status;
    }
    return UTP_STATUS_OK;
}

utp_status_t utp_context_accept(utp_context_t* context)
{
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
        utp_context_pending_slot_t* slot = &context->pending_incoming[index];

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
            utp_connect_attempt_info_t attempt = {0};

            utp_context_send_pending_close(context, &slot->pending, (uint16_t)(-UTP_STATUS_IO));
            utp_context_endpoint_from_address(&attempt.remote, &slot->pending.peer);
            attempt.encryption = UTP_ENCRYPTION_NONE;
            attempt.type       = UTP_CONNECT_ATTEMPT_PASSIVE;
            utp_context_report_connect_error(context, utp_internal_error_to_status(error), "passive accept failed",
                                             &attempt);
            utp_context_release_pending_slot(slot);
            return utp_internal_error_to_status(error);
        }
        return UTP_STATUS_OK;
    }
    return UTP_STATUS_WOULD_BLOCK;
}
