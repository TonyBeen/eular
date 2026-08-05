#include "connection/connection.h"

#include <errno.h>
#include <string.h>

#include <openssl/rand.h>

#include "mtu/mtu.h"
#include "util/allocator.h"

// 协议帧调度策略
#define UTP_CONNECTION_RETRANSMITTABLE_FRAMES                                      \
    ((UTP_FRAME_BIT(UTP_FRAME_TYPE_MAX) - 1u) &                                    \
     ~(UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) | UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING) | \
       UTP_FRAME_BIT(UTP_FRAME_TYPE_PING)))

// ACK 与流量控制策略
#define UTP_CONNECTION_ACK_ELICITING_THRESHOLD      2u
#define UTP_CONNECTION_ACK_REORDER_THRESHOLD        1u
#define UTP_CONNECTION_MAX_ACK_DELAY_MS             25u
#define UTP_CONNECTION_DEFAULT_FLOW_WINDOW          (UTP_STREAM_DEFAULT_FLOW_WINDOW * 4u)
#define UTP_CONNECTION_FLOW_UPDATE_DIVISOR          10u
#define UTP_CONNECTION_FLOW_UPDATE_MIN_INTERVAL_US  UINT64_C(20000)
#define UTP_CONNECTION_FLOW_BLOCKED_MIN_INTERVAL_US UINT64_C(50000)

// 关闭状态机参数
#define UTP_CONNECTION_CLOSE_PTO_DEFAULT_US UINT64_C(333333)    // 默认 333 毫秒
#define UTP_CONNECTION_CLOSE_PTO_MIN_US     UINT64_C(10000)     // 最小 10 毫秒
#define UTP_CONNECTION_CLOSE_PTO_MAX_US     UINT64_C(60000000)  // 最大 60 秒

// 路径验证参数
#define UTP_CONNECTION_PATH_CHALLENGE_TIMEOUT_US    UINT64_C(1500000) // 路径验证期间等待 PATH_RESPONSE 的超时时间(us)
#define UTP_CONNECTION_PATH_CHALLENGE_MAX_RETRIES   3u // 路径验证期间允许的最大重试次数
#define UTP_CONNECTION_PATH_VALIDATION_SEND_CREDIT  (UINT64_C(3) * UTP_PACKET_MTU_FLOOR) // 路径验证期间允许发送的最大字节数

// 本端默认流额度
#define UTP_CONNECTION_DEFAULT_MAX_STREAMS_BIDI 64u  // 默认双向流可创建数量
#define UTP_CONNECTION_DEFAULT_MAX_STREAMS_UNI  32u  // 默认单向流可创建数量

void        utp_connection_on_packet_abandoned(utp_connection_t* connection, const utp_packet_out_t* packet);
static void utp_connection_reclaim_closed_stream_slots(utp_connection_t* connection);
static void utp_connection_update_completed_peer_streams(utp_connection_t* connection);

/* 收包基础校验与明文握手保护。 */
static bool utp_connection_packet_type_is_valid(uint8_t type)
{
    return type >= UTP_PACKET_TYPE_INITIAL && type <= UTP_PACKET_TYPE_CONNECT;
}

static utp_internal_error_t utp_connection_untrusted_packet_error(const utp_connection_t* connection)
{
    return connection != NULL && connection->crypto_configured ? UTP_INTERNAL_ERROR_AUTH : UTP_INTERNAL_ERROR_PROTOCOL;
}

static bool utp_connection_is_handshake_frame(uint8_t frame_type)
{
    return frame_type == UTP_FRAME_TYPE_PADDING || frame_type == UTP_FRAME_TYPE_CRYPTO ||
           frame_type == UTP_FRAME_TYPE_ACK_FREQUENCY || frame_type == UTP_FRAME_TYPE_VERSION ||
           frame_type == UTP_FRAME_TYPE_TRANSPORT_PARAMS || frame_type == UTP_FRAME_TYPE_HANDSHAKE_DELAY;
}

// Initial/Handshake 是明文协商包，必须在进入通用帧处理前完成白名单校验。
static utp_internal_error_t utp_connection_validate_plaintext_handshake(const utp_connection_t*  connection,
                                                                        const utp_packet_view_t* view)
{
    bool   has_crypto = false;
    size_t offset     = 0u;

    if (connection == NULL || view == NULL || view->header.type != UTP_PACKET_TYPE_HANDSHAKE ||
        connection->role != UTP_CONNECTION_ROLE_ACTIVE ||
        (connection->state != UTP_CONNECTION_STATE_INITIAL_SENT &&
         connection->state != UTP_CONNECTION_STATE_CONNECTED)) {
        return utp_connection_untrusted_packet_error(connection);
    }
    while (offset < view->payload_length) {
        const uint8_t*       frame;
        uint8_t              frame_type;
        size_t               frame_length;
        utp_internal_error_t error = utp_packet_view_next_frame(view, &offset, &frame_type, &frame, &frame_length);

        if (error != UTP_INTERNAL_ERROR_OK || !utp_connection_is_handshake_frame(frame_type)) {
            return utp_connection_untrusted_packet_error(connection);
        }
        if (frame_type == UTP_FRAME_TYPE_CRYPTO) {
            utp_frame_crypto_t crypto;

            if (has_crypto || utp_frame_crypto_decode(&crypto, frame, frame_length) != UTP_INTERNAL_ERROR_OK ||
                !connection->crypto_configured || crypto.crypto_type != connection->crypto_type ||
                (connection->crypto_ready && memcmp(connection->peer_crypto_public_key, crypto.ephemeral_public_key,
                                                    sizeof(connection->peer_crypto_public_key)) != 0)) {
                return utp_connection_untrusted_packet_error(connection);
            }
            has_crypto = true;
        }
    }
    if (connection->crypto_configured != has_crypto) {
        return utp_connection_untrusted_packet_error(connection);
    }
    return UTP_INTERNAL_ERROR_OK;
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

/* 发包编码、关闭状态机与发送资格判定。 */
static utp_internal_error_t utp_connection_encode_header(utp_connection_t* connection, utp_packet_out_t* packet,
                                                         uint8_t packet_type)
{
    const utp_packet_header_t header = {
        connection->local_cid, connection->peer_cid,
        packet->packet_number, (uint16_t)(packet->data_size - UTP_PACKET_HEADER_SIZE),
        packet_type,           0u,
    };

    utp_internal_error_t error = utp_proto_encode_header(packet->raw_data, packet->alloc_size, &header);

    if (error == UTP_INTERNAL_ERROR_OK && connection->crypto_ready && packet_type != UTP_PACKET_TYPE_INITIAL &&
        packet_type != UTP_PACKET_TYPE_HANDSHAKE && (packet->po_flags & UTP_PO_NO_ENCRYPT) == 0u) {
        if ((size_t)packet->data_size + UTP_CRYPTO_AEAD_TAG_SIZE > UINT16_MAX) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        packet->po_flags          |= UTP_PO_ENCRYPTED | UTP_PO_KEEP_PLAINTEXT;
        packet->encrypt_data_size  = (uint16_t)(packet->data_size + UTP_CRYPTO_AEAD_TAG_SIZE);
    }
    return error;
}

bool utp_connection_is_close_packet(const utp_connection_t* connection, const utp_packet_out_t* packet)
{
    return connection != NULL && packet == &connection->close_packet;
}

static void utp_connection_reset_close_packet(utp_connection_t* connection)
{
    utp_packet_out_t* packet = &connection->close_packet;

    packet->sent_time_us               = 0u;
    packet->packet_number              = 0u;
    packet->ack_number                 = 0u;
    packet->loss_chain                 = packet;
    packet->frame_types                = UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE);
    packet->po_flags                   = 0u;
    packet->local_flags                = UTP_POL_NO_TRACK_ON_SEND;
    packet->data_size                  = 0u;
    packet->encrypt_data_size          = 0u;
    packet->alloc_size                 = sizeof(connection->close_packet_data);
    packet->packet_type                = UTP_PACKET_TYPE_CONNECTION_CLOSE;
    packet->slice_count                = 1u;
    packet->frame_meta_count           = 0u;
    packet->stream_data_size           = 0u;
    packet->path_validation_generation = 0u;
    packet->transient_ack_size         = 0u;
    packet->control_prefix_size        = 0u;
    packet->stream_id                  = 0u;
    packet->stream_offset              = 0u;
    packet->bw_packet_state.valid      = false;
    packet->bw_state                   = NULL;
    packet->raw_data                   = connection->close_packet_data;
    packet->encrypt_data               = connection->close_packet_data;
    packet->has_destination            = false;
    packet->bucket_index               = 0u;
    packet->slices[0].source           = UTP_PACKET_OUT_SLICE_RAW_OFFSET;
    packet->slices[0].offset           = 0u;
    packet->slices[0].length           = 0u;
    packet->slices[0].data             = NULL;
    utp_packet_out_clear_send_attempts(packet);
}

static utp_internal_error_t utp_connection_prepare_close_packet(utp_connection_t* connection, uint16_t error_code,
                                                                bool local_close_started)
{
    const utp_frame_connection_close_t close = {error_code, NULL, 0u};
    utp_packet_out_t*                  packet;
    uint64_t                           packet_number;
    size_t                             payload_length;
    utp_internal_error_t               error;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    packet = &connection->close_packet;
    // close_packet 是连接内的专用存储，不进入 PacketOut 池，确保池耗尽时仍能关闭连接。
    utp_connection_reset_close_packet(connection);
    error = utp_frame_connection_close_encode(packet->raw_data + UTP_PACKET_HEADER_SIZE,
                                              packet->alloc_size - UTP_PACKET_HEADER_SIZE, &close);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    payload_length = UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE;
    error          = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    packet->packet_number    = packet_number;
    packet->data_size        = (uint16_t)(UTP_PACKET_HEADER_SIZE + payload_length);
    packet->slices[0].length = packet->data_size;
    error                    = utp_connection_encode_header(connection, packet, UTP_PACKET_TYPE_CONNECTION_CLOSE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        connection->state            = UTP_CONNECTION_STATE_CLOSING;
        connection->close_error_code = error_code;
        connection->close_pending    = true;
        if (local_close_started) {
            connection->local_close_started = true;
        }
        connection->retransmission_deadline_us = 0u;
        utp_send_control_set_connected(&connection->send_control, false);
    }
    return error;
}

static uint64_t utp_connection_close_pto(const utp_connection_t* connection)
{
    uint64_t pto = utp_send_control_srtt(&connection->send_control);

    if (pto == 0u) {
        pto = UTP_CONNECTION_CLOSE_PTO_DEFAULT_US;
    }
    if (pto < UTP_CONNECTION_CLOSE_PTO_MIN_US) {
        return UTP_CONNECTION_CLOSE_PTO_MIN_US;
    }
    return pto > UTP_CONNECTION_CLOSE_PTO_MAX_US ? UTP_CONNECTION_CLOSE_PTO_MAX_US : pto;
}

static uint64_t utp_connection_add_deadline(uint64_t now_us, uint64_t delay_us)
{
    return now_us > UINT64_MAX - delay_us ? UINT64_MAX : now_us + delay_us;
}

static uint64_t utp_connection_milliseconds_to_microseconds(uint64_t milliseconds)
{
    return milliseconds > UINT64_MAX / UINT64_C(1000) ? UINT64_MAX : milliseconds * UINT64_C(1000);
}

static uint16_t utp_connection_current_packet_capacity(const utp_connection_t* connection)
{
    uint16_t capacity;

    if (connection == NULL) {
        return 0u;
    }
    capacity = connection->state == UTP_CONNECTION_STATE_CONNECTED
                   ? utp_mtu_discovery_current_max_packet_size(&connection->mtu_discovery)
                   : utp_mtu_packet_size_from_mtu(connection->mtu_discovery.mtu_min, connection->peer.family);
    return capacity > connection->packet_capacity ? connection->packet_capacity : capacity;
}

static uint16_t utp_connection_plaintext_packet_capacity(const utp_connection_t* connection)
{
    uint16_t capacity = utp_connection_current_packet_capacity(connection);

    if (connection != NULL && connection->crypto_ready) {
        return capacity > UTP_CRYPTO_AEAD_TAG_SIZE ? (uint16_t)(capacity - UTP_CRYPTO_AEAD_TAG_SIZE) : 0u;
    }
    return capacity;
}

static uint16_t utp_connection_packet_wire_size(const utp_packet_out_t* packet)
{
    return packet != NULL && (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size
                                                                         : packet->data_size;
}

static bool utp_connection_packet_bypasses_congestion(const utp_packet_out_t* packet)
{
    if (packet == NULL) {
        return false;
    }
    return packet->packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE ||
           packet->frame_types == UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK) ||
           packet->frame_types == UTP_FRAME_BIT(UTP_FRAME_TYPE_PATH_RESPONSE);
}

static bool utp_connection_can_transmit_packet(utp_connection_t* connection, const utp_packet_out_t* packet)
{
    return connection != NULL && packet != NULL &&
           (utp_connection_packet_bypasses_congestion(packet) ||
            utp_send_control_can_transmit_packet(&connection->send_control, utp_connection_packet_wire_size(packet)));
}

static utp_packet_out_t* utp_connection_next_scheduled_admitted(utp_connection_t* connection)
{
    utp_packet_out_t* packet;

    if (connection == NULL) {
        return NULL;
    }
    while ((packet = utp_send_control_peek_scheduled(&connection->send_control)) != NULL) {
        if ((packet->po_flags & UTP_PO_PATH_VALIDATION) != 0u &&
            packet->path_validation_generation != connection->path_validation_generation) {
            packet = utp_send_control_next_scheduled(&connection->send_control);
            utp_connection_on_packet_abandoned(connection, packet);
            utp_packet_out_pool_release(&connection->packet_pool, packet);
            continue;
        }
        return utp_connection_can_transmit_packet(connection, packet)
                   ? utp_send_control_next_scheduled(&connection->send_control)
                   : NULL;
    }
    return NULL;
}

static void utp_connection_mark_peer_activity(utp_connection_t* connection, uint64_t now_us)
{
    if (connection == NULL || connection->state != UTP_CONNECTION_STATE_CONNECTED || now_us == 0u) {
        return;
    }
    connection->last_peer_activity_us   = now_us;
    connection->keepalive_missed_probes = 0u;
    connection->keepalive_deadline_us   = utp_connection_add_deadline(now_us, UTP_CONNECTION_KEEPALIVE_INTERVAL_US);
}

static bool utp_connection_record_peer_close(utp_connection_t* connection, const utp_frame_connection_close_t* close)
{
    if (connection == NULL || close == NULL || connection->peer_close_received) {
        return false;
    }
    connection->peer_close_error_code    = close->error_code;
    connection->peer_close_reason        = close->reason;
    connection->peer_close_reason_length = close->reason_length;
    connection->peer_close_received      = true;
    return true;
}

static void utp_connection_enter_draining(utp_connection_t* connection, uint64_t now_us)
{
    const uint64_t pto = utp_connection_close_pto(connection);

    // draining 仅保留 CID 用于吸收迟到报文，不再处理帧、发送 ACK 或重传任何数据。
    connection->state                      = UTP_CONNECTION_STATE_DRAINING;
    connection->close_pending              = false;
    connection->close_pto_us               = pto;
    connection->close_deadline_us          = now_us > UINT64_MAX - 3u * pto ? UINT64_MAX : now_us + 3u * pto;
    connection->retransmission_deadline_us = 0u;
    utp_send_control_set_connected(&connection->send_control, false);
}

/* 路径验证。 */
static bool utp_connection_candidate_can_queue(const utp_connection_t* connection, size_t packet_length)
{
    uint64_t limit;

    if (connection == NULL || connection->path_state != UTP_CONNECTION_PATH_STATE_VALIDATING) {
        return false;
    }
    if (connection->candidate_rx_bytes > (UINT64_MAX - UTP_CONNECTION_PATH_VALIDATION_SEND_CREDIT) / UINT64_C(3)) {
        limit = UINT64_MAX;
    } else {
        limit = connection->candidate_rx_bytes * UINT64_C(3) + UTP_CONNECTION_PATH_VALIDATION_SEND_CREDIT;
    }
    return connection->candidate_tx_bytes <= limit &&
           connection->candidate_queued_bytes <= limit - connection->candidate_tx_bytes &&
           (uint64_t)packet_length <= limit - connection->candidate_tx_bytes - connection->candidate_queued_bytes;
}

static void utp_connection_begin_path_validation(utp_connection_t* connection, const utp_address_t* peer,
                                                 size_t received_length)
{
    if (connection->path_validation_generation == UINT32_MAX) {
        connection->path_validation_generation = 1u;
    } else {
        ++connection->path_validation_generation;
    }
    connection->candidate_peer             = *peer;
    connection->candidate_rx_bytes         = (uint64_t)received_length;
    connection->candidate_tx_bytes         = 0u;
    connection->candidate_queued_bytes     = 0u;
    connection->path_challenge_deadline_us = 0u;
    connection->path_challenge_retry_count = 0u;
    connection->path_challenge_pending     = false;
    connection->path_state                 = UTP_CONNECTION_PATH_STATE_VALIDATING;
}

static utp_internal_error_t utp_connection_queue_path_frame(utp_connection_t* connection, uint8_t frame_type,
                                                            const utp_frame_path_t* path,
                                                            const utp_address_t* destination, bool candidate_path)
{
    utp_packet_out_t*    packet = NULL;
    utp_internal_error_t error;
    uint64_t             packet_number;
    const size_t         packet_length = UTP_PACKET_HEADER_SIZE + UTP_FRAME_PATH_SIZE;
    const size_t         wire_packet_length =
        packet_length + (connection != NULL && connection->crypto_ready ? UTP_CRYPTO_AEAD_TAG_SIZE : 0u);

    if (connection == NULL || path == NULL || destination == NULL ||
        (frame_type != UTP_FRAME_TYPE_PATH_CHALLENGE && frame_type != UTP_FRAME_TYPE_PATH_RESPONSE) ||
        connection->state == UTP_CONNECTION_STATE_DRAINING || connection->state == UTP_CONNECTION_STATE_CLOSED) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (candidate_path && (!utp_address_equal(destination, &connection->candidate_peer) ||
                           !utp_connection_candidate_can_queue(connection, wire_packet_length))) {
        return UTP_INTERNAL_ERROR_PATH_VALIDATION_BLOCKED;
    }
    error = utp_packet_out_pool_acquire(&connection->packet_pool, (uint16_t)packet_length, &packet);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        packet->packet_number    = packet_number;
        packet->data_size        = (uint16_t)packet_length;
        packet->packet_type      = UTP_PACKET_TYPE_CTRL;
        packet->frame_types      = UTP_FRAME_BIT(frame_type);
        packet->slice_count      = 1u;
        packet->slices[0].source = UTP_PACKET_OUT_SLICE_RAW_OFFSET;
        packet->slices[0].offset = 0u;
        packet->slices[0].length = (uint16_t)packet_length;
        packet->slices[0].data   = NULL;
        error = utp_frame_path_encode(packet->raw_data + UTP_PACKET_HEADER_SIZE, UTP_FRAME_PATH_SIZE, frame_type, path);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_encode_header(connection, packet, UTP_PACKET_TYPE_CTRL);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        packet->has_destination = !utp_address_equal(destination, &connection->peer);
        if (packet->has_destination) {
            packet->destination = *destination;
        }
        if (candidate_path) {
            packet->po_flags                   |= UTP_PO_PATH_VALIDATION;
            packet->path_validation_generation  = connection->path_validation_generation;
        }
        error = utp_send_control_schedule_packet(&connection->send_control, packet, false);
    }
    if (error == UTP_INTERNAL_ERROR_OK && candidate_path) {
        connection->candidate_queued_bytes += (uint64_t)wire_packet_length;
    }
    if (error != UTP_INTERNAL_ERROR_OK && packet != NULL) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
    return error;
}

static utp_internal_error_t utp_connection_send_path_challenge(utp_connection_t* connection, uint64_t now_us)
{
    utp_frame_path_t     path;
    utp_internal_error_t error;

    if (connection == NULL || now_us == 0u || connection->path_state != UTP_CONNECTION_PATH_STATE_VALIDATING ||
        connection->path_challenge_pending) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (connection->path_challenge_retry_count >= UTP_CONNECTION_PATH_CHALLENGE_MAX_RETRIES) {
        return UTP_INTERNAL_ERROR_TIMEOUT;
    }
    if (RAND_bytes(connection->path_challenge, sizeof(connection->path_challenge)) != 1) {
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    memcpy(path.data, connection->path_challenge, sizeof(path.data));
    error = utp_connection_queue_path_frame(connection, UTP_FRAME_TYPE_PATH_CHALLENGE, &path,
                                            &connection->candidate_peer, true);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    connection->path_challenge_pending = true;
    connection->path_challenge_retry_count++;
    connection->path_challenge_deadline_us = now_us > UINT64_MAX - UTP_CONNECTION_PATH_CHALLENGE_TIMEOUT_US
                                                 ? UINT64_MAX
                                                 : now_us + UTP_CONNECTION_PATH_CHALLENGE_TIMEOUT_US;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_handle_path_challenge(utp_connection_t* connection, const uint8_t* frame,
                                                                 size_t frame_length, const utp_address_t* peer,
                                                                 bool candidate_path)
{
    utp_frame_path_t     path;
    utp_internal_error_t error;

    error = utp_frame_path_decode(&path, frame, frame_length, UTP_FRAME_TYPE_PATH_CHALLENGE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_connection_queue_path_frame(connection, UTP_FRAME_TYPE_PATH_RESPONSE, &path, peer, candidate_path);
}

static utp_internal_error_t utp_connection_handle_path_response(utp_connection_t* connection, const uint8_t* frame,
                                                                size_t frame_length, const utp_address_t* peer,
                                                                bool candidate_path)
{
    utp_frame_path_t     response;
    utp_internal_error_t error;

    error = utp_frame_path_decode(&response, frame, frame_length, UTP_FRAME_TYPE_PATH_RESPONSE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (!candidate_path || connection->path_state != UTP_CONNECTION_PATH_STATE_VALIDATING ||
        !connection->path_challenge_pending || !utp_address_equal(peer, &connection->candidate_peer) ||
        memcmp(response.data, connection->path_challenge, sizeof(response.data)) != 0) {
        return UTP_INTERNAL_ERROR_OK;
    }
    connection->peer                       = connection->candidate_peer;
    connection->path_challenge_deadline_us = 0u;
    connection->path_challenge_retry_count = 0u;
    connection->path_challenge_pending     = false;
    connection->candidate_queued_bytes     = 0u;
    connection->path_state                 = UTP_CONNECTION_PATH_STATE_VALIDATED;
    return UTP_INTERNAL_ERROR_OK;
}

/* 已发送包确认、丢失与控制帧生命周期。 */
static void utp_connection_release_queue(utp_connection_t* connection, struct utp_packet_out_tailq* packets)
{
    utp_packet_out_t* packet;

    while ((packet = TAILQ_FIRST(packets)) != NULL) {
        size_t meta_index;

        for (meta_index = 0u; meta_index < packet->frame_meta_count; ++meta_index) {
            const utp_frame_meta_info_t* meta = &packet->frame_meta[meta_index];

            if ((meta->frame_flags & UTP_FRAME_META_SEMANTIC_CONTROL) != 0u && meta->owner != NULL) {
                utp_connection_control_slot_t* slot = meta->owner;

                if (slot->in_flight && slot->in_flight_generation == meta->generation) {
                    slot->in_flight = false;
                }
            }
        }
        if ((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u && packet->stream_data_size != 0u) {
            utp_stream_t* stream = utp_connection_find_stream_internal(connection, packet->stream_id);

            if (stream != NULL) {
                (void)utp_stream_on_packet_acked_range(stream, packet->stream_offset, packet->stream_data_size);
            }
        }
        TAILQ_REMOVE(packets, packet, po_next);
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
}

static void utp_connection_process_acknowledged_packets(utp_connection_t*                  connection,
                                                        const struct utp_packet_out_tailq* packets, uint64_t now_us)
{
    const utp_packet_out_t* packet;

    TAILQ_FOREACH(packet, packets, po_next)
    {
        if ((packet->po_flags & UTP_PO_MTU_PROBE) != 0u) {
            (void)utp_mtu_discovery_on_probe_ack(&connection->mtu_discovery, packet->packet_number,
                                                 now_us / UINT64_C(1000));
        } else if ((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u) {
            (void)utp_mtu_discovery_on_data_packet_ack(
                &connection->mtu_discovery, utp_connection_packet_wire_size(packet), now_us / UINT64_C(1000));
        }
    }
}

static void utp_connection_process_lost_packet(utp_connection_t* connection, const utp_packet_out_t* packet,
                                               uint64_t now_us)
{
    if (connection == NULL || packet == NULL || now_us == 0u ||
        (packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) == 0u) {
        return;
    }
    (void)utp_mtu_discovery_on_data_packet_loss(&connection->mtu_discovery, utp_connection_packet_wire_size(packet),
                                                now_us / UINT64_C(1000));
}

static void utp_connection_process_detected_losses(utp_connection_t* connection, uint64_t now_us)
{
    utp_packet_out_t* packet;

    if (connection == NULL || now_us == 0u) {
        return;
    }
    TAILQ_FOREACH(packet, &connection->send_control.lost_packets, po_next)
    {
        if ((packet->local_flags & UTP_POL_LOSS) == 0u) {
            utp_connection_process_lost_packet(connection, packet, now_us);
            packet->local_flags |= UTP_POL_LOSS;
        }
    }
}

static void utp_connection_release_discarded_packets(utp_connection_t* connection, uint64_t now_us)
{
    utp_packet_out_t* packet;

    if (connection == NULL || now_us == 0u) {
        return;
    }
    while ((packet = utp_send_control_next_discarded(&connection->send_control)) != NULL) {
        if ((packet->po_flags & UTP_PO_MTU_PROBE) != 0u) {
            (void)utp_mtu_discovery_on_probe_lost(&connection->mtu_discovery, packet->packet_number,
                                                  now_us / UINT64_C(1000));
        }
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
}

static void utp_connection_commit_sent_controls(utp_connection_t* connection, const utp_packet_out_t* packet,
                                                uint64_t now_us)
{
    size_t index;

    for (index = 0u; index < packet->frame_meta_count; ++index) {
        const utp_frame_meta_info_t*   meta = &packet->frame_meta[index];
        utp_connection_control_slot_t* slot;

        if ((meta->frame_flags & UTP_FRAME_META_SEMANTIC_CONTROL) == 0u || meta->owner == NULL) {
            continue;
        }
        slot         = meta->owner;
        slot->queued = false;
        if (slot->generation == meta->generation) {
            slot->in_flight            = true;
            slot->in_flight_generation = meta->generation;
        }
        switch (meta->frame_type) {
        case UTP_FRAME_TYPE_MAX_DATA:
            if (meta->value > connection->local_max_data_advertised) {
                connection->local_max_data_advertised = meta->value;
            }
            connection->last_max_data_sent_us = now_us;
            break;
        case UTP_FRAME_TYPE_MAX_STREAM_DATA: {
            utp_stream_t* stream = utp_connection_find_stream_internal(connection, slot->stream_id);

            if (stream != NULL) {
                if (meta->value > stream->local_max_stream_data_advertised) {
                    stream->local_max_stream_data_advertised = meta->value;
                }
                stream->last_max_stream_data_sent_us = now_us;
            }
            break;
        }
        case UTP_FRAME_TYPE_DATA_BLOCKED:
            connection->last_data_blocked_sent_us = now_us;
            break;
        case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED: {
            utp_stream_t* stream = utp_connection_find_stream_internal(connection, slot->stream_id);

            if (stream != NULL) {
                stream->last_stream_data_blocked_sent_us = now_us;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void utp_connection_requeue_lost_controls(const utp_packet_out_t* packet)
{
    size_t index;

    for (index = 0u; index < packet->frame_meta_count; ++index) {
        const utp_frame_meta_info_t*   meta = &packet->frame_meta[index];
        utp_connection_control_slot_t* slot;

        if ((meta->frame_flags & UTP_FRAME_META_SEMANTIC_CONTROL) == 0u || meta->owner == NULL) {
            continue;
        }
        slot         = meta->owner;
        slot->queued = false;
        if (slot->in_flight && slot->in_flight_generation == meta->generation) {
            slot->in_flight = false;
        }
        if (slot->generation == meta->generation) {
            slot->pending = true;
        }
    }
}

static bool utp_connection_has_pending_controls(const utp_connection_t* connection)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (connection == NULL) {
        return false;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->control_slots, &iter)) != NULL) {
        const utp_connection_control_slot_t* slot =
            (const utp_connection_control_slot_t*)((const uint8_t*)node -
                                                   offsetof(utp_connection_control_slot_t, node));

        if (slot->pending && !slot->queued && !slot->in_flight) {
            return true;
        }
    }
    return false;
}

/* 流、可靠控制帧与哈希索引管理。 */
static uint32_t utp_connection_local_stream_initiator_bit(const utp_connection_t* connection)
{
    return connection->role == UTP_CONNECTION_ROLE_ACTIVE ? UTP_STREAM_CLIENT_INITIATED : UTP_STREAM_SERVER_INITIATED;
}

static uint32_t utp_connection_peer_stream_initiator_bit(const utp_connection_t* connection)
{
    return connection->role == UTP_CONNECTION_ROLE_ACTIVE ? UTP_STREAM_SERVER_INITIATED : UTP_STREAM_CLIENT_INITIATED;
}

static uint8_t utp_connection_stream_type_from_id(uint32_t stream_id)
{
    return (stream_id & UTP_STREAM_UNIDIRECTIONAL) == 0u ? UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL
                                                         : UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL;
}

static uint64_t utp_connection_hash_u32(uint32_t value) { return (uint64_t)value * UINT64_C(11400714819323198485); }

static uint64_t utp_connection_hash_u64(uint64_t value)
{
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31u);
}

static bool utp_connection_stream_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_stream_t* stream = (const utp_stream_t*)((const uint8_t*)node - offsetof(utp_stream_t, hash_node));

    (void)user_data;
    return key != NULL && stream->stream_id == *(const uint32_t*)key;
}

static bool utp_connection_control_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_connection_control_slot_t* slot =
        (const utp_connection_control_slot_t*)((const uint8_t*)node - offsetof(utp_connection_control_slot_t, node));
    const uint64_t expected = ((uint64_t)slot->frame_type << 32u) | slot->stream_id;

    (void)user_data;
    return key != NULL && expected == *(const uint64_t*)key;
}

static bool utp_connection_pending_max_stream_data_matches(const utp_hash_node_t* node, const void* key,
                                                           void* user_data)
{
    const utp_connection_pending_max_stream_data_t* pending =
        (const utp_connection_pending_max_stream_data_t*)((const uint8_t*)node -
                                                          offsetof(utp_connection_pending_max_stream_data_t, node));

    (void)user_data;
    return key != NULL && pending->stream_id == *(const uint32_t*)key;
}

static utp_stream_t* utp_connection_stream_from_node(utp_hash_node_t* node)
{
    return node == NULL ? NULL : (utp_stream_t*)((uint8_t*)node - offsetof(utp_stream_t, hash_node));
}

static utp_connection_control_slot_t* utp_connection_control_slot_from_node(utp_hash_node_t* node)
{
    return node == NULL
               ? NULL
               : (utp_connection_control_slot_t*)((uint8_t*)node - offsetof(utp_connection_control_slot_t, node));
}

static utp_connection_pending_max_stream_data_t* utp_connection_pending_max_stream_data_from_node(utp_hash_node_t* node)
{
    return node == NULL
               ? NULL
               : (utp_connection_pending_max_stream_data_t*)((uint8_t*)node -
                                                             offsetof(utp_connection_pending_max_stream_data_t, node));
}

static uint32_t utp_connection_stream_ordinal(uint32_t stream_id) { return stream_id / UTP_STREAM_TYPES + 1u; }

static bool     utp_connection_stream_is_peer_initiated(const utp_connection_t* connection, uint32_t stream_id)
{
    return (stream_id & UINT32_C(1)) == utp_connection_peer_stream_initiator_bit(connection);
}

static bool utp_connection_take_pending_peer_max_stream_data(utp_connection_t* connection, uint32_t stream_id,
                                                             uint64_t* out_max_stream_data)
{
    utp_connection_pending_max_stream_data_t* pending;

    if (connection == NULL || out_max_stream_data == NULL) {
        return false;
    }
    // MAX_STREAM_DATA 可以先于首个 STREAM 到达，先按 stream_id 保存其最新最大值。
    pending = utp_connection_pending_max_stream_data_from_node(
        utp_hash_table_find(&connection->pending_peer_max_stream_data, utp_connection_hash_u32(stream_id), &stream_id,
                            utp_connection_pending_max_stream_data_matches, NULL));
    if (pending != NULL) {
        *out_max_stream_data = pending->value;
        (void)utp_hash_table_remove(&connection->pending_peer_max_stream_data, &pending->node);
        utp_allocator_free(NULL, pending);
        return true;
    }
    return false;
}

static utp_internal_error_t utp_connection_store_pending_peer_max_stream_data(utp_connection_t* connection,
                                                                              uint32_t          stream_id,
                                                                              uint64_t          maximum_stream_data)
{
    utp_connection_pending_max_stream_data_t* pending;
    utp_internal_error_t                      error;

    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    pending = utp_connection_pending_max_stream_data_from_node(
        utp_hash_table_find(&connection->pending_peer_max_stream_data, utp_connection_hash_u32(stream_id), &stream_id,
                            utp_connection_pending_max_stream_data_matches, NULL));
    if (pending != NULL) {
        if (maximum_stream_data > pending->value) {
            pending->value = maximum_stream_data;
        }
        return UTP_INTERNAL_ERROR_OK;
    }
    pending = utp_allocator_alloc(NULL, sizeof(*pending));
    if (pending == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    utp_hash_node_init(&pending->node);
    pending->stream_id = stream_id;
    pending->value     = maximum_stream_data;
    error              = utp_hash_table_insert(&connection->pending_peer_max_stream_data, &pending->node,
                                               utp_connection_hash_u32(stream_id), &stream_id,
                                               utp_connection_pending_max_stream_data_matches, NULL);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_allocator_free(NULL, pending);
    }
    return error;
}

static utp_internal_error_t utp_connection_alloc_stream(utp_connection_t* connection, uint32_t stream_id,
                                                        utp_stream_t** out_stream)
{
    utp_stream_t*        stream;
    uint64_t             pending_max_stream_data;
    utp_internal_error_t error;

    if (connection == NULL || out_stream == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *out_stream = NULL;
    utp_connection_reclaim_closed_stream_slots(connection);
    stream = utp_allocator_alloc(NULL, sizeof(*stream));
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    utp_stream_init(stream, stream_id);
    utp_hash_node_init(&stream->hash_node);
    stream->connection                = connection;
    stream->connection_consumed_total = &connection->local_stream_data_consumed_total;
    // 先完成哈希表插入，再消费提前到达的窗口，失败路径不会丢失对端通告。
    error = utp_hash_table_insert(&connection->streams, &stream->hash_node, utp_connection_hash_u32(stream_id),
                                  &stream_id, utp_connection_stream_matches, NULL);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_stream_cleanup(stream);
        utp_allocator_free(NULL, stream);
        return error;
    }
    if (utp_connection_take_pending_peer_max_stream_data(connection, stream_id, &pending_max_stream_data)) {
        utp_stream_update_peer_max_stream_data(stream, pending_max_stream_data);
    }
    *out_stream = stream;
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_get_or_create_peer_stream(utp_connection_t* connection, uint32_t stream_id,
                                                                     utp_stream_t** out_stream)
{
    utp_stream_t* stream;

    if (out_stream == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    stream = utp_connection_find_stream_internal(connection, stream_id);
    if (stream != NULL) {
        if (!utp_stream_local_can_receive(stream)) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        *out_stream = stream;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (!utp_connection_stream_is_peer_initiated(connection, stream_id)) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    utp_connection_update_completed_peer_streams(connection);
    if (utp_connection_stream_ordinal(stream_id) >
        (uint32_t)connection->local_max_streams[utp_connection_stream_type_from_id(stream_id)]) {
        return UTP_INTERNAL_ERROR_STREAM_LIMIT;
    }
    {
        const utp_internal_error_t error = utp_connection_alloc_stream(connection, stream_id, &stream);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
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

static utp_connection_control_slot_t* utp_connection_find_control_slot(utp_connection_t* connection, uint8_t frame_type,
                                                                       uint32_t stream_id, bool create)
{
    uint64_t                       key;
    utp_connection_control_slot_t* slot;
    utp_internal_error_t           error;

    if (connection == NULL) {
        return NULL;
    }
    // control slot 按“帧类型 + 流 ID/方向”合并，只保留具有最新语义的一份待发送状态。
    key  = ((uint64_t)frame_type << 32u) | stream_id;
    slot = utp_connection_control_slot_from_node(utp_hash_table_find(
        &connection->control_slots, utp_connection_hash_u64(key), &key, utp_connection_control_slot_matches, NULL));
    if (slot != NULL || !create) {
        return slot;
    }
    slot = utp_allocator_alloc(NULL, sizeof(*slot));
    if (slot == NULL) {
        return NULL;
    }
    utp_hash_node_init(&slot->node);
    slot->value                = 0u;
    slot->final_size           = 0u;
    slot->stream_id            = stream_id;
    slot->generation           = 0u;
    slot->in_flight_generation = 0u;
    slot->error_code           = 0u;
    slot->frame_type           = frame_type;
    slot->pending              = false;
    slot->queued               = false;
    slot->in_flight            = false;
    error = utp_hash_table_insert(&connection->control_slots, &slot->node, utp_connection_hash_u64(key), &key,
                                  utp_connection_control_slot_matches, NULL);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_allocator_free(NULL, slot);
        return NULL;
    }
    return slot;
}

static void utp_connection_control_advance_generation(utp_connection_control_slot_t* slot)
{
    // generation 区分同一语义槽位的不同时代，旧包 ACK/丢失不能覆盖更新后的值。
    if (slot->generation == UINT32_MAX) {
        slot->generation = 1u;
    } else {
        ++slot->generation;
    }
}

static utp_internal_error_t utp_connection_mark_control_pending(utp_connection_t* connection, uint8_t frame_type,
                                                                uint32_t stream_id, uint64_t value, uint16_t error_code,
                                                                uint64_t final_size)
{
    utp_connection_control_slot_t* slot;
    bool                           changed = false;

    slot = utp_connection_find_control_slot(connection, frame_type, stream_id, true);
    if (slot == NULL) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    switch (frame_type) {
    case UTP_FRAME_TYPE_MAX_DATA:
    case UTP_FRAME_TYPE_MAX_STREAM_DATA:
    case UTP_FRAME_TYPE_MAX_STREAMS:
        if (value > slot->value) {
            slot->value = value;
            changed     = true;
        }
        break;
    case UTP_FRAME_TYPE_DATA_BLOCKED:
    case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED:
    case UTP_FRAME_TYPE_STREAMS_BLOCKED:
        if (slot->generation == 0u || slot->value != value) {
            slot->value = value;
            changed     = true;
        }
        break;
    case UTP_FRAME_TYPE_RESET_STREAM:
        if (slot->generation != 0u) {
            return slot->error_code == error_code && slot->final_size == final_size ? UTP_INTERNAL_ERROR_OK
                                                                                    : UTP_INTERNAL_ERROR_STATE;
        }
        slot->error_code = error_code;
        slot->final_size = final_size;
        changed          = true;
        break;
    default:
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (changed || slot->generation == 0u) {
        utp_connection_control_advance_generation(slot);
    }
    if (!slot->queued && !slot->in_flight) {
        slot->pending = true;
    } else if (changed) {
        slot->pending = true;
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_queue_max_data(utp_connection_t* connection, uint64_t maximum_data,
                                                          uint64_t now_us)
{
    (void)now_us;
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_MAX_DATA, 0u, maximum_data, 0u, 0u);
}

static utp_internal_error_t utp_connection_queue_max_stream_data(utp_connection_t* connection, uint32_t stream_id,
                                                                 uint64_t maximum_stream_data, uint64_t now_us)
{
    (void)now_us;
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_MAX_STREAM_DATA, stream_id,
                                               maximum_stream_data, 0u, 0u);
}

static utp_internal_error_t utp_connection_queue_data_blocked(utp_connection_t* connection, uint64_t data_limit,
                                                              uint64_t now_us)
{
    (void)now_us;
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_DATA_BLOCKED, 0u, data_limit, 0u, 0u);
}

static utp_internal_error_t utp_connection_queue_stream_data_blocked(utp_connection_t* connection, uint32_t stream_id,
                                                                     uint64_t stream_data_limit, uint64_t now_us)
{
    (void)now_us;
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_STREAM_DATA_BLOCKED, stream_id,
                                               stream_data_limit, 0u, 0u);
}

static utp_internal_error_t utp_connection_queue_reset_stream(utp_connection_t* connection, uint32_t stream_id,
                                                              uint16_t error_code, uint64_t final_size)
{
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_RESET_STREAM, stream_id, 0u, error_code,
                                               final_size);
}

static utp_internal_error_t utp_connection_queue_max_streams(utp_connection_t* connection, uint8_t stream_type,
                                                             uint16_t maximum_streams)
{
    if (stream_type > UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_MAX_STREAMS, stream_type, maximum_streams, 0u,
                                               0u);
}

static utp_internal_error_t utp_connection_queue_streams_blocked(utp_connection_t* connection, uint8_t stream_type,
                                                                 uint16_t stream_limit)
{
    if (stream_type > UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return utp_connection_mark_control_pending(connection, UTP_FRAME_TYPE_STREAMS_BLOCKED, stream_type, stream_limit,
                                               0u, 0u);
}

static bool utp_connection_stream_is_limit_complete(const utp_connection_t* connection, const utp_stream_t* stream)
{
    if (connection == NULL || stream == NULL || !stream->used) {
        return false;
    }
    if (stream->reset) {
        return true;
    }
    if (utp_connection_stream_type_from_id(stream->stream_id) == UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL) {
        return utp_connection_stream_is_peer_initiated(connection, stream->stream_id) ? stream->peer_fin
                                                                                      : stream->local_fin_sent;
    }
    return stream->local_fin_queued && stream->local_fin_sent && stream->peer_fin;
}

static bool utp_connection_control_slot_is_stream_specific(const utp_connection_control_slot_t* slot,
                                                           uint32_t                             stream_id)
{
    return slot != NULL && slot->stream_id == stream_id &&
           (slot->frame_type == UTP_FRAME_TYPE_MAX_STREAM_DATA ||
            slot->frame_type == UTP_FRAME_TYPE_STREAM_DATA_BLOCKED || slot->frame_type == UTP_FRAME_TYPE_RESET_STREAM);
}

static bool utp_connection_stream_has_active_control(const utp_connection_t* connection, uint32_t stream_id)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (connection == NULL) {
        return false;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->control_slots, &iter)) != NULL) {
        const utp_connection_control_slot_t* slot =
            (const utp_connection_control_slot_t*)((const uint8_t*)node -
                                                   offsetof(utp_connection_control_slot_t, node));

        if (utp_connection_control_slot_is_stream_specific(slot, stream_id) &&
            (slot->pending || slot->queued || slot->in_flight)) {
            return true;
        }
    }
    return false;
}

static bool utp_connection_packet_queue_has_stream(const struct utp_packet_out_tailq* packets, uint32_t stream_id)
{
    const utp_packet_out_t* packet;

    if (packets == NULL) {
        return false;
    }
    TAILQ_FOREACH(packet, packets, po_next)
    {
        if ((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u && packet->stream_id == stream_id) {
            return true;
        }
    }
    return false;
}

static bool utp_connection_stream_has_packet_reference(const utp_connection_t* connection, uint32_t stream_id)
{
    if (connection == NULL) {
        return false;
    }
    return utp_connection_packet_queue_has_stream(&connection->send_control.scheduled_packets, stream_id) ||
           utp_connection_packet_queue_has_stream(&connection->send_control.ledger.unacked_packets, stream_id) ||
           utp_connection_packet_queue_has_stream(&connection->send_control.lost_packets, stream_id) ||
           utp_connection_packet_queue_has_stream(&connection->send_control.discarded_packets, stream_id);
}

static void utp_connection_release_idle_stream_controls(utp_connection_t* connection, uint32_t stream_id)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (connection == NULL) {
        return;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->control_slots, &iter)) != NULL) {
        utp_connection_control_slot_t* slot = utp_connection_control_slot_from_node(node);

        if (!utp_connection_control_slot_is_stream_specific(slot, stream_id) || slot->pending || slot->queued ||
            slot->in_flight) {
            continue;
        }
        (void)utp_hash_table_remove(&connection->control_slots, &slot->node);
        utp_allocator_free(NULL, slot);
    }
}

static void utp_connection_update_completed_peer_streams(utp_connection_t* connection)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (connection == NULL) {
        return;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
        utp_stream_t* stream = utp_connection_stream_from_node(node);
        uint8_t       stream_type;

        if (stream == NULL || stream->stream_limit_released ||
            !utp_connection_stream_is_peer_initiated(connection, stream->stream_id) ||
            !utp_connection_stream_is_limit_complete(connection, stream)) {
            continue;
        }
        // 每条对端流只归还一次额度，MAX_STREAMS 始终单调递增。
        stream->stream_limit_released = true;
        stream_type                   = utp_connection_stream_type_from_id(stream->stream_id);
        if (connection->local_max_streams[stream_type] == UINT16_MAX) {
            continue;
        }
        ++connection->local_max_streams[stream_type];
        (void)utp_connection_queue_max_streams(connection, stream_type, connection->local_max_streams[stream_type]);
    }
}

static void utp_connection_reclaim_closed_stream_slots(utp_connection_t* connection)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (connection == NULL) {
        return;
    }
    utp_connection_update_completed_peer_streams(connection);
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
        utp_stream_t* stream = utp_connection_stream_from_node(node);

        // 流即使逻辑关闭，只要 PacketOut 或可靠 control 仍引用它，就不能释放对象。
        if (stream == NULL || stream->recv_buffered_bytes != 0u || stream->send_buffer_length != 0u ||
            stream->send_in_flight_bytes != 0u ||
            utp_connection_stream_has_packet_reference(connection, stream->stream_id) ||
            utp_connection_stream_has_active_control(connection, stream->stream_id) ||
            !utp_connection_stream_is_limit_complete(connection, stream) ||
            (utp_connection_stream_is_peer_initiated(connection, stream->stream_id) &&
             !stream->stream_limit_released)) {
            continue;
        }
        utp_connection_release_idle_stream_controls(connection, stream->stream_id);
        (void)utp_hash_table_remove(&connection->streams, &stream->hash_node);
        utp_stream_cleanup(stream);
        utp_allocator_free(NULL, stream);
    }
}

/* 流量控制更新与可靠控制帧合包。 */
static utp_internal_error_t utp_connection_queue_pending_flow_control(utp_connection_t* connection, uint64_t now_us,
                                                                      bool* queued)
{
    uint64_t         target;
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

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
        *queued = true;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
        utp_stream_t* stream = utp_connection_stream_from_node(node);

        if (stream == NULL) {
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
            *queued = true;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static uint8_t utp_connection_control_priority(uint8_t frame_type)
{
    switch (frame_type) {
    case UTP_FRAME_TYPE_RESET_STREAM:
        return 3u;
    case UTP_FRAME_TYPE_MAX_DATA:
    case UTP_FRAME_TYPE_MAX_STREAM_DATA:
    case UTP_FRAME_TYPE_MAX_STREAMS:
        return 4u;
    case UTP_FRAME_TYPE_DATA_BLOCKED:
    case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED:
    case UTP_FRAME_TYPE_STREAMS_BLOCKED:
        return 5u;
    default:
        return UINT8_MAX;
    }
}

static utp_internal_error_t utp_connection_encode_control_slot(const utp_connection_control_slot_t* slot,
                                                               uint8_t* buffer, size_t capacity, size_t* out_length)
{
    if (slot == NULL || buffer == NULL || out_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    switch (slot->frame_type) {
    case UTP_FRAME_TYPE_RESET_STREAM: {
        const utp_frame_reset_stream_t frame = {slot->error_code, slot->stream_id, slot->final_size};

        *out_length = UTP_FRAME_RESET_STREAM_SIZE;
        return utp_frame_reset_stream_encode(buffer, capacity, &frame);
    }
    case UTP_FRAME_TYPE_MAX_DATA: {
        const utp_frame_max_data_t frame = {slot->value};

        *out_length = UTP_FRAME_MAX_DATA_SIZE;
        return utp_frame_max_data_encode(buffer, capacity, &frame);
    }
    case UTP_FRAME_TYPE_MAX_STREAM_DATA: {
        const utp_frame_max_stream_data_t frame = {slot->stream_id, slot->value};

        *out_length = UTP_FRAME_MAX_STREAM_DATA_SIZE;
        return utp_frame_max_stream_data_encode(buffer, capacity, &frame);
    }
    case UTP_FRAME_TYPE_MAX_STREAMS: {
        const utp_frame_streams_limit_t frame = {(uint16_t)slot->value, (uint8_t)slot->stream_id};

        *out_length = UTP_FRAME_STREAMS_LIMIT_SIZE;
        return utp_frame_max_streams_encode(buffer, capacity, &frame);
    }
    case UTP_FRAME_TYPE_DATA_BLOCKED: {
        const utp_frame_data_blocked_t frame = {slot->value};

        *out_length = UTP_FRAME_DATA_BLOCKED_SIZE;
        return utp_frame_data_blocked_encode(buffer, capacity, &frame);
    }
    case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED: {
        const utp_frame_stream_data_blocked_t frame = {slot->stream_id, slot->value};

        *out_length = UTP_FRAME_STREAM_DATA_BLOCKED_SIZE;
        return utp_frame_stream_data_blocked_encode(buffer, capacity, &frame);
    }
    case UTP_FRAME_TYPE_STREAMS_BLOCKED: {
        const utp_frame_streams_limit_t frame = {(uint16_t)slot->value, (uint8_t)slot->stream_id};

        *out_length = UTP_FRAME_STREAMS_LIMIT_SIZE;
        return utp_frame_streams_blocked_encode(buffer, capacity, &frame);
    }
    default:
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
}

static utp_internal_error_t utp_connection_encode_ack_payload(utp_connection_t* connection, uint64_t now_us,
                                                              uint8_t* payload, size_t capacity, size_t* out_length)
{
    utp_ack_range_t      ranges[UTP_CONNECTION_MAX_RECEIVE_RANGES];
    utp_ack_info_t       ack = {0u, 0u, ranges, 0u, UTP_CONNECTION_MAX_RECEIVE_RANGES};
    utp_internal_error_t error;

    if (connection == NULL || payload == NULL || out_length == NULL || now_us == 0u ||
        utp_ack_scheduler_pending_count(&connection->ack_scheduler) == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_ack_from_receive_history(&ack, &connection->receive_history, now_us, UTP_CONNECTION_MAX_RECEIVE_RANGES);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_ack_encode(payload, capacity, &ack, 0u, out_length);
}

static utp_internal_error_t utp_connection_queue_control_packet(utp_connection_t* connection, uint64_t now_us,
                                                                bool include_ack, bool include_controls,
                                                                bool schedule_front, bool* queued)
{
    utp_connection_control_slot_t* selected[UTP_PACKET_OUT_MAX_FRAMES];
    uint8_t
        ack_payload[UTP_ACK_FRAME_HEADER_SIZE + (UTP_CONNECTION_MAX_RECEIVE_RANGES - 1u) * UTP_ACK_FRAME_RANGE_SIZE];
    utp_packet_out_t*    packet = NULL;
    utp_internal_error_t error;
    uint64_t             packet_number;
    size_t               selected_count = 0u;
    size_t               payload_length = 0u;
    size_t               ack_length     = 0u;
    size_t               priority;
    size_t               index;
    utp_hash_iter_t      control_iter;
    utp_hash_node_t*     control_node;
    uint16_t             packet_capacity;

    if (connection == NULL || queued == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *queued = false;
    if (!utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    packet_capacity = utp_connection_plaintext_packet_capacity(connection);
    if (packet_capacity < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    // ACK 是瞬态前缀，可靠 control 按优先级填充剩余 MTU；重传时会自动剔除旧 ACK。
    if (include_ack) {
        error = utp_connection_encode_ack_payload(connection, now_us, ack_payload, sizeof(ack_payload), &ack_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (ack_length > (size_t)packet_capacity - UTP_PACKET_HEADER_SIZE) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
        payload_length = ack_length;
    }
    for (priority = 3u; include_controls && priority <= 5u; ++priority) {
        utp_hash_iter_init(&control_iter);
        while ((control_node = utp_hash_iter_next(&connection->control_slots, &control_iter)) != NULL) {
            utp_connection_control_slot_t* slot = utp_connection_control_slot_from_node(control_node);
            size_t                         frame_length;

            if (!slot->pending || slot->queued || slot->in_flight ||
                utp_connection_control_priority(slot->frame_type) != priority) {
                continue;
            }
            switch (slot->frame_type) {
            case UTP_FRAME_TYPE_RESET_STREAM:
                frame_length = UTP_FRAME_RESET_STREAM_SIZE;
                break;
            case UTP_FRAME_TYPE_MAX_DATA:
            case UTP_FRAME_TYPE_DATA_BLOCKED:
                frame_length = UTP_FRAME_MAX_DATA_SIZE;
                break;
            case UTP_FRAME_TYPE_MAX_STREAM_DATA:
            case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED:
                frame_length = UTP_FRAME_MAX_STREAM_DATA_SIZE;
                break;
            case UTP_FRAME_TYPE_MAX_STREAMS:
            case UTP_FRAME_TYPE_STREAMS_BLOCKED:
                frame_length = UTP_FRAME_STREAMS_LIMIT_SIZE;
                break;
            default:
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            if (selected_count + (include_ack ? 1u : 0u) == UTP_PACKET_OUT_MAX_FRAMES ||
                frame_length > (size_t)packet_capacity - UTP_PACKET_HEADER_SIZE - payload_length) {
                continue;
            }
            selected[selected_count++]  = slot;
            payload_length             += frame_length;
        }
    }
    if (selected_count == 0u && !include_ack) {
        return UTP_INTERNAL_ERROR_OK;
    }
    error = utp_packet_out_pool_acquire(&connection->packet_pool, (uint16_t)(UTP_PACKET_HEADER_SIZE + payload_length),
                                        &packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
    if (error == UTP_INTERNAL_ERROR_OK) {
        size_t offset = ack_length;

        packet->packet_number       = packet_number;
        packet->data_size           = (uint16_t)(UTP_PACKET_HEADER_SIZE + payload_length);
        packet->packet_type         = UTP_PACKET_TYPE_CTRL;
        packet->frame_types         = 0u;
        packet->control_prefix_size = (uint16_t)payload_length;
        packet->transient_ack_size  = (uint16_t)ack_length;
        packet->slice_count         = 1u;
        packet->slices[0].source    = UTP_PACKET_OUT_SLICE_RAW_OFFSET;
        packet->slices[0].offset    = 0u;
        packet->slices[0].length    = packet->data_size;
        packet->slices[0].data      = NULL;
        if (include_ack) {
            memcpy(packet->raw_data + UTP_PACKET_HEADER_SIZE, ack_payload, ack_length);
            packet->frame_types               = UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK);
            packet->frame_meta[0].owner       = NULL;
            packet->frame_meta[0].value       = 0u;
            packet->frame_meta[0].offset      = UTP_PACKET_HEADER_SIZE;
            packet->frame_meta[0].length      = (uint16_t)ack_length;
            packet->frame_meta[0].generation  = 0u;
            packet->frame_meta[0].frame_type  = UTP_FRAME_TYPE_ACK;
            packet->frame_meta[0].frame_flags = UTP_FRAME_META_TRANSIENT_ON_RETRANSMIT;
        }
        for (index = 0u; index < selected_count && error == UTP_INTERNAL_ERROR_OK; ++index) {
            size_t frame_length = 0u;
            size_t meta_index   = index + (include_ack ? 1u : 0u);

            error =
                utp_connection_encode_control_slot(selected[index], packet->raw_data + UTP_PACKET_HEADER_SIZE + offset,
                                                   payload_length - offset, &frame_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                utp_frame_meta_info_t* meta = &packet->frame_meta[meta_index];

                meta->owner          = selected[index];
                meta->value          = selected[index]->value;
                meta->offset         = (uint16_t)(UTP_PACKET_HEADER_SIZE + offset);
                meta->length         = (uint16_t)frame_length;
                meta->generation     = selected[index]->generation;
                meta->frame_type     = selected[index]->frame_type;
                meta->frame_flags    = UTP_FRAME_META_SEMANTIC_CONTROL;
                packet->frame_types |= UTP_FRAME_BIT(selected[index]->frame_type);
                offset              += frame_length;
            }
        }
        packet->frame_meta_count = (uint8_t)(selected_count + (include_ack ? 1u : 0u));
        if (error == UTP_INTERNAL_ERROR_OK && offset != payload_length) {
            error = UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_encode_header(connection, packet, UTP_PACKET_TYPE_CTRL);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = schedule_front
                    ? utp_send_control_schedule_packet_front(&connection->send_control, packet, selected_count != 0u)
                    : utp_send_control_schedule_packet(&connection->send_control, packet, selected_count != 0u);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
        return error;
    }
    for (index = 0u; index < selected_count; ++index) {
        selected[index]->pending = false;
        selected[index]->queued  = true;
    }
    *queued = true;
    return UTP_INTERNAL_ERROR_OK;
}

/* 流调度与 STREAM/control/ACK 合包。 */
static uint8_t utp_connection_stream_effective_priority(const utp_stream_t* stream)
{
    uint8_t boost;

    if (stream == NULL || stream->priority > UTP_STREAM_PRIORITY_LOWEST) {
        return UTP_STREAM_PRIORITY_DEFAULT;
    }
    boost = (uint8_t)(stream->strict_wait_rounds / 8u);
    return boost >= stream->priority ? UTP_STREAM_PRIORITY_HIGHEST : (uint8_t)(stream->priority - boost);
}

static uint32_t utp_connection_stream_schedule_distance(uint32_t cursor, uint32_t stream_id)
{
    return stream_id - cursor;
}

static utp_stream_t* utp_connection_select_stream(utp_connection_t* connection)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;
    utp_stream_t*    selected      = NULL;
    uint8_t          best_priority = UTP_STREAM_PRIORITY_LOWEST;
    uint32_t         best_distance = UINT32_MAX;

    if (connection == NULL) {
        return NULL;
    }
    // DRR 使用游标和 deficit 控制份额；Strict 使用等待轮次提升防止低优先级永久饥饿。
    if (connection->stream_scheduler_mode == 1u) {
        utp_hash_iter_init(&iter);
        while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
            utp_stream_t* stream = utp_connection_stream_from_node(node);
            uint32_t      distance;

            if (!utp_stream_has_send_work(stream)) {
                continue;
            }
            distance = utp_connection_stream_schedule_distance(connection->stream_scheduler_cursor, stream->stream_id);
            if (selected == NULL || distance < best_distance) {
                selected      = stream;
                best_distance = distance;
            }
        }
        if (selected != NULL) {
            const uint32_t quantum = UINT32_C(1200) * (uint32_t)(UTP_STREAM_PRIORITY_LOWEST - selected->priority + 1u);

            selected->drr_deficit =
                selected->drr_deficit > UINT32_C(131072) - quantum ? UINT32_C(131072) : selected->drr_deficit + quantum;
            connection->stream_scheduler_cursor = selected->stream_id + 1u;
        }
        return selected;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
        utp_stream_t* stream = utp_connection_stream_from_node(node);
        uint8_t       effective_priority;

        if (!utp_stream_has_send_work(stream)) {
            stream->strict_wait_rounds = 0u;
            continue;
        }
        effective_priority = utp_connection_stream_effective_priority(stream);
        if (selected == NULL || effective_priority < best_priority) {
            selected      = stream;
            best_priority = effective_priority;
        }
    }
    if (selected == NULL) {
        return selected;
    }
    best_distance = UINT32_MAX;
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
        utp_stream_t* stream = utp_connection_stream_from_node(node);
        uint32_t      distance;

        if (!utp_stream_has_send_work(stream) || utp_connection_stream_effective_priority(stream) != best_priority) {
            continue;
        }
        distance = utp_connection_stream_schedule_distance(connection->stream_scheduler_cursor, stream->stream_id);
        if (distance < best_distance) {
            selected      = stream;
            best_distance = distance;
        }
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&connection->streams, &iter)) != NULL) {
        utp_stream_t* stream = utp_connection_stream_from_node(node);

        if (!utp_stream_has_send_work(stream)) {
            stream->strict_wait_rounds = 0u;
        } else if (stream == selected) {
            stream->strict_wait_rounds = 0u;
        } else if (stream->strict_wait_rounds != UINT8_MAX) {
            ++stream->strict_wait_rounds;
        }
    }
    connection->stream_scheduler_cursor = selected->stream_id + 1u;
    return selected;
}

static utp_internal_error_t utp_connection_queue_next_stream_packet(utp_connection_t* connection, uint64_t now_us,
                                                                    bool include_ack, bool include_controls,
                                                                    bool* queued)
{
    uint8_t
        ack_payload[UTP_ACK_FRAME_HEADER_SIZE + (UTP_CONNECTION_MAX_RECEIVE_RANGES - 1u) * UTP_ACK_FRAME_RANGE_SIZE];
    uint8_t                        stream_header[UTP_FRAME_STREAM_HEADER_SIZE];
    utp_packet_out_t*              packet = NULL;
    utp_stream_t*                  stream;
    const uint8_t*                 stream_data;
    utp_connection_control_slot_t* selected[UTP_PACKET_OUT_MAX_FRAMES];
    size_t                         control_index;
    size_t                         stream_header_length;
    size_t                         packet_length;
    size_t                         raw_length;
    size_t                         max_data_length;
    size_t                         ack_length = 0u;
    size_t                         control_prefix_length;
    size_t                         selected_count = 0u;
    size_t                         priority;
    uint32_t                       stream_data_size;
    uint64_t                       stream_offset;
    uint64_t                       packet_number;
    uint16_t                       packet_capacity;
    bool                           fin;
    bool                           stream_committed = false;
    utp_internal_error_t           error;
    utp_hash_iter_t                control_iter;
    utp_hash_node_t*               control_node;

    if (queued == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *queued = false;
    if (!utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    packet_capacity = utp_connection_plaintext_packet_capacity(connection);
    if (packet_capacity < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    // 包内布局固定为 ACK、可靠 control、STREAM header、外部数据视图，数据本身不复制。
    if (include_ack) {
        error = utp_connection_encode_ack_payload(connection, now_us, ack_payload, sizeof(ack_payload), &ack_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (packet_capacity < UTP_PACKET_HEADER_SIZE + ack_length + UTP_FRAME_STREAM_HEADER_SIZE) {
            return UTP_INTERNAL_ERROR_LIMIT;
        }
    }
    control_prefix_length = ack_length;
    if (include_controls) {
        for (priority = 3u; priority <= 5u; ++priority) {
            utp_hash_iter_init(&control_iter);
            while ((control_node = utp_hash_iter_next(&connection->control_slots, &control_iter)) != NULL) {
                utp_connection_control_slot_t* slot = utp_connection_control_slot_from_node(control_node);
                size_t                         frame_length;

                if (!slot->pending || slot->queued || slot->in_flight ||
                    utp_connection_control_priority(slot->frame_type) != priority) {
                    continue;
                }
                switch (slot->frame_type) {
                case UTP_FRAME_TYPE_RESET_STREAM:
                    frame_length = UTP_FRAME_RESET_STREAM_SIZE;
                    break;
                case UTP_FRAME_TYPE_MAX_DATA:
                case UTP_FRAME_TYPE_DATA_BLOCKED:
                    frame_length = UTP_FRAME_MAX_DATA_SIZE;
                    break;
                case UTP_FRAME_TYPE_MAX_STREAM_DATA:
                case UTP_FRAME_TYPE_STREAM_DATA_BLOCKED:
                    frame_length = UTP_FRAME_MAX_STREAM_DATA_SIZE;
                    break;
                case UTP_FRAME_TYPE_MAX_STREAMS:
                case UTP_FRAME_TYPE_STREAMS_BLOCKED:
                    frame_length = UTP_FRAME_STREAMS_LIMIT_SIZE;
                    break;
                default:
                    return UTP_INTERNAL_ERROR_PROTOCOL;
                }
                if (selected_count + (include_ack ? 1u : 0u) + 1u > UTP_PACKET_OUT_MAX_FRAMES ||
                    frame_length > (size_t)packet_capacity - UTP_PACKET_HEADER_SIZE - UTP_FRAME_STREAM_HEADER_SIZE -
                                       control_prefix_length) {
                    continue;
                }
                selected[selected_count++]  = slot;
                control_prefix_length      += frame_length;
            }
        }
    }
    stream = utp_connection_select_stream(connection);
    if (stream == NULL) {
        return UTP_INTERNAL_ERROR_OK;
    }
    {
        if (packet_capacity < UTP_PACKET_HEADER_SIZE + control_prefix_length + UTP_FRAME_STREAM_HEADER_SIZE) {
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
        max_data_length =
            (size_t)(packet_capacity - UTP_PACKET_HEADER_SIZE - control_prefix_length - UTP_FRAME_STREAM_HEADER_SIZE);
        if (max_data_length > connection->peer_max_data - connection->stream_data_sent_total) {
            max_data_length = (size_t)(connection->peer_max_data - connection->stream_data_sent_total);
        }
        if (connection->stream_scheduler_mode == 1u && max_data_length > stream->drr_deficit) {
            max_data_length = stream->drr_deficit;
        }
        packet = NULL;
        error  = utp_stream_build_frame_view_limited(stream, stream_header, sizeof(stream_header), max_data_length,
                                                     &stream_header_length, &stream_data, &stream_data_size,
                                                     &stream_offset, &fin);
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            return UTP_INTERNAL_ERROR_OK;
        }
        if (error == UTP_INTERNAL_ERROR_OK &&
            (uint64_t)stream_data_size > UINT64_MAX - connection->stream_data_sent_total) {
            error = UTP_INTERNAL_ERROR_OVERFLOW;
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        packet_length =
            UTP_PACKET_HEADER_SIZE + control_prefix_length + stream_header_length + (size_t)stream_data_size;
        raw_length = UTP_PACKET_HEADER_SIZE + control_prefix_length + stream_header_length;
        if (packet_length > packet_capacity || raw_length > UINT16_MAX) {
            error = UTP_INTERNAL_ERROR_LIMIT;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_packet_out_pool_acquire(&connection->packet_pool, (uint16_t)raw_length, &packet);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            packet->packet_number       = packet_number;
            packet->data_size           = (uint16_t)packet_length;
            packet->packet_type         = UTP_PACKET_TYPE_CTRL;
            packet->frame_types         = UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM);
            packet->control_prefix_size = (uint16_t)control_prefix_length;
            packet->transient_ack_size  = (uint16_t)ack_length;
            packet->stream_id           = stream->stream_id;
            packet->stream_offset       = stream_offset;
            packet->stream_data_size    = stream_data_size;
            packet->frame_meta_count    = (uint8_t)(selected_count + (include_ack ? 2u : 1u));
            if (include_ack) {
                packet->frame_types               |= UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK);
                packet->frame_meta[0].owner        = NULL;
                packet->frame_meta[0].value        = 0u;
                packet->frame_meta[0].offset       = UTP_PACKET_HEADER_SIZE;
                packet->frame_meta[0].length       = (uint16_t)ack_length;
                packet->frame_meta[0].generation   = 0u;
                packet->frame_meta[0].frame_type   = UTP_FRAME_TYPE_ACK;
                packet->frame_meta[0].frame_flags  = UTP_FRAME_META_TRANSIENT_ON_RETRANSMIT;
                memcpy(packet->raw_data + UTP_PACKET_HEADER_SIZE, ack_payload, ack_length);
            }
            for (control_index = 0u; control_index < selected_count && error == UTP_INTERNAL_ERROR_OK;
                 ++control_index) {
                const size_t meta_index   = control_index + (include_ack ? 1u : 0u);
                size_t       frame_length = 0u;
                size_t       offset       = ack_length;
                size_t       prior_index;

                for (prior_index = 0u; prior_index < control_index; ++prior_index) {
                    switch (selected[prior_index]->frame_type) {
                    case UTP_FRAME_TYPE_RESET_STREAM:
                        offset += UTP_FRAME_RESET_STREAM_SIZE;
                        break;
                    case UTP_FRAME_TYPE_MAX_DATA:
                    case UTP_FRAME_TYPE_DATA_BLOCKED:
                        offset += UTP_FRAME_MAX_DATA_SIZE;
                        break;
                    case UTP_FRAME_TYPE_MAX_STREAMS:
                    case UTP_FRAME_TYPE_STREAMS_BLOCKED:
                        offset += UTP_FRAME_STREAMS_LIMIT_SIZE;
                        break;
                    default:
                        offset += UTP_FRAME_MAX_STREAM_DATA_SIZE;
                        break;
                    }
                }
                error = utp_connection_encode_control_slot(selected[control_index],
                                                           packet->raw_data + UTP_PACKET_HEADER_SIZE + offset,
                                                           control_prefix_length - offset, &frame_length);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    packet->frame_meta[meta_index].owner        = selected[control_index];
                    packet->frame_meta[meta_index].value        = selected[control_index]->value;
                    packet->frame_meta[meta_index].offset       = (uint16_t)(UTP_PACKET_HEADER_SIZE + offset);
                    packet->frame_meta[meta_index].length       = (uint16_t)frame_length;
                    packet->frame_meta[meta_index].generation   = selected[control_index]->generation;
                    packet->frame_meta[meta_index].frame_type   = selected[control_index]->frame_type;
                    packet->frame_meta[meta_index].frame_flags  = UTP_FRAME_META_SEMANTIC_CONTROL;
                    packet->frame_types                        |= UTP_FRAME_BIT(selected[control_index]->frame_type);
                }
            }
            control_index                                 = selected_count + (include_ack ? 1u : 0u);
            packet->frame_meta[control_index].owner       = stream;
            packet->frame_meta[control_index].value       = 0u;
            packet->frame_meta[control_index].offset      = (uint16_t)(UTP_PACKET_HEADER_SIZE + control_prefix_length);
            packet->frame_meta[control_index].length      = (uint16_t)stream_header_length;
            packet->frame_meta[control_index].generation  = 0u;
            packet->frame_meta[control_index].frame_type  = UTP_FRAME_TYPE_STREAM;
            packet->frame_meta[control_index].frame_flags = fin ? UTP_FRAME_META_FIN : 0u;
            memcpy(packet->raw_data + UTP_PACKET_HEADER_SIZE + control_prefix_length, stream_header,
                   stream_header_length);
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
            error            = utp_stream_commit_built_frame(stream, stream_data_size, fin);
            stream_committed = error == UTP_INTERNAL_ERROR_OK;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            connection->stream_data_sent_total += (uint64_t)stream_data_size;
        }
        if (error == UTP_INTERNAL_ERROR_OK && connection->stream_scheduler_mode == 1u) {
            stream->drr_deficit -= stream_data_size > stream->drr_deficit ? stream->drr_deficit : stream_data_size;
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_send_control_schedule_packet(&connection->send_control, packet, true);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            for (control_index = 0u; control_index < selected_count; ++control_index) {
                selected[control_index]->pending = false;
                selected[control_index]->queued  = true;
            }
        }
        if (error != UTP_INTERNAL_ERROR_OK && packet != NULL && stream_committed) {
            (void)utp_stream_abandon_built_frame(stream, packet->stream_offset, packet->stream_data_size, fin);
            connection->stream_data_sent_total -= (uint64_t)packet->stream_data_size;
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

static utp_internal_error_t utp_connection_queue_mtu_probe(utp_connection_t* connection, uint64_t now_us, bool* queued)
{
    utp_packet_out_t*    packet = NULL;
    utp_internal_error_t error;
    uint64_t             packet_number;
    uint64_t             now_ms;
    uint16_t             probe_mtu;
    uint16_t             packet_size;
    uint16_t             plaintext_packet_size;
    size_t               payload_size;
    uint16_t             padding_length;

    if (connection == NULL || queued == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    *queued = false;
    if (connection->state != UTP_CONNECTION_STATE_CONNECTED) {
        return UTP_INTERNAL_ERROR_OK;
    }
    now_ms = now_us / UINT64_C(1000);
    if (!utp_mtu_discovery_should_probe(&connection->mtu_discovery, now_ms)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    probe_mtu   = utp_mtu_discovery_next_probe_mtu(&connection->mtu_discovery);
    packet_size = utp_mtu_packet_size_from_mtu(probe_mtu, connection->peer.family);
    if (packet_size <= UTP_PACKET_HEADER_SIZE + 1u || packet_size > connection->packet_capacity) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    plaintext_packet_size = connection->crypto_ready ? (uint16_t)(packet_size - UTP_CRYPTO_AEAD_TAG_SIZE) : packet_size;
    payload_size          = (size_t)plaintext_packet_size - UTP_PACKET_HEADER_SIZE;
    if (payload_size <= 1u || payload_size - 1u < UTP_FRAME_PADDING_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    padding_length = (uint16_t)(payload_size - 1u - UTP_FRAME_PADDING_HEADER_SIZE);
    error          = utp_packet_out_pool_acquire(&connection->packet_pool, plaintext_packet_size, &packet);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_send_control_allocate_packet_number(&connection->send_control, &packet_number);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        packet->packet_number  = packet_number;
        packet->data_size      = plaintext_packet_size;
        packet->packet_type    = UTP_PACKET_TYPE_CTRL;
        packet->frame_types    = UTP_FRAME_BIT(UTP_FRAME_TYPE_PING) | UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING);
        packet->po_flags      |= UTP_PO_MTU_PROBE;
        packet->raw_data[UTP_PACKET_HEADER_SIZE] = UTP_FRAME_TYPE_PING;
        error =
            utp_frame_padding_encode(packet->raw_data + UTP_PACKET_HEADER_SIZE + 1u, payload_size - 1u, padding_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_encode_header(connection, packet, packet->packet_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_send_control_schedule_packet(&connection->send_control, packet, true);
    }
    if (error != UTP_INTERNAL_ERROR_OK && packet != NULL) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *queued = true;
    }
    return error;
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

static void utp_connection_cleanup_stream_node(utp_hash_node_t* node, void* user_data)
{
    utp_stream_t* stream = utp_connection_stream_from_node(node);

    (void)user_data;
    utp_stream_cleanup(stream);
    utp_allocator_free(NULL, stream);
}

static void utp_connection_cleanup_control_slot_node(utp_hash_node_t* node, void* user_data)
{
    (void)user_data;
    utp_allocator_free(NULL, utp_connection_control_slot_from_node(node));
}

static void utp_connection_cleanup_pending_max_stream_data_node(utp_hash_node_t* node, void* user_data)
{
    (void)user_data;
    utp_allocator_free(NULL, utp_connection_pending_max_stream_data_from_node(node));
}

utp_internal_error_t utp_connection_init(utp_connection_t* connection, utp_connection_role_t role, uint32_t local_cid,
                                         uint32_t peer_cid, const utp_address_t* peer, size_t packet_limit,
                                         uint16_t packet_capacity)
{
    const utp_packet_out_bucket_config_t buckets[] = {
        {1280u, packet_limit}, {1500u, packet_limit},      {4096u, packet_limit},
        {9000u, packet_limit}, {UINT16_MAX, packet_limit},
    };
    utp_internal_error_t error;
    size_t               index;

    if (connection == NULL || peer == NULL ||
        (role != UTP_CONNECTION_ROLE_ACTIVE && role != UTP_CONNECTION_ROLE_PASSIVE) || local_cid == 0u ||
        (role == UTP_CONNECTION_ROLE_PASSIVE && peer_cid == 0u) || packet_limit == 0u ||
        packet_capacity < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection->tx_aead = (utp_crypto_aead_t){0};
    connection->rx_aead = (utp_crypto_aead_t){0};
    utp_crypto_key_pair_clear(&connection->crypto_key_pair);
    for (index = 0u; index < UTP_STREAM_TYPES; ++index) {
        connection->next_stream_id[index] = 0u;
    }
    connection->streams                      = (utp_hash_table_t){0};
    connection->control_slots                = (utp_hash_table_t){0};
    connection->pending_peer_max_stream_data = (utp_hash_table_t){0};
    error                                    = utp_hash_table_init(&connection->streams, NULL, SIZE_MAX);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&connection->control_slots, NULL, SIZE_MAX);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&connection->pending_peer_max_stream_data, NULL, SIZE_MAX);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_hash_table_cleanup(&connection->streams, utp_connection_cleanup_stream_node, NULL);
        utp_hash_table_cleanup(&connection->control_slots, utp_connection_cleanup_control_slot_node, NULL);
        utp_hash_table_cleanup(&connection->pending_peer_max_stream_data,
                               utp_connection_cleanup_pending_max_stream_data_node, NULL);
        return error;
    }
    connection->context                                                 = NULL;
    connection->rx_bytes                                                = 0u;
    connection->tx_bytes                                                = 0u;
    connection->peer_handshake_packet_number                            = 0u;
    connection->retransmission_deadline_us                              = 0u;
    connection->close_deadline_us                                       = 0u;
    connection->close_last_sent_us                                      = 0u;
    connection->close_pto_us                                            = UTP_CONNECTION_CLOSE_PTO_DEFAULT_US;
    connection->keepalive_deadline_us                                   = 0u;
    connection->last_peer_activity_us                                   = 0u;
    connection->close_error_code                                        = 0u;
    connection->peer_close_error_code                                   = 0u;
    connection->peer_close_reason_length                                = 0u;
    connection->path_challenge_deadline_us                              = 0u;
    connection->candidate_rx_bytes                                      = 0u;
    connection->candidate_tx_bytes                                      = 0u;
    connection->candidate_queued_bytes                                  = 0u;
    connection->path_validation_generation                              = 0u;
    connection->path_challenge_retry_count                              = 0u;
    connection->keepalive_missed_probes                                 = 0u;
    connection->close_pending                                           = false;
    connection->udp_write_pending                                       = false;
    connection->local_close_started                                     = false;
    connection->peer_close_received                                     = false;
    connection->path_challenge_pending                                  = false;
    connection->crypto_type                                             = 0u;
    connection->crypto_configured                                       = false;
    connection->crypto_ready                                            = false;
    connection->peer_close_reason                                       = NULL;
    connection->stream_scheduler_mode                                   = 0u;
    connection->stream_scheduler_cursor                                 = 0u;
    connection->local_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL]  = UTP_CONNECTION_DEFAULT_MAX_STREAMS_BIDI;
    connection->local_max_streams[UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL] = UTP_CONNECTION_DEFAULT_MAX_STREAMS_UNI;
    connection->peer_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL]   = UTP_CONNECTION_DEFAULT_MAX_STREAMS_BIDI;
    connection->peer_max_streams[UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL]  = UTP_CONNECTION_DEFAULT_MAX_STREAMS_UNI;
    error = utp_packet_out_pool_init(&connection->packet_pool, NULL, packet_limit, buckets,
                                     sizeof(buckets) / sizeof(buckets[0]));
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_send_control_init(&connection->send_control, packet_limit, UTP_CONNECTION_RETRANSMITTABLE_FRAMES, 16u,
                                  (uint64_t)UTP_CONNECTION_MAX_ACK_DELAY_MS * UINT64_C(1000));
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_bbr_init(&connection->congestion, NULL);
        utp_send_control_set_congestion(&connection->send_control, utp_bbr_as_congestion(&connection->congestion));
        utp_send_control_set_pacing_enabled(&connection->send_control, true);
    }
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
    connection->candidate_peer                   = *peer;
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
    connection->state = role == UTP_CONNECTION_ROLE_PASSIVE ? UTP_CONNECTION_STATE_CONNECTED : UTP_CONNECTION_STATE_NEW;
    connection->path_state = UTP_CONNECTION_PATH_STATE_VALIDATED;
    utp_mtu_discovery_init(&connection->mtu_discovery, NULL, peer->family);
    {
        uint32_t local_bit = utp_connection_local_stream_initiator_bit(connection);

        connection->next_stream_id[local_bit]                             = local_bit;
        connection->next_stream_id[local_bit | UTP_STREAM_UNIDIRECTIONAL] = local_bit | UTP_STREAM_UNIDIRECTIONAL;
    }
    utp_send_control_set_connected(&connection->send_control, role == UTP_CONNECTION_ROLE_PASSIVE);
    return UTP_INTERNAL_ERROR_OK;
}

void utp_connection_set_mtu_config(utp_connection_t* connection, const utp_mtu_config_t* config)
{
    if (connection != NULL) {
        utp_mtu_discovery_init(&connection->mtu_discovery, config, connection->peer.family);
    }
}

utp_internal_error_t utp_connection_configure_crypto(utp_connection_t* connection, uint8_t crypto_type)
{
    utp_internal_error_t error;

    if (connection == NULL || connection->role != UTP_CONNECTION_ROLE_ACTIVE || connection->crypto_configured ||
        crypto_type > UTP_FRAME_CRYPTO_TYPE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_crypto_key_pair_generate(&connection->crypto_key_pair);
    if (error == UTP_INTERNAL_ERROR_OK) {
        connection->crypto_type       = crypto_type;
        connection->crypto_configured = true;
    }
    return error;
}

utp_internal_error_t utp_connection_encode_crypto(const utp_connection_t* connection, uint8_t* buffer, size_t capacity)
{
    utp_frame_crypto_t crypto;

    if (connection == NULL || buffer == NULL || !connection->crypto_configured) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    crypto.crypto_type = connection->crypto_type;
    memcpy(crypto.ephemeral_public_key, connection->crypto_key_pair.public_key, sizeof(crypto.ephemeral_public_key));
    return utp_frame_crypto_encode(buffer, capacity, &crypto);
}

utp_internal_error_t utp_connection_adopt_crypto(utp_connection_t* connection, uint8_t crypto_type,
                                                 utp_crypto_aead_t* tx, utp_crypto_aead_t* rx)
{
    if (connection == NULL || tx == NULL || rx == NULL || connection->crypto_configured ||
        tx->provider_context == NULL || rx->provider_context == NULL ||
        crypto_type > UTP_FRAME_CRYPTO_TYPE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection->tx_aead           = *tx;
    connection->rx_aead           = *rx;
    *tx                           = (utp_crypto_aead_t){0};
    *rx                           = (utp_crypto_aead_t){0};
    connection->crypto_type       = crypto_type;
    connection->crypto_configured = true;
    connection->crypto_ready      = true;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_connection_encode_packet_wire(const utp_connection_t* connection,
                                                       const utp_packet_out_t* packet, uint8_t* buffer, size_t capacity,
                                                       size_t* out_length)
{
    utp_packet_header_t  header;
    size_t               plaintext_length;
    size_t               ciphertext_length;
    utp_internal_error_t error;

    if (out_length != NULL) {
        *out_length = 0u;
    }
    if (connection == NULL || packet == NULL || buffer == NULL || out_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & UTP_PO_ENCRYPTED) == 0u) {
        return utp_packet_out_flatten(packet, buffer, capacity, out_length);
    }
    if (!connection->crypto_ready || packet->encrypt_data_size != packet->data_size + UTP_CRYPTO_AEAD_TAG_SIZE ||
        packet->encrypt_data_size > capacity) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    error = utp_packet_out_flatten(packet, buffer, capacity - UTP_CRYPTO_AEAD_TAG_SIZE, &plaintext_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_proto_decode_header(&header, buffer, plaintext_length);
    if (error != UTP_INTERNAL_ERROR_OK || plaintext_length != UTP_PACKET_HEADER_SIZE + header.payload_length) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    header.payload_length = (uint16_t)(header.payload_length + UTP_CRYPTO_AEAD_TAG_SIZE);
    error                 = utp_proto_encode_header(buffer, capacity, &header);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_aead_seal(&connection->tx_aead, packet->packet_number, buffer + UTP_PACKET_HEADER_SIZE,
                                     plaintext_length - UTP_PACKET_HEADER_SIZE, buffer, UTP_PACKET_HEADER_SIZE,
                                     buffer + UTP_PACKET_HEADER_SIZE, capacity - UTP_PACKET_HEADER_SIZE,
                                     &ciphertext_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = UTP_PACKET_HEADER_SIZE + ciphertext_length;
        if (*out_length != packet->encrypt_data_size) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
    }
    return error;
}

void utp_connection_cleanup(utp_connection_t* connection)
{
    size_t index;

    if (connection == NULL) {
        return;
    }
    utp_crypto_aead_cleanup(&connection->tx_aead);
    utp_crypto_aead_cleanup(&connection->rx_aead);
    utp_crypto_key_pair_clear(&connection->crypto_key_pair);
    utp_send_control_cleanup(&connection->send_control);
    utp_receive_history_cleanup(&connection->receive_history);
    utp_packet_out_pool_cleanup(&connection->packet_pool);
    utp_hash_table_cleanup(&connection->streams, utp_connection_cleanup_stream_node, NULL);
    utp_hash_table_cleanup(&connection->control_slots, utp_connection_cleanup_control_slot_node, NULL);
    utp_hash_table_cleanup(&connection->pending_peer_max_stream_data,
                           utp_connection_cleanup_pending_max_stream_data_node, NULL);
    for (index = 0u; index < UTP_STREAM_TYPES; ++index) {
        connection->next_stream_id[index] = 0u;
    }
    connection->context                                                 = NULL;
    connection->local_cid                                               = 0u;
    connection->peer_cid                                                = 0u;
    connection->peer_max_data                                           = 0u;
    connection->local_max_data_advertised                               = 0u;
    connection->stream_data_sent_total                                  = 0u;
    connection->local_stream_data_received_total                        = 0u;
    connection->local_stream_data_consumed_total                        = 0u;
    connection->last_max_data_sent_us                                   = 0u;
    connection->last_data_blocked_sent_us                               = 0u;
    connection->packet_capacity                                         = 0u;
    connection->recv_reassembly_memory_bytes                            = 0u;
    connection->local_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL]  = 0u;
    connection->local_max_streams[UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL] = 0u;
    connection->peer_max_streams[UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL]   = 0u;
    connection->peer_max_streams[UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL]  = 0u;
    connection->recv_reassembly_fragment_count                          = 0u;
    connection->rx_bytes                                                = 0u;
    connection->tx_bytes                                                = 0u;
    connection->peer_handshake_packet_number                            = 0u;
    connection->retransmission_deadline_us                              = 0u;
    connection->close_deadline_us                                       = 0u;
    connection->close_last_sent_us                                      = 0u;
    connection->close_pto_us                                            = 0u;
    connection->keepalive_deadline_us                                   = 0u;
    connection->last_peer_activity_us                                   = 0u;
    connection->close_error_code                                        = 0u;
    connection->peer_close_error_code                                   = 0u;
    connection->peer_close_reason_length                                = 0u;
    connection->path_challenge_deadline_us                              = 0u;
    connection->candidate_rx_bytes                                      = 0u;
    connection->candidate_tx_bytes                                      = 0u;
    connection->candidate_queued_bytes                                  = 0u;
    connection->path_validation_generation                              = 0u;
    connection->path_challenge_retry_count                              = 0u;
    connection->keepalive_missed_probes                                 = 0u;
    connection->close_pending                                           = false;
    connection->udp_write_pending                                       = false;
    connection->local_close_started                                     = false;
    connection->peer_close_received                                     = false;
    connection->path_challenge_pending                                  = false;
    connection->crypto_type                                             = 0u;
    connection->crypto_configured                                       = false;
    connection->crypto_ready                                            = false;
    connection->peer_close_reason                                       = NULL;
    connection->role                                                    = UTP_CONNECTION_ROLE_ACTIVE;
    connection->state                                                   = UTP_CONNECTION_STATE_CLOSED;
    connection->path_state                                              = UTP_CONNECTION_PATH_STATE_UNKNOWN;
}

utp_internal_error_t utp_connection_queue_packet(utp_connection_t* connection, uint8_t packet_type,
                                                 const uint8_t* payload, size_t payload_length, bool track_on_send)
{
    utp_packet_out_t*    packet;
    utp_internal_error_t error;
    uint64_t             packet_number;
    uint32_t             frame_types;
    size_t               packet_length;
    uint16_t             packet_capacity;

    if (connection == NULL || packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE ||
        !utp_connection_packet_type_is_valid(packet_type) || (payload == NULL && payload_length != 0u) ||
        (connection->state == UTP_CONNECTION_STATE_CLOSING && packet_type != UTP_PACKET_TYPE_CONNECTION_CLOSE) ||
        connection->state == UTP_CONNECTION_STATE_DRAINING || connection->state == UTP_CONNECTION_STATE_CLOSED ||
        payload_length > UINT16_MAX) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((connection->state == UTP_CONNECTION_STATE_NEW &&
         (connection->role != UTP_CONNECTION_ROLE_ACTIVE || packet_type != UTP_PACKET_TYPE_INITIAL)) ||
        (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT && packet_type == UTP_PACKET_TYPE_INITIAL)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    packet_length   = UTP_PACKET_HEADER_SIZE + payload_length;
    packet_capacity = utp_connection_plaintext_packet_capacity(connection);
    if (packet_length > packet_capacity) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    error = utp_frame_scan(payload, payload_length, &frame_types);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE &&
        (frame_types != UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE) || track_on_send)) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (packet_type != UTP_PACKET_TYPE_CONNECTION_CLOSE &&
        (frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE)) != 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
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
    if (error == UTP_INTERNAL_ERROR_OK && packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE) {
        utp_frame_connection_close_t close;

        error = utp_frame_connection_close_decode(&close, payload, payload_length);
        if (error == UTP_INTERNAL_ERROR_OK) {
            connection->state                      = UTP_CONNECTION_STATE_CLOSING;
            connection->close_error_code           = close.error_code;
            connection->close_pending              = true;
            connection->local_close_started        = true;
            connection->retransmission_deadline_us = 0u;
            utp_send_control_set_connected(&connection->send_control, false);
        }
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    }
    return error;
}

utp_internal_error_t utp_connection_queue_close(utp_connection_t* connection, uint16_t error_code)
{
    if (connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (connection->state == UTP_CONNECTION_STATE_CLOSED || connection->state == UTP_CONNECTION_STATE_DRAINING) {
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    // close 是幂等屏障：首次调用后不再接受业务发送，也不重复生成新的 close。
    if (connection->state == UTP_CONNECTION_STATE_CLOSING) {
        if (connection->local_close_started) {
            return UTP_INTERNAL_ERROR_OK;
        }
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    return utp_connection_prepare_close_packet(connection, error_code, true);
}

utp_internal_error_t utp_connection_prepare_destroy_close(utp_connection_t* connection)
{
    if (connection == NULL || connection->local_cid == 0u || connection->peer_cid == 0u ||
        connection->state == UTP_CONNECTION_STATE_CLOSED || connection->state == UTP_CONNECTION_STATE_DRAINING) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    return utp_connection_prepare_close_packet(connection, 0u, true);
}

static utp_internal_error_t utp_connection_rearm_local_close_packet(utp_connection_t* connection)
{
    if (connection == NULL || connection->state != UTP_CONNECTION_STATE_CLOSING || !connection->local_close_started ||
        connection->peer_close_received || connection->close_pending) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    return utp_connection_prepare_close_packet(connection, connection->close_error_code, false);
}

static bool utp_connection_packet_stream_is_reset(const utp_connection_t* connection, const utp_packet_out_t* packet)
{
    const utp_stream_t* stream;

    if (connection == NULL || packet == NULL || (packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) == 0u) {
        return false;
    }
    stream = utp_connection_find_stream_internal((utp_connection_t*)connection, packet->stream_id);
    return stream != NULL && stream->used && stream->reset;
}

utp_packet_out_t* utp_connection_next_packet_to_send_at(utp_connection_t* connection, uint64_t now_us)
{
    utp_packet_out_t* packet;
    uint64_t          packet_number;
    bool              queued_control       = false;
    bool              queued_stream_packet = false;
    bool              include_ack;
    bool              has_pending_controls;

    if (connection == NULL) {
        return NULL;
    }
    if (connection->state == UTP_CONNECTION_STATE_DRAINING || connection->state == UTP_CONNECTION_STATE_CLOSED) {
        return NULL;
    }
    if (connection->state == UTP_CONNECTION_STATE_CLOSING) {
        while ((packet = utp_send_control_next_scheduled(&connection->send_control)) != NULL) {
            utp_connection_on_packet_abandoned(connection, packet);
            utp_packet_out_pool_release(&connection->packet_pool, packet);
        }
        return connection->close_pending ? &connection->close_packet : NULL;
    }
    utp_connection_update_completed_peer_streams(connection);
    packet = utp_connection_next_scheduled_admitted(connection);
    if (packet != NULL) {
        return packet;
    }
    if (utp_send_control_peek_scheduled(&connection->send_control) != NULL) {
        if (now_us != 0u && utp_ack_scheduler_pending_count(&connection->ack_scheduler) != 0u &&
            utp_connection_queue_control_packet(connection, now_us, true, false, true, &queued_control) ==
                UTP_INTERNAL_ERROR_OK &&
            queued_control) {
            return utp_connection_next_scheduled_admitted(connection);
        }
        return NULL;
    }
    {
        bool flow_control_due = false;

        (void)utp_connection_queue_pending_flow_control(connection, now_us, &flow_control_due);
    }
    include_ack          = now_us != 0u && utp_ack_scheduler_pending_count(&connection->ack_scheduler) != 0u;
    has_pending_controls = utp_connection_has_pending_controls(connection);
    if (has_pending_controls &&
        utp_connection_queue_next_stream_packet(connection, now_us, include_ack, true, &queued_stream_packet) ==
            UTP_INTERNAL_ERROR_OK &&
        queued_stream_packet) {
        packet = utp_connection_next_scheduled_admitted(connection);
        if (packet != NULL || utp_send_control_peek_scheduled(&connection->send_control) != NULL) {
            return packet;
        }
    }
    if (has_pending_controls &&
        utp_connection_queue_control_packet(connection, now_us, include_ack, true, false, &queued_control) ==
            UTP_INTERNAL_ERROR_OK &&
        queued_control) {
        return utp_connection_next_scheduled_admitted(connection);
    }
    if (include_ack &&
        utp_connection_queue_next_stream_packet(connection, now_us, true, false, &queued_stream_packet) ==
            UTP_INTERNAL_ERROR_OK &&
        queued_stream_packet) {
        packet = utp_connection_next_scheduled_admitted(connection);
        if (packet != NULL || utp_send_control_peek_scheduled(&connection->send_control) != NULL) {
            return packet;
        }
    }
    if (include_ack &&
        utp_connection_queue_control_packet(connection, now_us, true, false, false, &queued_control) ==
            UTP_INTERNAL_ERROR_OK &&
        queued_control) {
        return utp_connection_next_scheduled_admitted(connection);
    }
    while ((packet = utp_send_control_next_lost(&connection->send_control)) != NULL) {
        utp_connection_requeue_lost_controls(packet);
        if (packet->control_prefix_size != 0u &&
            utp_packet_out_strip_prefix(packet, packet->control_prefix_size) != UTP_INTERNAL_ERROR_OK) {
            utp_packet_out_pool_release(&connection->packet_pool, packet);
            continue;
        }
        if (packet->frame_types == 0u || utp_connection_packet_stream_is_reset(connection, packet)) {
            utp_packet_out_pool_release(&connection->packet_pool, packet);
            continue;
        }
        if (!utp_connection_can_transmit_packet(connection, packet)) {
            (void)utp_send_control_reschedule_lost(&connection->send_control, packet);
            return NULL;
        }
        goto rewrite_packet_number;
    }
    has_pending_controls = utp_connection_has_pending_controls(connection);
    if (utp_connection_queue_next_stream_packet(connection, now_us, false, has_pending_controls,
                                                &queued_stream_packet) == UTP_INTERNAL_ERROR_OK &&
        queued_stream_packet) {
        packet = utp_connection_next_scheduled_admitted(connection);
        if (packet != NULL || utp_send_control_peek_scheduled(&connection->send_control) != NULL) {
            return packet;
        }
    }
    has_pending_controls = utp_connection_has_pending_controls(connection);
    if (has_pending_controls &&
        utp_connection_queue_control_packet(connection, now_us, false, true, false, &queued_control) ==
            UTP_INTERNAL_ERROR_OK &&
        queued_control) {
        return utp_connection_next_scheduled_admitted(connection);
    }
    if (utp_connection_queue_next_stream_packet(connection, now_us, false, false, &queued_stream_packet) ==
            UTP_INTERNAL_ERROR_OK &&
        queued_stream_packet) {
        packet = utp_connection_next_scheduled_admitted(connection);
        if (packet != NULL || utp_send_control_peek_scheduled(&connection->send_control) != NULL) {
            return packet;
        }
    }
    if (utp_connection_queue_mtu_probe(connection, now_us, &queued_control) == UTP_INTERNAL_ERROR_OK &&
        queued_control) {
        return utp_connection_next_scheduled_admitted(connection);
    }
    return NULL;

rewrite_packet_number:
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
    if ((packet->po_flags & UTP_PO_MTU_PROBE) != 0u) {
        (void)utp_mtu_discovery_on_probe_sent(
            &connection->mtu_discovery, packet->packet_number,
            utp_mtu_from_packet_size(utp_connection_packet_wire_size(packet), connection->peer.family),
            now_us / UINT64_C(1000));
    }
    connection->tx_bytes += utp_connection_packet_wire_size(packet);
    if ((packet->po_flags & UTP_PO_PATH_VALIDATION) != 0u &&
        packet->path_validation_generation == connection->path_validation_generation) {
        if ((uint64_t)utp_connection_packet_wire_size(packet) <= connection->candidate_queued_bytes) {
            connection->candidate_queued_bytes -= (uint64_t)utp_connection_packet_wire_size(packet);
        }
        if ((uint64_t)utp_connection_packet_wire_size(packet) <= UINT64_MAX - connection->candidate_tx_bytes) {
            connection->candidate_tx_bytes += (uint64_t)utp_connection_packet_wire_size(packet);
        }
    }
    utp_connection_commit_sent_controls(connection, packet, now_us);
    if (packet->transient_ack_size != 0u) {
        utp_ack_scheduler_on_ack_sent(&connection->ack_scheduler);
    }
    if (packet->packet_type == UTP_PACKET_TYPE_INITIAL && connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        connection->state == UTP_CONNECTION_STATE_NEW) {
        connection->state = UTP_CONNECTION_STATE_INITIAL_SENT;
    } else if (packet->packet_type == UTP_PACKET_TYPE_CONNECTION_CLOSE ||
               (packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_CONNECTION_CLOSE)) != 0u) {
        connection->close_last_sent_us = now_us;
        if (connection->peer_close_received) {
            utp_connection_enter_draining(connection, now_us);
        } else {
            connection->state                      = UTP_CONNECTION_STATE_CLOSING;
            connection->close_pending              = false;
            connection->close_pto_us               = utp_connection_close_pto(connection);
            connection->close_deadline_us          = now_us > UINT64_MAX - 3u * connection->close_pto_us
                                                         ? UINT64_MAX
                                                         : now_us + 3u * connection->close_pto_us;
            connection->retransmission_deadline_us = 0u;
            utp_send_control_set_connected(&connection->send_control, false);
        }
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
    if (!tracked && !utp_connection_is_close_packet(connection, packet)) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    } else if (utp_send_control_unacked_packet_count(&connection->send_control) != 0u &&
               connection->retransmission_deadline_us == 0u) {
        error = utp_connection_ensure_retransmission_deadline(connection, now_us);
    }
    return UTP_INTERNAL_ERROR_OK;
}

static bool utp_connection_is_mtu_write_error(utp_internal_error_t error)
{
    if (error == UTP_INTERNAL_ERROR_OVERFLOW) {
        return true;
    }
#ifdef EMSGSIZE
    return utp_internal_error_to_errno(error) == EMSGSIZE;
#else
    return false;
#endif
}

void utp_connection_on_packet_send_error(utp_connection_t* connection, const utp_packet_out_t* packet,
                                         utp_internal_error_t error, uint64_t now_us)
{
    if (connection == NULL || packet == NULL || now_us == 0u || (packet->po_flags & UTP_PO_MTU_PROBE) == 0u ||
        !utp_connection_is_mtu_write_error(error)) {
        return;
    }
    (void)utp_mtu_discovery_on_probe_send_failed(
        &connection->mtu_discovery,
        utp_mtu_from_packet_size(utp_connection_packet_wire_size(packet), connection->peer.family),
        now_us / UINT64_C(1000));
}

void utp_connection_on_packet_abandoned(utp_connection_t* connection, const utp_packet_out_t* packet)
{
    size_t index;
    bool   stream_fin = false;

    if (connection == NULL || packet == NULL) {
        return;
    }
    if ((packet->po_flags & UTP_PO_PATH_VALIDATION) != 0u &&
        packet->path_validation_generation == connection->path_validation_generation &&
        (uint64_t)utp_connection_packet_wire_size(packet) <= connection->candidate_queued_bytes) {
        connection->candidate_queued_bytes -= (uint64_t)utp_connection_packet_wire_size(packet);
    }
    for (index = 0u; index < packet->frame_meta_count; ++index) {
        const utp_frame_meta_info_t* meta = &packet->frame_meta[index];

        if ((meta->frame_flags & UTP_FRAME_META_SEMANTIC_CONTROL) != 0u && meta->owner != NULL) {
            utp_connection_control_slot_t* slot = meta->owner;

            if (slot->queued) {
                slot->queued = false;
                if (slot->generation == meta->generation) {
                    slot->pending = true;
                }
            }
        }
        if (meta->frame_type == UTP_FRAME_TYPE_STREAM && (meta->frame_flags & UTP_FRAME_META_FIN) != 0u) {
            stream_fin = true;
        }
    }
    if ((packet->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u) {
        utp_stream_t* stream = utp_connection_find_stream_internal(connection, packet->stream_id);

        if (stream != NULL &&
            utp_stream_abandon_built_frame(stream, packet->stream_offset, packet->stream_data_size, stream_fin) ==
                UTP_INTERNAL_ERROR_OK &&
            (uint64_t)packet->stream_data_size <= connection->stream_data_sent_total) {
            connection->stream_data_sent_total -= (uint64_t)packet->stream_data_size;
        }
    }
}

static utp_internal_error_t utp_connection_on_packet_received_internal(utp_connection_t* connection,
                                                                       const uint8_t* packet, size_t packet_length,
                                                                       size_t               wire_packet_length,
                                                                       utp_packet_in_t*     packet_in,
                                                                       const utp_address_t* peer, uint64_t now_us)
{
    utp_packet_view_t    view;
    utp_internal_error_t error;
    uint64_t             largest_before;
    size_t               offset;
    bool                 handshake_done;
    bool                 ack_progress;
    bool                 candidate_path;
    bool                 peer_close;

    if (connection == NULL || packet == NULL || peer == NULL || now_us == 0u || wire_packet_length < packet_length) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_packet_view_decode(&view, packet, packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + view.payload_length ||
        !utp_connection_packet_type_is_valid(view.header.type) || view.header.packet_number == 0u ||
        view.header.dcid != connection->local_cid) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (view.header.type == UTP_PACKET_TYPE_INITIAL && connection->crypto_configured) {
        return utp_connection_untrusted_packet_error(connection);
    }
    if (view.header.type == UTP_PACKET_TYPE_HANDSHAKE) {
        error = utp_connection_validate_plaintext_handshake(connection, &view);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if (connection->role == UTP_CONNECTION_ROLE_ACTIVE && connection->state == UTP_CONNECTION_STATE_INITIAL_SENT &&
        view.header.type == UTP_PACKET_TYPE_HANDSHAKE && connection->peer_cid == 0u) {
        if (view.header.scid == 0u) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        connection->peer_cid = view.header.scid;
    } else if (view.header.scid != connection->peer_cid) {
        return utp_connection_untrusted_packet_error(connection);
    }
    candidate_path = !utp_address_equal(&connection->peer, peer);
    if (candidate_path && !utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (connection->state == UTP_CONNECTION_STATE_DRAINING) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (connection->state == UTP_CONNECTION_STATE_CLOSING) {
        bool   peer_close_newly_received = false;
        size_t close_offset              = 0u;

        while (close_offset < view.payload_length) {
            const uint8_t* frame;
            uint8_t        frame_type;
            size_t         frame_length;

            error = utp_packet_view_next_frame(&view, &close_offset, &frame_type, &frame, &frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (frame_type == UTP_FRAME_TYPE_CONNECTION_CLOSE) {
                utp_frame_connection_close_t close;

                error = utp_frame_connection_close_decode(&close, frame, frame_length);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    return error;
                }
                if (utp_connection_record_peer_close(connection, &close)) {
                    peer_close_newly_received = true;
                }
            }
        }
        if (peer_close_newly_received) {
            if (!connection->close_pending) {
                utp_connection_enter_draining(connection, now_us);
            }
        } else if (!connection->close_pending && connection->close_last_sent_us != 0u &&
                   now_us - connection->close_last_sent_us >= connection->close_pto_us) {
            (void)utp_connection_rearm_local_close_packet(connection);
        }
        return UTP_INTERNAL_ERROR_OK;
    }
    if (candidate_path) {
        if (connection->path_state != UTP_CONNECTION_PATH_STATE_VALIDATING ||
            !utp_address_equal(&connection->candidate_peer, peer)) {
            utp_connection_begin_path_validation(connection, peer, wire_packet_length);
        } else if ((uint64_t)wire_packet_length > UINT64_MAX - connection->candidate_rx_bytes) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        } else {
            connection->candidate_rx_bytes += (uint64_t)wire_packet_length;
        }
        error = utp_connection_send_path_challenge(connection, now_us);
        if (error != UTP_INTERNAL_ERROR_OK && error != UTP_INTERNAL_ERROR_STATE) {
            return error;
        }
    }
    largest_before = utp_receive_history_largest(&connection->receive_history);
    if (!candidate_path) {
        error = utp_receive_history_insert(&connection->receive_history, view.header.packet_number, now_us);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    offset         = 0u;
    handshake_done = false;
    ack_progress   = false;
    peer_close     = false;
    while (offset < view.payload_length) {
        const uint8_t* frame;
        uint8_t        frame_type;
        size_t         frame_length;

        error = utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (candidate_path) {
            if (frame_type == UTP_FRAME_TYPE_PATH_CHALLENGE) {
                const bool charge_candidate = connection->path_state == UTP_CONNECTION_PATH_STATE_VALIDATING &&
                                              utp_address_equal(peer, &connection->candidate_peer);

                error = utp_connection_handle_path_challenge(connection, frame, frame_length, peer, charge_candidate);
            } else if (frame_type == UTP_FRAME_TYPE_PATH_RESPONSE) {
                error = utp_connection_handle_path_response(connection, frame, frame_length, peer, true);
            } else if (frame_type == UTP_FRAME_TYPE_CONNECTION_CLOSE) {
                utp_frame_connection_close_t close;

                error = utp_frame_connection_close_decode(&close, frame, frame_length);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    if (utp_connection_record_peer_close(connection, &close)) {
                        peer_close = true;
                    }
                }
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            continue;
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
            utp_connection_process_acknowledged_packets(connection, &acknowledged, now_us);
            utp_connection_release_queue(connection, &acknowledged);
            utp_connection_process_detected_losses(connection, now_us);
            utp_connection_release_discarded_packets(connection, now_us);
        } else if (frame_type == UTP_FRAME_TYPE_CRYPTO) {
            utp_frame_crypto_t crypto;

            if (view.header.type != UTP_PACKET_TYPE_INITIAL && view.header.type != UTP_PACKET_TYPE_HANDSHAKE) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            error = utp_frame_crypto_decode(&crypto, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK || !connection->crypto_configured ||
                crypto.crypto_type != connection->crypto_type) {
                return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
            }
            if (!connection->crypto_ready) {
                if (connection->role != UTP_CONNECTION_ROLE_ACTIVE || connection->peer_cid == 0u) {
                    return UTP_INTERNAL_ERROR_PROTOCOL;
                }
                error = utp_crypto_create_directional_aead(
                    &connection->crypto_key_pair, crypto.ephemeral_public_key, connection->local_cid,
                    connection->peer_cid, crypto.crypto_type, true, &connection->tx_aead, &connection->rx_aead);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    return error;
                }
                memcpy(connection->peer_crypto_public_key, crypto.ephemeral_public_key,
                       sizeof(connection->peer_crypto_public_key));
                connection->crypto_ready = true;
            } else if (memcmp(connection->peer_crypto_public_key, crypto.ephemeral_public_key,
                              sizeof(connection->peer_crypto_public_key)) != 0) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
        } else if (frame_type == UTP_FRAME_TYPE_HANDSHAKE_DONE) {
            handshake_done = true;
        } else if (frame_type == UTP_FRAME_TYPE_CONNECTION_CLOSE) {
            utp_frame_connection_close_t close;

            error = utp_frame_connection_close_decode(&close, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (utp_connection_record_peer_close(connection, &close)) {
                peer_close = true;
            }
        } else if (frame_type == UTP_FRAME_TYPE_PATH_CHALLENGE) {
            error = utp_connection_handle_path_challenge(connection, frame, frame_length, peer, false);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        } else if (frame_type == UTP_FRAME_TYPE_PATH_RESPONSE) {
            error = utp_connection_handle_path_response(connection, frame, frame_length, peer, false);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
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
                error = utp_stream_on_reset(stream, reset.error_code, true);
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        } else if (frame_type == UTP_FRAME_TYPE_MAX_STREAMS) {
            utp_frame_streams_limit_t maximum;

            error = utp_frame_max_streams_decode(&maximum, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (maximum.stream_limit > connection->peer_max_streams[maximum.stream_type]) {
                connection->peer_max_streams[maximum.stream_type] = maximum.stream_limit;
            }
        } else if (frame_type == UTP_FRAME_TYPE_STREAMS_BLOCKED) {
            utp_frame_streams_limit_t blocked;

            error = utp_frame_streams_blocked_decode(&blocked, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
            if (blocked.stream_limit < connection->local_max_streams[blocked.stream_type]) {
                error = utp_connection_queue_max_streams(connection, blocked.stream_type,
                                                         connection->local_max_streams[blocked.stream_type]);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    return error;
                }
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
            stream = utp_connection_find_stream_internal(connection, max_stream_data.stream_id);
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
            stream     = utp_connection_find_stream_internal(connection, blocked.stream_id);
            advertised = stream == NULL ? UTP_STREAM_DEFAULT_FLOW_WINDOW : stream->local_max_stream_data_advertised;
            error      = utp_connection_queue_max_stream_data(connection, blocked.stream_id, advertised, now_us);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        }
    }
    if (!candidate_path && !peer_close) {
        (void)utp_ack_scheduler_on_packet(&connection->ack_scheduler, view.header.packet_number, largest_before,
                                          utp_connection_packet_is_ack_eliciting(&view), handshake_done, now_us);
    }
    if (!candidate_path) {
        if ((uint64_t)wire_packet_length > UINT64_MAX - connection->rx_bytes) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        connection->rx_bytes += (uint64_t)wire_packet_length;
    }
    if (!candidate_path && view.header.type == UTP_PACKET_TYPE_HANDSHAKE &&
        connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
        (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT ||
         connection->state == UTP_CONNECTION_STATE_CONNECTED)) {
        if (connection->crypto_configured && !connection->crypto_ready) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        connection->peer_handshake_packet_number = view.header.packet_number;
        if (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT) {
            struct utp_packet_out_tailq retired_handshake_packets;

            TAILQ_INIT(&retired_handshake_packets);
            error = utp_send_control_retire_handshake_packets(&connection->send_control, now_us,
                                                              &retired_handshake_packets);
            if (error != UTP_INTERNAL_ERROR_OK) {
                utp_connection_release_queue(connection, &retired_handshake_packets);
                return error;
            }
            utp_connection_release_queue(connection, &retired_handshake_packets);
            connection->state = UTP_CONNECTION_STATE_CONNECTED;
            utp_send_control_set_connected(&connection->send_control, true);
        }
    } else if (!candidate_path && handshake_done && connection->role == UTP_CONNECTION_ROLE_PASSIVE &&
               connection->state != UTP_CONNECTION_STATE_CONNECTED) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (peer_close) {
        error = utp_connection_prepare_close_packet(connection, connection->peer_close_error_code, false);
        if (error != UTP_INTERNAL_ERROR_OK) {
            /* 对端已终止连接，回应包构造失败时也不能无限停留在 closing。 */
            utp_connection_enter_draining(connection, now_us);
        }
    } else {
        if (!candidate_path) {
            utp_connection_mark_peer_activity(connection, now_us);
        }
        if (ack_progress) {
            connection->retransmission_deadline_us = 0u;
            error                                  = utp_connection_ensure_retransmission_deadline(connection, now_us);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_connection_decrypt_packet(utp_connection_t* connection, uint8_t* packet,
                                                          size_t* packet_length)
{
    utp_packet_header_t  header;
    size_t               plaintext_length;
    utp_internal_error_t error;

    if (connection == NULL || packet == NULL || packet_length == NULL || *packet_length < UTP_PACKET_HEADER_SIZE) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_proto_decode_header(&header, packet, *packet_length);
    if (error != UTP_INTERNAL_ERROR_OK || *packet_length != UTP_PACKET_HEADER_SIZE + header.payload_length) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (!connection->crypto_ready) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (header.type == UTP_PACKET_TYPE_INITIAL) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    if (header.type == UTP_PACKET_TYPE_HANDSHAKE) {
        return connection->role == UTP_CONNECTION_ROLE_ACTIVE &&
                       (connection->state == UTP_CONNECTION_STATE_INITIAL_SENT ||
                        connection->state == UTP_CONNECTION_STATE_CONNECTED)
                   ? UTP_INTERNAL_ERROR_OK
                   : UTP_INTERNAL_ERROR_AUTH;
    }
    if (header.payload_length < UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_CRYPTO;
    }
    error = utp_crypto_aead_open(&connection->rx_aead, header.packet_number, packet + UTP_PACKET_HEADER_SIZE,
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

utp_internal_error_t utp_connection_on_packet_received(utp_connection_t* connection, uint8_t* packet,
                                                       size_t packet_length, const utp_address_t* peer, uint64_t now_us)
{
    const size_t         wire_packet_length = packet_length;
    utp_internal_error_t error              = utp_connection_decrypt_packet(connection, packet, &packet_length);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_connection_on_packet_received_internal(connection, packet, packet_length, wire_packet_length, NULL, peer,
                                                      now_us);
}

utp_internal_error_t utp_connection_on_packet_in_received(utp_connection_t* connection, utp_packet_in_t* packet,
                                                          const utp_address_t* peer, uint64_t now_us)
{
    size_t               wire_packet_length;
    size_t               packet_length;
    utp_internal_error_t error;

    if (packet == NULL || packet->data == NULL || !packet->in_use) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    wire_packet_length = packet->length;
    packet_length      = packet->length;
    error              = utp_connection_decrypt_packet(connection, packet->data, &packet_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    packet->length = (uint16_t)packet_length;
    return utp_connection_on_packet_received_internal(connection, packet->data, packet->length, wire_packet_length,
                                                      packet, peer, now_us);
}

utp_internal_error_t utp_connection_on_plaintext_packet_in_received(utp_connection_t* connection,
                                                                    utp_packet_in_t* packet, size_t wire_packet_length,
                                                                    const utp_address_t* peer, uint64_t now_us)
{
    if (packet == NULL || packet->data == NULL || !packet->in_use) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return utp_connection_on_packet_received_internal(connection, packet->data, packet->length, wire_packet_length,
                                                      packet, peer, now_us);
}

utp_internal_error_t utp_connection_queue_ack(utp_connection_t* connection, uint64_t now_us)
{
    bool                 queued;
    utp_internal_error_t error;

    if (connection == NULL || now_us == 0u || utp_ack_scheduler_pending_count(&connection->ack_scheduler) == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_connection_queue_control_packet(connection, now_us, true, true, false, &queued);
    if (error != UTP_INTERNAL_ERROR_OK || !queued) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_STATE : error;
    }
    return UTP_INTERNAL_ERROR_OK;
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
    utp_internal_error_t error;

    if (connection == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection->retransmission_deadline_us = 0u;
    error                                  = utp_send_control_on_retransmission_timeout(&connection->send_control);
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_connection_process_detected_losses(connection, now_us);
    }
    utp_connection_release_discarded_packets(connection, now_us);
    return error;
}

uint64_t utp_connection_retransmission_deadline(const utp_connection_t* connection)
{
    return connection == NULL ? 0u : connection->retransmission_deadline_us;
}

uint64_t utp_connection_close_deadline(const utp_connection_t* connection)
{
    return connection == NULL ? 0u : connection->close_deadline_us;
}

uint64_t utp_connection_keepalive_deadline(const utp_connection_t* connection)
{
    return connection == NULL || connection->state != UTP_CONNECTION_STATE_CONNECTED
               ? 0u
               : connection->keepalive_deadline_us;
}

uint64_t utp_connection_mtu_deadline(const utp_connection_t* connection, uint64_t now_us)
{
    const utp_mtu_discovery_t* discovery;
    uint64_t                   deadline_ms;

    if (connection == NULL || now_us == 0u || connection->state != UTP_CONNECTION_STATE_CONNECTED) {
        return 0u;
    }
    discovery = &connection->mtu_discovery;
    if (!utp_mtu_discovery_enabled(discovery)) {
        return 0u;
    }
    if (utp_mtu_discovery_has_in_flight_probe(discovery)) {
        deadline_ms = discovery->in_flight_probe_deadline_ms;
    } else if (utp_mtu_discovery_should_probe(discovery, now_us / UINT64_C(1000))) {
        return now_us;
    } else {
        deadline_ms = discovery->next_probe_time_ms;
    }
    return deadline_ms == 0u ? 0u : utp_connection_milliseconds_to_microseconds(deadline_ms);
}

uint64_t utp_connection_pacing_deadline(const utp_connection_t* connection)
{
    if (connection == NULL || connection->state == UTP_CONNECTION_STATE_CLOSING ||
        connection->state == UTP_CONNECTION_STATE_DRAINING || connection->state == UTP_CONNECTION_STATE_CLOSED) {
        return 0u;
    }
    return utp_send_control_pacing_deadline(&connection->send_control);
}

utp_internal_error_t utp_connection_on_mtu_timeout(utp_connection_t* connection, uint64_t now_us)
{
    utp_packet_out_t*    packet;
    uint64_t             packet_number;
    utp_internal_error_t error;

    if (connection == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (connection->state != UTP_CONNECTION_STATE_CONNECTED ||
        !utp_mtu_discovery_has_in_flight_probe(&connection->mtu_discovery) ||
        now_us / UINT64_C(1000) < connection->mtu_discovery.in_flight_probe_deadline_ms) {
        return UTP_INTERNAL_ERROR_OK;
    }
    packet_number = connection->mtu_discovery.in_flight_probe_packet_number;
    if (!utp_mtu_discovery_on_probe_timeout(&connection->mtu_discovery, now_us / UINT64_C(1000))) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    error = utp_send_control_take_mtu_probe(&connection->send_control, packet_number, &packet);
    if (error == UTP_INTERNAL_ERROR_OK && packet != NULL) {
        utp_packet_out_pool_release(&connection->packet_pool, packet);
    } else if (error != UTP_INTERNAL_ERROR_NOT_FOUND) {
        return error;
    }
    return utp_connection_ensure_retransmission_deadline(connection, now_us);
}

utp_internal_error_t utp_connection_on_keepalive_timeout(utp_connection_t* connection, uint64_t now_us)
{
    const uint8_t        ping = UTP_FRAME_TYPE_PING;
    utp_internal_error_t error;
    uint64_t             activity_deadline;

    if (connection == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (connection->state != UTP_CONNECTION_STATE_CONNECTED || connection->keepalive_deadline_us == 0u ||
        now_us < connection->keepalive_deadline_us) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (connection->last_peer_activity_us == 0u) {
        utp_connection_mark_peer_activity(connection, now_us);
        return UTP_INTERNAL_ERROR_OK;
    }
    activity_deadline =
        utp_connection_add_deadline(connection->last_peer_activity_us, UTP_CONNECTION_KEEPALIVE_INTERVAL_US);
    if (now_us < activity_deadline) {
        connection->keepalive_deadline_us = activity_deadline;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (connection->keepalive_missed_probes >= UTP_CONNECTION_KEEPALIVE_MAX_PROBES) {
        connection->state                      = UTP_CONNECTION_STATE_DRAINING;
        connection->keepalive_deadline_us      = 0u;
        connection->retransmission_deadline_us = 0u;
        connection->close_deadline_us          = now_us;
        utp_send_control_set_connected(&connection->send_control, false);
        return UTP_INTERNAL_ERROR_TIMEOUT;
    }
    error = utp_connection_queue_packet(connection, UTP_PACKET_TYPE_CTRL, &ping, sizeof(ping), true);
    if (error != UTP_INTERNAL_ERROR_OK) {
        connection->keepalive_deadline_us = utp_connection_add_deadline(now_us, UINT64_C(10000));
        return UTP_INTERNAL_ERROR_OK;
    }
    ++connection->keepalive_missed_probes;
    connection->keepalive_deadline_us = utp_connection_add_deadline(now_us, UTP_CONNECTION_KEEPALIVE_TIMEOUT_US);
    return UTP_INTERNAL_ERROR_OK;
}

uint64_t utp_connection_path_validation_deadline(const utp_connection_t* connection)
{
    return connection == NULL || connection->path_state != UTP_CONNECTION_PATH_STATE_VALIDATING
               ? 0u
               : connection->path_challenge_deadline_us;
}

utp_internal_error_t utp_connection_on_path_validation_timeout(utp_connection_t* connection, uint64_t now_us)
{
    if (connection == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (connection->path_state != UTP_CONNECTION_PATH_STATE_VALIDATING || !connection->path_challenge_pending ||
        connection->path_challenge_deadline_us == 0u || now_us < connection->path_challenge_deadline_us) {
        return UTP_INTERNAL_ERROR_OK;
    }
    connection->path_challenge_pending     = false;
    connection->path_challenge_deadline_us = 0u;
    if (connection->path_challenge_retry_count >= UTP_CONNECTION_PATH_CHALLENGE_MAX_RETRIES) {
        connection->candidate_peer         = connection->peer;
        connection->candidate_rx_bytes     = 0u;
        connection->candidate_tx_bytes     = 0u;
        connection->candidate_queued_bytes = 0u;
        connection->path_state             = UTP_CONNECTION_PATH_STATE_VALIDATED;
        return UTP_INTERNAL_ERROR_OK;
    }
    return utp_connection_send_path_challenge(connection, now_us);
}

utp_internal_error_t utp_connection_set_stream_scheduler_mode(utp_connection_t* connection, uint8_t mode)
{
    if (connection == NULL || mode > 1u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection->stream_scheduler_mode   = mode;
    connection->stream_scheduler_cursor = 0u;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_connection_create_stream_internal(utp_connection_t* connection, bool bidirectional,
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
    if (utp_connection_stream_ordinal(stream_id) >
        (uint32_t)connection->peer_max_streams[utp_connection_stream_type_from_id(stream_id)]) {
        (void)utp_connection_queue_streams_blocked(
            connection, utp_connection_stream_type_from_id(stream_id),
            connection->peer_max_streams[utp_connection_stream_type_from_id(stream_id)]);
        return UTP_INTERNAL_ERROR_STREAM_LIMIT;
    }
    {
        const utp_internal_error_t error = utp_connection_alloc_stream(connection, stream_id, &stream);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    connection->next_stream_id[slot] = stream_id + UTP_STREAM_TYPES;
    *out_stream_id                   = stream_id;
    return UTP_INTERNAL_ERROR_OK;
}

utp_stream_t* utp_connection_find_stream_internal(utp_connection_t* connection, uint32_t stream_id)
{
    if (connection == NULL) {
        return NULL;
    }
    return utp_connection_stream_from_node(utp_hash_table_find(&connection->streams, utp_connection_hash_u32(stream_id),
                                                               &stream_id, utp_connection_stream_matches, NULL));
}

utp_internal_error_t utp_stream_reset_internal(utp_stream_t* stream, uint16_t error_code)
{
    utp_connection_t*    connection;
    uint64_t             final_size;
    utp_internal_error_t error;

    if (stream == NULL || !stream->used || stream->connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection = stream->connection;
    if (!utp_connection_is_connected(connection)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (!utp_stream_local_can_send(stream)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    if (utp_stream_is_closed(stream)) {
        return UTP_INTERNAL_ERROR_CLOSED;
    }
    error = utp_stream_send_buffered_end_offset(stream, &final_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_connection_queue_reset_stream(connection, stream->stream_id, error_code, final_size);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_stream_on_reset(stream, error_code, false);
}

utp_connection_state_t utp_connection_state(const utp_connection_t* connection)
{
    return connection == NULL ? UTP_CONNECTION_STATE_CLOSED : connection->state;
}

bool utp_connection_is_connected(const utp_connection_t* connection)
{
    return connection != NULL && connection->state == UTP_CONNECTION_STATE_CONNECTED;
}
