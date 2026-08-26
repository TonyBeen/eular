#include "context/context.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <net/if.h>
#endif

#include "proto/ack.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "socket/address.h"
#include "util/allocator.h"
#include "util/error.h"
#include "util/time.h"

typedef struct utp_context_replay {
    utp_context_t*       context;     // 所属 Context，不拥有
    utp_connection_t*    connection;  // 被重放入站包的连接，不拥有
    const utp_address_t* peer;        // 入站包来源地址，不拥有
    uint64_t             now_us;      // 重放使用的接收时刻
} utp_context_replay_t;

typedef struct utp_context_peer_index_key {
    const utp_address_t* peer;      // 来源地址，不拥有
    uint32_t             peer_cid;  // 对端连接 ID
} utp_context_peer_index_key_t;

static void utp_context_report_connection_error(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                utp_status_t status, uint16_t peer_error_code, const uint8_t* reason,
                                                size_t reason_length, bool peer_initiated);
static void utp_context_report_connect_error(utp_context_t* context, utp_status_t status, const char* message,
                                             const utp_connect_attempt_info_t* attempt);
static utp_internal_error_t utp_context_flush_connection(utp_context_t* context, utp_context_connection_slot_t* slot);
static utp_internal_error_t utp_context_flush_connection_at(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                            uint64_t now_us);
static utp_internal_error_t utp_context_refresh_timer(utp_context_t* context, uint64_t now_us);
static void                 utp_context_on_udp_writable(uint32_t events, void* user_data);
static utp_internal_error_t utp_context_accept_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot);
static void utp_context_release_connection_slot(utp_context_t* context, utp_context_connection_slot_t* slot);
static void utp_context_release_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot);
static void utp_context_report_connected(utp_context_t* context, utp_context_connection_slot_t* slot);
static utp_internal_error_t utp_context_queue_session_token(utp_context_t*                 context,
                                                            utp_context_connection_slot_t* slot);
static utp_internal_error_t utp_context_complete_zero_rtt_response(utp_context_t*                 context,
                                                                   utp_context_connection_slot_t* slot,
                                                                   uint64_t                       now_us);
static utp_internal_error_t utp_context_process_nat_probe_timer(utp_context_t* context, uint64_t now_us);
static utp_internal_error_t utp_context_retry_nat_probe_send(utp_context_t* context, uint64_t now_us);
static utp_internal_error_t utp_context_on_nat_probe_packet(utp_context_t* context, const utp_packet_header_t* header,
                                                            const uint8_t* payload, size_t payload_length,
                                                            const utp_address_t* peer, const utp_address_t* local,
                                                            uint64_t now_us);
/** @brief 将 Context 固定配置应用至新建连接，所有建连路径必须调用。 */
static utp_internal_error_t utp_context_configure_connection(utp_context_t* context, utp_connection_t* connection)
{
    utp_internal_error_t error;

    if (context == NULL || connection == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection->context = context;
    error = utp_connection_set_congestion_algorithm(connection, context->cc_algorithm, &context->bbr_config,
                                                    &context->cubic_config, context->clock_granularity_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_connection_set_mtu_config(connection, &context->mtu_config);
    error = utp_connection_set_local_transport_config(
        connection, &context->local_transport_params, &context->local_ack_frequency, context->enable_keepalive,
        context->keepalive_interval_ms, context->keepalive_timeout_ms, context->keepalive_probes);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_set_stream_scheduler_mode(connection, (uint8_t)context->stream_scheduler_mode);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_set_stream_terminal_capacity(connection, context->stream_terminal_capacity);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_connection_set_path_validation_buffer_capacity(connection, context->path_validation_buffer_capacity);
    }
    return error;
}

static const uint8_t k_zero_rtt_version_frame[UTP_FRAME_VERSION_SIZE] = {
    UTP_FRAME_TYPE_VERSION, 0u, 0u, 0u, UTP_PROTOCOL_VERSION,
};
static const uint8_t k_zero_rtt_transport_params_frame[UTP_FRAME_TRANSPORT_PARAMS_SIZE] = {
    UTP_FRAME_TYPE_TRANSPORT_PARAMS,
};
static const uint8_t k_zero_rtt_ack_frequency_frame[UTP_FRAME_ACK_FREQUENCY_SIZE] = {
    UTP_FRAME_TYPE_ACK_FREQUENCY, 2u, 1u, 0u, 0u, 0u, 25u,
};

/** @brief 追加 0-RTT 固定协商参数；这些参数仅用于握手序校验，连接默认值仍由本地配置生效。 */
static utp_internal_error_t utp_context_append_zero_rtt_parameters(uint8_t* buffer, size_t capacity, size_t* offset,
                                                                   bool include_delay, uint32_t delay_us)
{
    if (buffer == NULL || offset == NULL || *offset > capacity ||
        capacity - *offset < sizeof(k_zero_rtt_version_frame) + sizeof(k_zero_rtt_transport_params_frame) +
                                 sizeof(k_zero_rtt_ack_frequency_frame) +
                                 (include_delay ? UTP_FRAME_HANDSHAKE_DELAY_SIZE : 0u)) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    memcpy(buffer + *offset, k_zero_rtt_version_frame, sizeof(k_zero_rtt_version_frame));
    *offset += sizeof(k_zero_rtt_version_frame);
    memcpy(buffer + *offset, k_zero_rtt_transport_params_frame, sizeof(k_zero_rtt_transport_params_frame));
    *offset += sizeof(k_zero_rtt_transport_params_frame);
    memcpy(buffer + *offset, k_zero_rtt_ack_frequency_frame, sizeof(k_zero_rtt_ack_frequency_frame));
    *offset += sizeof(k_zero_rtt_ack_frequency_frame);
    if (include_delay) {
        const utp_frame_handshake_delay_t delay = {delay_us};
        const utp_internal_error_t        error =
            utp_frame_handshake_delay_encode(buffer + *offset, capacity - *offset, &delay);

        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        *offset += UTP_FRAME_HANDSHAKE_DELAY_SIZE;
    }
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 追加确认请求包号的单区间 ACK，以及与该请求严格绑定的本地处理耗时。 */
static utp_internal_error_t utp_context_append_handshake_feedback(uint8_t* buffer, size_t capacity, size_t* offset,
                                                                  uint64_t request_packet_number,
                                                                  uint64_t request_received_us, uint64_t now_us)
{
    utp_ack_range_t             range;
    utp_ack_info_t              ack;
    utp_frame_handshake_delay_t delay;
    size_t                      ack_length;
    uint64_t                    delay_us;
    utp_internal_error_t        error;

    if (buffer == NULL || offset == NULL || *offset > capacity || request_packet_number == 0u ||
        request_received_us == 0u || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    delay_us           = now_us > request_received_us ? now_us - request_received_us : 0u;
    range.low          = request_packet_number;
    range.high         = request_packet_number;
    ack.largest_acked  = request_packet_number;
    ack.ack_delay      = delay_us;
    ack.ranges         = &range;
    ack.range_count    = 1u;
    ack.range_capacity = 1u;
    error              = utp_ack_encode(buffer + *offset, capacity - *offset, &ack, 0u, &ack_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    *offset             += ack_length;
    delay.delay_time_us  = delay_us > UINT32_MAX ? UINT32_MAX : (uint32_t)delay_us;
    error                = utp_frame_handshake_delay_encode(buffer + *offset, capacity - *offset, &delay);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *offset += UTP_FRAME_HANDSHAKE_DELAY_SIZE;
    }
    return error;
}

/** @brief 将 early 区补齐至握手最小包长，PADDING 始终位于加密边界之后。 */
static utp_internal_error_t utp_context_pad_zero_rtt_payload(utp_connection_t* connection, uint8_t* payload,
                                                             size_t capacity, size_t prefix_length, bool encrypted,
                                                             size_t* payload_length)
{
    uint16_t target_size;
    size_t   required_padding;

    if (connection == NULL || payload == NULL || payload_length == NULL || prefix_length > *payload_length) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    target_size           = utp_mtu_packet_size_from_mtu(connection->mtu_discovery.mtu_min, connection->peer.family);
    const size_t tag_size = encrypted ? UTP_CRYPTO_AEAD_TAG_SIZE : 0u;

    if ((size_t)target_size <= UTP_PACKET_HEADER_SIZE + tag_size + *payload_length) {
        return UTP_INTERNAL_ERROR_OK;
    }
    required_padding = (size_t)target_size - UTP_PACKET_HEADER_SIZE - tag_size - *payload_length;
    if (required_padding < UTP_FRAME_PADDING_HEADER_SIZE || required_padding > UINT16_MAX ||
        required_padding > capacity - *payload_length) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    if (utp_frame_padding_encode(payload + *payload_length, capacity - *payload_length,
                                 (uint16_t)(required_padding - UTP_FRAME_PADDING_HEADER_SIZE)) !=
        UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    *payload_length += required_padding;
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 返回 Unix 秒；票据时效必须使用墙上时间而非单调时钟。 */
static uint64_t utp_context_now_seconds(void)
{
    const time_t now = time(NULL);

    return now > 0 ? (uint64_t)now : UINT64_C(1);
}

/** @brief 从哈希节点取得动态 Connection 槽位。 */
static utp_context_connection_slot_t* utp_context_connection_slot_from_node(utp_hash_node_t* node)
{
    return node == NULL
               ? NULL
               : (utp_context_connection_slot_t*)((uint8_t*)node - offsetof(utp_context_connection_slot_t, node));
}

/** @brief 从来源地址和对端 CID 哈希节点取得被动连接槽位。 */
static utp_context_connection_slot_t* utp_context_connection_slot_from_peer_node(utp_hash_node_t* node)
{
    return node == NULL
               ? NULL
               : (utp_context_connection_slot_t*)((uint8_t*)node - offsetof(utp_context_connection_slot_t, peer_node));
}

/** @brief 按本地 CID 比较 Connection 槽位。 */
static bool utp_context_connection_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_connection_slot_t* slot;

    (void)user_data;
    slot = (const utp_context_connection_slot_t*)((const uint8_t*)node - offsetof(utp_context_connection_slot_t, node));
    return key != NULL && slot->connection.local_cid == *(const uint32_t*)key;
}

/** @brief 按来源地址和对端 CID 比较被动连接槽位。 */
static bool utp_context_connection_peer_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_connection_slot_t* slot;
    const utp_context_peer_index_key_t*  peer_key = key;

    (void)user_data;
    slot = (const utp_context_connection_slot_t*)((const uint8_t*)node -
                                                  offsetof(utp_context_connection_slot_t, peer_node));
    return peer_key != NULL && peer_key->peer != NULL && slot->connection.peer_cid == peer_key->peer_cid &&
           utp_address_equal(&slot->connection.peer, peer_key->peer);
}

/** @brief 从哈希节点取得动态 pending 槽位。 */
static utp_context_pending_slot_t* utp_context_pending_slot_from_node(utp_hash_node_t* node)
{
    return node == NULL ? NULL
                        : (utp_context_pending_slot_t*)((uint8_t*)node - offsetof(utp_context_pending_slot_t, node));
}

/** @brief 从来源地址和对端 CID 哈希节点取得 pending 槽位。 */
static utp_context_pending_slot_t* utp_context_pending_slot_from_peer_node(utp_hash_node_t* node)
{
    return node == NULL
               ? NULL
               : (utp_context_pending_slot_t*)((uint8_t*)node - offsetof(utp_context_pending_slot_t, peer_node));
}

/** @brief 按本地 CID 比较 pending 槽位。 */
static bool utp_context_pending_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_pending_slot_t* slot;

    (void)user_data;
    slot = (const utp_context_pending_slot_t*)((const uint8_t*)node - offsetof(utp_context_pending_slot_t, node));
    return key != NULL && slot->pending.local_cid == *(const uint32_t*)key;
}

/** @brief 按来源地址和对端 CID 比较 pending 槽位。 */
static bool utp_context_pending_peer_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_pending_slot_t*   slot;
    const utp_context_peer_index_key_t* peer_key = key;

    (void)user_data;
    slot = (const utp_context_pending_slot_t*)((const uint8_t*)node - offsetof(utp_context_pending_slot_t, peer_node));
    return peer_key != NULL && peer_key->peer != NULL && slot->pending.peer_cid == peer_key->peer_cid &&
           utp_address_equal(&slot->pending.peer, peer_key->peer);
}

/** @brief 为来源地址和对端 CID 生成稳定的 64 位哈希。 */
static uint64_t utp_context_peer_index_hash(const utp_address_t* peer, uint32_t peer_cid)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t   address_length;
    size_t   index;

    address_length  = peer->family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;
    hash           ^= peer->family;
    hash           *= UINT64_C(1099511628211);
    hash           ^= peer->port;
    hash           *= UINT64_C(1099511628211);
    hash           ^= peer->scope_id;
    hash           *= UINT64_C(1099511628211);
    for (index = 0u; index < address_length; ++index) {
        hash ^= peer->address[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= peer_cid;
    hash *= UINT64_C(1099511628211);
    return hash;
}

/** @brief 使所有已缓存的恢复凭证失效，根密钥替换后不得继续导出或使用旧状态。 */
static void utp_context_invalidate_resumption_state(utp_context_t* context)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL) {
        return;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        utp_context_connection_slot_t* slot       = utp_context_connection_slot_from_node(node);
        utp_connection_t*              connection = &slot->connection;

        utp_crypto_secure_clear(connection->session_token, sizeof(connection->session_token));
        connection->session_token_size               = 0u;
        connection->session_token_expires_at_seconds = 0u;
        connection->session_token_issued             = false;
        if (slot->connect_pending && (slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN ||
                                      slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE)) {
            const utp_connect_attempt_info_t attempt = slot->connect_attempt;

            utp_context_report_connect_error(context, UTP_STATUS_CANCELLED, "resumption key replaced", &attempt);
            utp_context_release_connection_slot(context, slot);
        }
    }
}

/** @brief 从哈希节点取得动态 replay 记录。 */
static utp_context_zero_rtt_replay_entry_t* utp_context_replay_entry_from_node(utp_hash_node_t* node)
{
    return node == NULL ? NULL
                        : (utp_context_zero_rtt_replay_entry_t*)((uint8_t*)node -
                                                                 offsetof(utp_context_zero_rtt_replay_entry_t, node));
}

/** @brief 比较 replay 记录的完整 40 字节键，哈希值仅用于分桶。 */
static bool utp_context_replay_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_zero_rtt_replay_entry_t* entry;

    (void)user_data;
    entry = (const utp_context_zero_rtt_replay_entry_t*)((const uint8_t*)node -
                                                         offsetof(utp_context_zero_rtt_replay_entry_t, node));
    return key != NULL && memcmp(entry->key, key, sizeof(entry->key)) == 0;
}

/** @brief 为 replay 哈希表生成稳定的 64 位分桶哈希。 */
static uint64_t utp_context_replay_hash(const uint8_t key[UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE])
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t   index;

    for (index = 0u; index < UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE; ++index) {
        hash ^= key[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/** @brief 删除 replay 记录时释放其动态内存。 */
static void utp_context_free_replay_entry(utp_hash_node_t* node, void* user_data)
{
    (void)user_data;
    utp_allocator_free(NULL, utp_context_replay_entry_from_node(node));
}

/** @brief 清空 replay cache，Context root 替换和销毁均调用。 */
static void utp_context_clear_zero_rtt_replay(utp_context_t* context)
{
    if (context != NULL) {
        utp_hash_table_clear(&context->zero_rtt_replay, utp_context_free_replay_entry, NULL);
    }
}

/** @brief 仅在容量耗尽时惰性回收已过期 replay 记录。 */
static void utp_context_purge_expired_zero_rtt_replay(utp_context_t* context, uint64_t now_seconds)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->zero_rtt_replay, &iter)) != NULL) {
        utp_context_zero_rtt_replay_entry_t* entry = utp_context_replay_entry_from_node(node);

        if (entry->expires_at_seconds <= now_seconds) {
            (void)utp_hash_table_remove(&context->zero_rtt_replay, node);
            utp_allocator_free(NULL, entry);
        }
    }
}

/** @brief 记录一个已认证的 0-RTT 包；缓存满时绝不淘汰未过期记录。 */
static bool utp_context_remember_zero_rtt_replay(
    utp_context_t* context, const uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE],
    const uint8_t early_attempt_nonce[UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE], uint64_t now_seconds,
    uint64_t expires_at_seconds)
{
    uint8_t              digest[UTP_CRYPTO_SHA256_SIZE];
    uint8_t              key[UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE];
    utp_internal_error_t error;

    if (context == NULL || encrypted_server_info == NULL || early_attempt_nonce == NULL ||
        expires_at_seconds <= now_seconds) {
        return false;
    }
    error = utp_crypto_sha256(encrypted_server_info, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE, digest);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    memcpy(key, digest, 16u);
    memcpy(key + 16u, early_attempt_nonce, UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE);
    const uint64_t hash = utp_context_replay_hash(key);
    if (utp_hash_table_find(&context->zero_rtt_replay, hash, key, utp_context_replay_matches, NULL) != NULL) {
        return false;
    }
    if (utp_hash_table_count(&context->zero_rtt_replay) >= context->zero_rtt_replay_cache_capacity) {
        utp_context_purge_expired_zero_rtt_replay(context, now_seconds);
    }
    if (utp_hash_table_count(&context->zero_rtt_replay) >= context->zero_rtt_replay_cache_capacity) {
        return false;
    }
    utp_context_zero_rtt_replay_entry_t* entry = utp_allocator_alloc(NULL, sizeof(*entry));
    if (entry == NULL) {
        return false;
    }
    utp_hash_node_init(&entry->node);
    entry->expires_at_seconds = expires_at_seconds;
    memcpy(entry->key, key, sizeof(entry->key));
    error = utp_hash_table_insert(&context->zero_rtt_replay, &entry->node, hash, key, utp_context_replay_matches, NULL);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_allocator_free(NULL, entry);
        return false;
    }
    return true;
}

static void utp_context_log(utp_context_t* context, utp_log_level_t level, const char* message)
{
    if (context == NULL || message == NULL || !utp_internal_log_enabled(&context->logger, level)) {
        return;
    }
    utp_internal_log(&context->logger, &context->tag, level, message);
}

static void utp_context_log_ids(utp_context_t* context, utp_log_level_t level, const char* event, uint32_t local_cid,
                                uint32_t peer_cid)
{
    if (context == NULL || event == NULL || !utp_internal_log_enabled(&context->logger, level)) {
        return;
    }
    char message[160];
    (void)snprintf(message, sizeof(message), "%s: local_cid=%" PRIu32 ", peer_cid=%" PRIu32, event, local_cid,
                   peer_cid);
    utp_context_log(context, level, message);
}

static void utp_context_log_close(utp_context_t* context, const utp_context_connection_slot_t* slot,
                                  utp_status_t status, uint16_t peer_error_code, bool peer_initiated)
{
    const utp_log_level_t level = status == UTP_STATUS_OK ? UTP_LOG_LEVEL_INFO : UTP_LOG_LEVEL_WARNING;
    if (context == NULL || slot == NULL || !utp_internal_log_enabled(&context->logger, level)) {
        return;
    }
    char message[256];
    (void)snprintf(message, sizeof(message),
                   "connection closed: local_cid=%" PRIu32 ", status=%s, peer_initiated=%u, peer_error=%" PRIu16,
                   slot->connection.local_cid, utp_status_string(status), peer_initiated ? 1u : 0u, peer_error_code);
    utp_context_log(context, level, message);
}

/** @brief 返回当前 Binding 请求要求的响应来源，用于诊断日志。 */
static const char* utp_context_nat_step_name(uint8_t step)
{
    switch (step) {
    case UTP_NAT_PROBE_STEP_PRIMARY_BINDING:
        return "[PrimaryBinding|ChangePort|ChangeIP]";
    case UTP_NAT_PROBE_STEP_ALTERNATE_BINDING:
        return "[AlternateBinding|ChangePort]";
    default:
        return "[Unknown]";
    }
}

static const char* utp_context_nat_response_source_name(uint8_t change_flags)
{
    if (change_flags == UTP_NAT_PROBE_CHANGE_PORT) {
        return "ChangePort";
    }
    if (change_flags == UTP_NAT_PROBE_CHANGE_BOTH) {
        return "ChangePort|ChangeIP";
    }
    return "Probe";
}

/** @brief 输出单个 NAT endpoint；仅在 debug 日志启用时执行格式化。 */
static void utp_context_log_nat_endpoint(utp_context_t* context, const char* event, uint8_t step,
                                         const utp_address_t* endpoint, uint64_t packet_number)
{
    char address[UTP_ADDRESS_TEXT_MAX_LENGTH];
    char message[256];

    if (!utp_internal_log_enabled(&context->logger, UTP_LOG_LEVEL_DEBUG)) {
        return;
    }
    if (utp_address_format(endpoint, address, sizeof(address)) != UTP_INTERNAL_ERROR_OK) {
        return;
    }
    (void)snprintf(message, sizeof(message), "NAT %s [%s] -> %s:%" PRIu16 " pn=%" PRIu64,
                   utp_context_nat_step_name(step), event, address, endpoint->port, packet_number);
    utp_context_log(context, UTP_LOG_LEVEL_DEBUG, message);
}

/** @brief 格式化 NAT 响应的本地投递地址与入口接口标识。 */
static void utp_context_format_nat_local(const utp_address_t* local, char address[UTP_ADDRESS_TEXT_MAX_LENGTH],
                                         char ifname[64u])
{
    memcpy(address, "<unknown>", sizeof("<unknown>"));
    memcpy(ifname, "<unknown>", sizeof("<unknown>"));
    if (local == NULL) {
        return;
    }
    (void)utp_address_format(local, address, UTP_ADDRESS_TEXT_MAX_LENGTH);
#if !defined(_WIN32)
    if (local->scope_id != 0u) {
        (void)if_indextoname(local->scope_id, ifname);
    }
#endif
}

/** @brief 输出已通过关联校验的 NAT 响应详情。 */
static void utp_context_log_nat_response(utp_context_t* context, uint8_t step, const utp_address_t* peer,
                                         const utp_nat_probe_response_t* response, uint64_t packet_number,
                                         const utp_address_t* local, uint64_t rtt_us)
{
    char peer_address[UTP_ADDRESS_TEXT_MAX_LENGTH];
    char mapped_address[UTP_ADDRESS_TEXT_MAX_LENGTH];
    char local_address[UTP_ADDRESS_TEXT_MAX_LENGTH];
    char local_ifname[64u];
    char message[448];

    if (!utp_internal_log_enabled(&context->logger, UTP_LOG_LEVEL_DEBUG) ||
        utp_address_format(peer, peer_address, sizeof(peer_address)) != UTP_INTERNAL_ERROR_OK ||
        utp_address_format(&response->mapped, mapped_address, sizeof(mapped_address)) != UTP_INTERNAL_ERROR_OK) {
        return;
    }
    utp_context_format_nat_local(local, local_address, local_ifname);
    (void)snprintf(message, sizeof(message),
                   "NAT %s <- %s:%" PRIu16 " [BindingResponse|%s] mapped=%s:%" PRIu16 " recv=%s:%" PRIu16
                   " if=%s ifindex=%" PRIu32 " pn=%" PRIu64 " rtt_us=%" PRIu64 " alternate=%u",
                   utp_context_nat_step_name(step), peer_address, peer->port,
                   utp_context_nat_response_source_name(response->change_flags), mapped_address, response->mapped.port,
                   local_address, local == NULL ? 0u : local->port, local_ifname, local == NULL ? 0u : local->scope_id,
                   packet_number, rtt_us, response->has_alternate ? 1u : 0u);
    utp_context_log(context, UTP_LOG_LEVEL_DEBUG, message);
}

/** @brief 输出已到达但未通过 NAT 探测关联校验的响应原因。 */
static void utp_context_log_nat_response_rejected(utp_context_t* context, uint8_t step, const utp_address_t* peer,
                                                  const utp_address_t* local, uint64_t packet_number,
                                                  const char* reason)
{
    char peer_address[UTP_ADDRESS_TEXT_MAX_LENGTH];
    char local_address[UTP_ADDRESS_TEXT_MAX_LENGTH];
    char local_ifname[64u];
    char message[400];

    if (!utp_internal_log_enabled(&context->logger, UTP_LOG_LEVEL_DEBUG) ||
        utp_address_format(peer, peer_address, sizeof(peer_address)) != UTP_INTERNAL_ERROR_OK) {
        return;
    }
    utp_context_format_nat_local(local, local_address, local_ifname);
    (void)snprintf(message, sizeof(message),
                   "NAT %s <- %s:%" PRIu16 " ignored recv=%s:%" PRIu16 " if=%s ifindex=%" PRIu32 " pn=%" PRIu64
                   " reason=%s",
                   utp_context_nat_step_name(step), peer_address, peer->port, local_address,
                   local == NULL ? 0u : local->port, local_ifname, local == NULL ? 0u : local->scope_id, packet_number,
                   reason);
    utp_context_log(context, UTP_LOG_LEVEL_DEBUG, message);
}

static uint64_t utp_context_now_us(void)
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

static bool utp_context_crypto_type_from_encryption(utp_encryption_mode_t encryption, uint8_t* crypto_type)
{
    if (crypto_type == NULL) {
        return false;
    }
    if (encryption == UTP_ENCRYPTION_AES_GCM_128) {
        *crypto_type = UTP_FRAME_CRYPTO_TYPE_AES_GCM_128;
        return true;
    }
    if (encryption == UTP_ENCRYPTION_AES_GCM_256) {
        *crypto_type = UTP_FRAME_CRYPTO_TYPE_AES_GCM_256;
        return true;
    }
    return false;
}

static utp_encryption_mode_t utp_context_encryption_from_crypto_type(uint8_t crypto_type)
{
    return crypto_type == UTP_FRAME_CRYPTO_TYPE_AES_GCM_256 ? UTP_ENCRYPTION_AES_GCM_256 : UTP_ENCRYPTION_AES_GCM_128;
}

/** @brief 为已建立的被动连接签发统一的加密恢复凭证。 */
static utp_internal_error_t utp_context_queue_session_token(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (context == NULL || slot == NULL || !slot->used || slot->connection.role != UTP_CONNECTION_ROLE_PASSIVE ||
        slot->connection.session_token_issued || context->zero_rtt_token_max_lifetime_seconds == 0u ||
        !context->resumption_keys_ready) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (slot->connection.crypto_configured && !slot->connection.crypto_ready) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (!context->resumption_key_explicit && !context->default_resumption_key_warning_logged) {
        utp_context_log(context, UTP_LOG_LEVEL_WARNING,
                        "utp: using built-in default resumption key; configure a custom key for production");
        context->default_resumption_key_warning_logged = true;
    }
    const uint64_t now_seconds        = utp_context_now_seconds();
    const uint64_t expires_at_seconds = context->zero_rtt_token_max_lifetime_seconds > UINT64_MAX - now_seconds
                                            ? UINT64_MAX
                                            : now_seconds + context->zero_rtt_token_max_lifetime_seconds;
    const uint8_t encryption_mode = slot->connection.crypto_configured
                                        ? (uint8_t)utp_context_encryption_from_crypto_type(slot->connection.crypto_type)
                                        : (uint8_t)UTP_ENCRYPTION_NONE;
    uint8_t       token_payload[UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE];
    uint8_t       payload[UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE];
    utp_frame_session_token_t frame;
    utp_internal_error_t      error = utp_crypto_random_bytes(token_payload, UTP_CRYPTO_RESUMPTION_PSK_SIZE);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_server_info_seal(context->resumption_keys.ticket_seal_key, token_payload, encryption_mode,
                                            expires_at_seconds, token_payload + UTP_CRYPTO_RESUMPTION_PSK_SIZE);
    }
    frame.payload            = token_payload;
    frame.payload_length     = UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE;
    frame.expires_at_seconds = expires_at_seconds;
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_frame_session_token_encode(payload, sizeof(payload), &frame);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_CTRL, payload,
                                            UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + frame.payload_length, true);
    }
    utp_crypto_secure_clear(token_payload, sizeof(token_payload));
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connection.session_token_issued = true;
    }
    return error;
}

static bool utp_context_log_level_is_valid(utp_log_level_t level)
{
    return level == UTP_LOG_LEVEL_DEBUG || level == UTP_LOG_LEVEL_INFO || level == UTP_LOG_LEVEL_WARNING ||
           level == UTP_LOG_LEVEL_ERROR || level == UTP_LOG_LEVEL_SILENCE;
}

static bool utp_context_cid_in_use(const utp_context_t* context, uint32_t cid)
{
    if (cid == 0u) {
        return true;
    }
    return utp_hash_table_find(&context->connections, cid, &cid, utp_context_connection_slot_matches, NULL) != NULL ||
           utp_hash_table_find(&context->pending_incoming, cid, &cid, utp_context_pending_slot_matches, NULL) != NULL;
}

static utp_internal_error_t utp_context_alloc_cid(utp_context_t* context, uint32_t* out_cid)
{
    uint32_t candidate;
    uint32_t attempts;

    if (context == NULL || out_cid == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    // CID 为 0 时仅用于尚未完成解复用的 Initial 包；已分配连接必须跳过该值。
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
    utp_hash_node_t* node;

    if (context == NULL || local_cid == 0u) {
        return NULL;
    }
    node = utp_hash_table_find(&context->connections, local_cid, &local_cid, utp_context_connection_slot_matches, NULL);
    return utp_context_connection_slot_from_node(node);
}

static utp_context_connection_slot_t* utp_context_find_connection_by_peer(utp_context_t*       context,
                                                                          const utp_address_t* peer)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL || peer == NULL) {
        return NULL;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);

        if (utp_address_equal(&slot->connection.peer, peer)) {
            return slot;
        }
    }
    return NULL;
}

/** @brief 按来源地址和对端 CID 查找已晋升的被动连接。 */
static utp_context_connection_slot_t* utp_context_find_passive_connection_by_peer(utp_context_t*       context,
                                                                                  const utp_address_t* peer,
                                                                                  uint32_t             peer_cid)
{
    const utp_context_peer_index_key_t key = {peer, peer_cid};
    utp_hash_node_t*                   node;

    if (context == NULL || peer == NULL || peer_cid == 0u) {
        return NULL;
    }
    node = utp_hash_table_find(&context->passive_connections_by_peer, utp_context_peer_index_hash(peer, peer_cid), &key,
                               utp_context_connection_peer_slot_matches, NULL);
    return utp_context_connection_slot_from_peer_node(node);
}

/** @brief 查找可响应重复加密 0-RTT 的短期握手缓存，匹配必须同时绑定来源和完整 token payload。 */
static utp_context_connection_slot_t* utp_context_find_zero_rtt_response(
    utp_context_t* context, const utp_address_t* peer,
    const uint8_t token_payload[UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE])
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL || peer == NULL || token_payload == NULL) {
        return NULL;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);

        if (slot->connection.role == UTP_CONNECTION_ROLE_PASSIVE && slot->zero_rtt_response_active &&
            utp_address_equal(&slot->connection.peer, peer) &&
            memcmp(slot->zero_rtt_session_token, token_payload, UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE) == 0) {
            return slot;
        }
    }
    return NULL;
}

static utp_context_connection_slot_t* utp_context_alloc_connection_slot(utp_context_t* context)
{
    utp_context_connection_slot_t* slot;

    if (context == NULL) {
        return NULL;
    }
    slot = TAILQ_FIRST(&context->free_connection_slots);
    if (slot != NULL) {
        TAILQ_REMOVE(&context->free_connection_slots, slot, free_next);
    } else {
        slot = utp_allocator_alloc(NULL, sizeof(*slot));
        if (slot == NULL) {
            return NULL;
        }
    }
    utp_hash_node_init(&slot->node);
    utp_hash_node_init(&slot->peer_node);
    slot->connection.local_cid            = 0u;
    slot->connection.peer_cid             = 0u;
    slot->connect_deadline_us             = 0u;
    slot->zero_rtt_early_packet           = NULL;
    slot->zero_rtt_early_wire_size        = 0u;
    slot->zero_rtt_request_packet_number  = 0u;
    slot->zero_rtt_request_received_us    = 0u;
    slot->zero_rtt_response_deadline_us   = 0u;
    slot->zero_rtt_expire_deadline_us     = 0u;
    slot->zero_rtt_amplification_rx_bytes = 0u;
    slot->zero_rtt_amplification_tx_bytes = 0u;
    slot->connect_retries_remaining       = 0;
    slot->zero_rtt_response_retries       = 0u;
    slot->zero_rtt_early_data             = NULL;
    slot->zero_rtt_early_data_size        = 0u;
    slot->zero_rtt_expires_at_seconds     = 0u;
    slot->terminal_error_status           = UTP_STATUS_OK;
    slot->terminal_error_reason           = NULL;
    slot->terminal_error_reason_length    = 0u;
    slot->zero_rtt_early_fin              = false;
    slot->zero_rtt_awaiting_accept        = false;
    slot->zero_rtt_accepted               = false;
    slot->zero_rtt_response_active        = false;
    slot->zero_rtt_response_queued        = false;
    slot->zero_rtt_response_sent          = false;
    slot->zero_rtt_early_delivered        = false;
    slot->zero_rtt_encryption_mode        = UTP_CRYPTO_ENCRYPTION_MODE_NONE;
    slot->used                            = true;
    slot->connected_reported              = false;
    slot->connection_error_reported       = false;
    slot->connect_pending                 = false;
    slot->terminal_error_queued           = false;
    slot->terminal_error_suppressed       = false;
    return slot;
}

/** @brief 将完成初始化的 Connection 槽位注册到 CID 哈希表。 */
static utp_internal_error_t utp_context_register_connection_slot(utp_context_t*                 context,
                                                                 utp_context_connection_slot_t* slot)
{
    const uint32_t       local_cid = slot == NULL ? 0u : slot->connection.local_cid;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || !slot->used || local_cid == 0u || slot->node.table != NULL ||
        (slot->connection.role == UTP_CONNECTION_ROLE_PASSIVE && slot->peer_node.table != NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_hash_table_insert(&context->connections, &slot->node, local_cid, &local_cid,
                                  utp_context_connection_slot_matches, NULL);
    if (error != UTP_INTERNAL_ERROR_OK || slot->connection.role != UTP_CONNECTION_ROLE_PASSIVE) {
        return error;
    }
    {
        const utp_context_peer_index_key_t key = {&slot->connection.peer, slot->connection.peer_cid};

        error = utp_hash_table_insert(&context->passive_connections_by_peer, &slot->peer_node,
                                      utp_context_peer_index_hash(key.peer, key.peer_cid), &key,
                                      utp_context_connection_peer_slot_matches, NULL);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        (void)utp_hash_table_remove(&context->connections, &slot->node);
    }
    return error;
}

/** @brief 从 CID 哈希表摘除 Connection，但保留槽位供重试重新初始化。 */
static void utp_context_unregister_connection_slot(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (context != NULL && slot != NULL && slot->peer_node.table == &context->passive_connections_by_peer) {
        (void)utp_hash_table_remove(&context->passive_connections_by_peer, &slot->peer_node);
    }
    if (context != NULL && slot != NULL && slot->node.table == &context->connections) {
        (void)utp_hash_table_remove(&context->connections, &slot->node);
    }
}

static void utp_context_release_connection_slot(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (context != NULL && slot != NULL && slot->used) {
        if (slot->terminal_error_queued) {
            TAILQ_REMOVE(&context->terminal_error_slots, slot, terminal_error_next);
            slot->terminal_error_queued = false;
        }
        utp_context_unregister_connection_slot(context, slot);
        if (slot->connection.local_cid != 0u) {
            utp_connection_cleanup(&slot->connection);
        }
        slot->connected_reported        = false;
        slot->connection_error_reported = false;
        slot->connect_deadline_us       = 0u;
        slot->connect_retries_remaining = 0;
        slot->connect_pending           = false;
        if (slot->zero_rtt_early_packet != NULL) {
            utp_packet_in_release(slot->zero_rtt_early_packet);
            slot->zero_rtt_early_packet = NULL;
        }
        utp_allocator_free(NULL, slot->zero_rtt_early_data);
        slot->zero_rtt_early_data = NULL;
        utp_crypto_secure_clear(slot->zero_rtt_resumption_psk, sizeof(slot->zero_rtt_resumption_psk));
        slot->zero_rtt_early_wire_size        = 0u;
        slot->zero_rtt_request_packet_number  = 0u;
        slot->zero_rtt_request_received_us    = 0u;
        slot->zero_rtt_response_deadline_us   = 0u;
        slot->zero_rtt_expire_deadline_us     = 0u;
        slot->zero_rtt_amplification_rx_bytes = 0u;
        slot->zero_rtt_amplification_tx_bytes = 0u;
        slot->zero_rtt_response_retries       = 0u;
        slot->zero_rtt_early_data_size        = 0u;
        slot->zero_rtt_expires_at_seconds     = 0u;
        slot->terminal_error_status           = UTP_STATUS_OK;
        slot->terminal_error_reason           = NULL;
        slot->terminal_error_reason_length    = 0u;
        slot->zero_rtt_early_fin              = false;
        slot->zero_rtt_awaiting_accept        = false;
        slot->zero_rtt_accepted               = false;
        slot->zero_rtt_response_active        = false;
        slot->zero_rtt_response_queued        = false;
        slot->zero_rtt_response_sent          = false;
        slot->zero_rtt_early_delivered        = false;
        slot->zero_rtt_encryption_mode        = UTP_CRYPTO_ENCRYPTION_MODE_NONE;
        slot->terminal_error_suppressed       = false;
        slot->used                            = false;
        TAILQ_INSERT_TAIL(&context->free_connection_slots, slot, free_next);
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

    utp_context_log_ids(context, UTP_LOG_LEVEL_WARNING, "peer protocol error", slot->connection.local_cid,
                        slot->connection.peer_cid);
    // 回调期间连接必须已不可用，防止用户继续排入业务数据。
    utp_connection_close_on_protocol_error(&slot->connection, close_code, utp_context_now_us());
    utp_context_report_connection_error(context, slot, status, 0u, (const uint8_t*)reason, strlen(reason), false);
    if (utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_DRAINING) {
        return UTP_INTERNAL_ERROR_OK;
    }
    close_error = utp_context_flush_connection(context, slot);
    return close_error;
}

static utp_context_pending_slot_t* utp_context_find_pending_slot(utp_context_t* context, uint32_t local_cid)
{
    utp_hash_node_t* node;

    if (context == NULL || local_cid == 0u) {
        return NULL;
    }
    node =
        utp_hash_table_find(&context->pending_incoming, local_cid, &local_cid, utp_context_pending_slot_matches, NULL);
    return utp_context_pending_slot_from_node(node);
}

static utp_context_pending_slot_t* utp_context_find_pending_by_peer(utp_context_t* context, uint32_t peer_cid,
                                                                    const utp_address_t* peer)
{
    const utp_context_peer_index_key_t key = {peer, peer_cid};
    utp_hash_node_t*                   node;

    if (context == NULL || peer == NULL || peer_cid == 0u) {
        return NULL;
    }
    node = utp_hash_table_find(&context->pending_incoming_by_peer, utp_context_peer_index_hash(peer, peer_cid), &key,
                               utp_context_pending_peer_slot_matches, NULL);
    return utp_context_pending_slot_from_peer_node(node);
}

static utp_context_pending_slot_t* utp_context_alloc_pending_slot(utp_context_t* context)
{
    utp_context_pending_slot_t* slot;

    if (context == NULL || utp_hash_table_count(&context->pending_incoming) >= context->pending_incoming.max_entries) {
        return NULL;
    }
    slot = TAILQ_FIRST(&context->free_pending_slots);
    if (slot != NULL) {
        TAILQ_REMOVE(&context->free_pending_slots, slot, free_next);
    } else {
        slot = utp_allocator_alloc(NULL, sizeof(*slot));
        if (slot == NULL) {
            return NULL;
        }
    }
    utp_hash_node_init(&slot->node);
    utp_hash_node_init(&slot->peer_node);
    slot->pending.local_cid = 0u;
    slot->used              = true;
    slot->queued            = false;
    return slot;
}

/** @brief 将 pending 注册到 CID 哈希表并计入配置的容量。 */
static utp_internal_error_t utp_context_register_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot)
{
    const uint32_t       local_cid = slot == NULL ? 0u : slot->pending.local_cid;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || !slot->used || local_cid == 0u || slot->node.table != NULL ||
        slot->peer_node.table != NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_hash_table_insert(&context->pending_incoming, &slot->node, local_cid, &local_cid,
                                  utp_context_pending_slot_matches, NULL);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    {
        const utp_context_peer_index_key_t key = {&slot->pending.peer, slot->pending.peer_cid};

        error = utp_hash_table_insert(&context->pending_incoming_by_peer, &slot->peer_node,
                                      utp_context_peer_index_hash(key.peer, key.peer_cid), &key,
                                      utp_context_pending_peer_slot_matches, NULL);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        (void)utp_hash_table_remove(&context->pending_incoming, &slot->node);
    }
    return error;
}

static void utp_context_release_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot)
{
    if (context != NULL && slot != NULL && slot->used) {
        if (slot->peer_node.table == &context->pending_incoming_by_peer) {
            (void)utp_hash_table_remove(&context->pending_incoming_by_peer, &slot->peer_node);
        }
        if (slot->node.table == &context->pending_incoming) {
            (void)utp_hash_table_remove(&context->pending_incoming, &slot->node);
        }
        utp_pending_incoming_reset(&slot->pending);
        slot->queued = false;
        slot->used   = false;
        TAILQ_INSERT_TAIL(&context->free_pending_slots, slot, free_next);
    }
}

static utp_internal_error_t utp_context_send_raw(utp_context_t* context, const utp_address_t* peer,
                                                 const utp_address_t* local, const uint8_t* packet,
                                                 size_t packet_length)
{
    size_t sent_length = 0u;

    return utp_udp_socket_send_from_to(&context->udp_socket, packet, packet_length, peer, local, &sent_length);
}

static utp_internal_error_t utp_context_resolve_packet_slice(const utp_packet_out_t*       packet,
                                                             const utp_packet_out_slice_t* slice,
                                                             utp_udp_send_slice_t*         out_slice)
{
    if (slice->length == 0u) {
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

/** @brief 将未加密 PacketOut 映射为可供 sendmmsg 使用的零拷贝消息。 */
static utp_internal_error_t utp_context_prepare_packet_send_message(const utp_connection_t* connection,
                                                                    const utp_packet_out_t* packet,
                                                                    utp_udp_send_slice_t*   slices,
                                                                    utp_udp_send_message_t* message)
{
    size_t total_length = 0u;

    message->peer = packet->has_destination ? &packet->destination : &connection->peer;
    message->local =
        (packet->po_flags & UTP_PO_PATH_VALIDATION) != 0u ? &connection->candidate_local : &connection->local;
    message->slices = slices;
    if (packet->slice_count == 0u) {
        slices[0].data       = packet->raw_data;
        slices[0].length     = packet->data_size;
        message->slice_count = 1u;
        message->sent_length = 0u;
        return UTP_INTERNAL_ERROR_OK;
    }
    if (packet->slice_count > UTP_PACKET_OUT_MAX_SLICES) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (uint8_t index = 0u; index < packet->slice_count; ++index) {
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
    message->slice_count = packet->slice_count;
    message->sent_length = 0u;
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 完成实际发送后的协议记账。 */
static utp_internal_error_t utp_context_complete_packet_send(utp_context_t*                 context,
                                                             utp_context_connection_slot_t* slot,
                                                             utp_packet_out_t* packet, uint64_t sent_at_us)
{
    const bool     zero_rtt_response = (packet->po_flags & UTP_PO_ZERO_RTT_RESPONSE) != 0u;
    const uint64_t packet_size =
        (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size : packet->data_size;
    utp_internal_error_t error = utp_connection_on_packet_sent(&slot->connection, packet, sent_at_us);

    if (error == UTP_INTERNAL_ERROR_OK && slot->zero_rtt_response_active) {
        slot->zero_rtt_amplification_tx_bytes += packet_size;
    }
    if (error == UTP_INTERNAL_ERROR_OK && zero_rtt_response) {
        error = utp_context_complete_zero_rtt_response(context, slot, sent_at_us);
    }
    return error;
}

static utp_internal_error_t utp_context_send_packet(utp_context_t* context, const utp_connection_t* connection,
                                                    const utp_address_t* peer, const utp_packet_out_t* packet)
{
    const utp_address_t* local;

    if (packet == NULL || packet->raw_data == NULL || packet->data_size == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    local = (packet->po_flags & UTP_PO_PATH_VALIDATION) != 0u ? &connection->candidate_local : &connection->local;
    if ((packet->po_flags & UTP_PO_ENCRYPTED) != 0u) {
        size_t               wire_length;
        utp_internal_error_t error;

        error = utp_connection_encode_packet_wire(connection, packet, context->encrypt_send_buffer,
                                                  sizeof(context->encrypt_send_buffer), &wire_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        return utp_context_send_raw(context, peer, local, context->encrypt_send_buffer, wire_length);
    }
    utp_udp_send_slice_t   slices[UTP_PACKET_OUT_MAX_SLICES];
    utp_udp_send_message_t message;
    utp_internal_error_t   error = utp_context_prepare_packet_send_message(connection, packet, slices, &message);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    message.peer  = peer;
    message.local = local;
    return utp_udp_socket_send_from_to_slices(&context->udp_socket, message.slices, message.slice_count, message.peer,
                                              message.local, &message.sent_length);
}

static void utp_context_send_destroy_close(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    utp_connection_t*    connection;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || !slot->used || !utp_udp_socket_is_open(&context->udp_socket)) {
        return;
    }
    // destroy 路径绕过普通队列，只尝试一次 UDP 写入，失败也不能阻塞同步资源释放。
    connection = &slot->connection;
    error      = utp_connection_prepare_destroy_close(connection);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "prepare destroy close failed");
        return;
    }
    error = utp_context_send_packet(context, connection, &connection->peer, &connection->close_packet);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "best-effort destroy close failed");
    } else {
        utp_context_log_ids(context, UTP_LOG_LEVEL_DEBUG, "destroy close sent", connection->local_cid,
                            connection->peer_cid);
    }
}

static void utp_context_report_terminal_send_error(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                   utp_internal_error_t error, const char* reason)
{
    if (context == NULL || slot == NULL || !slot->used || slot->terminal_error_queued) {
        return;
    }
    // 本地永久发送错误后立即屏蔽收发；回调和资源释放必须等到当前 Context 调度边界。
    slot->terminal_error_status          = utp_internal_error_to_status(error);
    slot->terminal_error_reason          = reason;
    slot->terminal_error_reason_length   = reason == NULL ? 0u : strlen(reason);
    slot->connection.state               = UTP_CONNECTION_STATE_CLOSED;
    slot->connection.local_close_started = true;
    slot->connection.close_pending       = false;
    slot->connection.udp_write_pending   = false;
    slot->terminal_error_queued          = true;
    TAILQ_INSERT_TAIL(&context->terminal_error_slots, slot, terminal_error_next);
}

/** @brief 在 Context 调度边界投递本地永久发送错误，并释放对应连接。 */
static void utp_context_drain_terminal_errors(utp_context_t* context)
{
    while (context != NULL && !TAILQ_EMPTY(&context->terminal_error_slots)) {
        utp_context_connection_slot_t* slot = TAILQ_FIRST(&context->terminal_error_slots);

        TAILQ_REMOVE(&context->terminal_error_slots, slot, terminal_error_next);
        slot->terminal_error_queued = false;
        if (!slot->terminal_error_suppressed) {
            if (slot->connect_pending && !utp_connection_is_connected(&slot->connection)) {
                utp_context_report_connect_error(context, slot->terminal_error_status, slot->terminal_error_reason,
                                                 &slot->connect_attempt);
            } else {
                utp_context_report_connection_error(context, slot, slot->terminal_error_status, 0u,
                                                    (const uint8_t*)slot->terminal_error_reason,
                                                    slot->terminal_error_reason_length, false);
            }
        }
        utp_context_release_connection_slot(context, slot);
    }
}

bool utp_context_suppress_terminal_error(utp_context_t* context, utp_connection_t* connection)
{
    utp_context_connection_slot_t* slot;

    if (context == NULL || connection == NULL || connection->context != context) {
        return false;
    }
    slot = utp_context_find_connection_slot(context, connection->local_cid);
    if (slot == NULL || &slot->connection != connection || !slot->terminal_error_queued) {
        return false;
    }
    slot->terminal_error_suppressed = true;
    return true;
}

static bool utp_context_has_pending_udp_write(const utp_context_t* context)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL) {
        return false;
    }
    if (context->nat_probe.write_pending) {
        return true;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        const utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);

        if (slot->connection.udp_write_pending) {
            return true;
        }
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        const utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);

        if (slot->pending.handshake_write_pending) {
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

static bool utp_context_address_same_ip(const utp_address_t* left, const utp_address_t* right)
{
    size_t address_length;

    if (left->family != right->family) {
        return false;
    }
    if (left->family == UTP_ADDRESS_FAMILY_IPV4) {
        address_length = 4u;
    } else if (left->family == UTP_ADDRESS_FAMILY_IPV6) {
        address_length = 16u;
    } else {
        return false;
    }
    return memcmp(left->address, right->address, address_length) == 0;
}

static bool utp_context_endpoint_equal_address(const utp_endpoint_t* endpoint, const utp_address_t* address)
{
    const size_t address_length = address->family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;

    return endpoint->family == address->family && endpoint->port == address->port &&
           endpoint->scope_id == address->scope_id && memcmp(endpoint->address, address->address, address_length) == 0;
}

/** @brief 判断服务端观测的映射是否等于本机接收该 UDP 响应的实际目的地址。 */
static bool utp_context_endpoint_matches_local_address(const utp_endpoint_t* endpoint, const utp_address_t* address)
{
    size_t address_length;

    if (endpoint->family != address->family || endpoint->port != address->port) {
        return false;
    }
    address_length = address->family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;
    return memcmp(endpoint->address, address->address, address_length) == 0;
}

static bool utp_context_endpoint_equal(const utp_endpoint_t* left, const utp_endpoint_t* right)
{
    size_t address_length;

    if (left->family != right->family || left->port != right->port || left->scope_id != right->scope_id) {
        return false;
    }
    address_length = left->family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : 16u;
    return memcmp(left->address, right->address, address_length) == 0;
}

/** @brief 比较两个公开 endpoint 的地址部分，不比较端口。 */
static bool utp_context_endpoint_same_ip(const utp_endpoint_t* left, const utp_endpoint_t* right)
{
    size_t address_length;

    if (left->family != right->family) {
        return false;
    }
    if (left->family == UTP_ADDRESS_FAMILY_IPV4) {
        address_length = 4u;
    } else if (left->family == UTP_ADDRESS_FAMILY_IPV6) {
        address_length = 16u;
    } else {
        return false;
    }
    return memcmp(left->address, right->address, address_length) == 0;
}

/** @brief 判断 NAT 服务下发的 endpoint 是否为未指定地址。 */
static bool utp_context_address_is_unspecified(const utp_address_t* address)
{
    size_t address_length;

    if (address->family == UTP_ADDRESS_FAMILY_IPV4) {
        address_length = 4u;
    } else if (address->family == UTP_ADDRESS_FAMILY_IPV6) {
        address_length = 16u;
    } else {
        return true;
    }
    for (size_t index = 0u; index < address_length; ++index) {
        if (address->address[index] != 0u) {
            return false;
        }
    }
    return true;
}

static void utp_context_nat_clear_records(utp_nat_probe_task_t* task)
{
    for (uint8_t index = 0u; index < UTP_NAT_PROBE_MAX_IN_FLIGHT; ++index) {
        task->records[index] = (utp_nat_probe_record_t){0};
    }
    task->record_count     = 0u;
    task->batch_sent_count = 0u;
    task->write_pending    = false;
}

static void utp_context_nat_add_port_sample(utp_nat_probe_result_t* result, uint16_t port)
{
    for (uint8_t index = 0u; index < result->port_sample_count; ++index) {
        if (result->port_samples[index] == port) {
            return;
        }
    }
    if (result->port_sample_count < UTP_NAT_PORT_SAMPLE_CAPACITY) {
        result->port_samples[result->port_sample_count] = port;
        ++result->port_sample_count;
    }
}

static void utp_context_nat_update_classification(utp_context_t* context)
{
    utp_nat_probe_task_t* task = &context->nat_probe;
    const utp_address_t*  local_address;

    if (task->primary_response_count == 0u) {
        // 无响应无法区分 UDP 被阻断、路径丢包或服务端故障，不能据此提前判定不可达。
        task->result.nat_class = UTP_NAT_CLASS_UNKNOWN;
    } else if (context->bound_address.family == UTP_ADDRESS_FAMILY_IPV6) {
        if (task->change_ip_port_succeeded) {
            task->result.nat_class = UTP_NAT_CLASS_OPEN_PUBLIC;
        } else if (!task->alternate_valid || task->secondary_response_count == 0u) {
            task->result.nat_class = UTP_NAT_CLASS_UNKNOWN;
        } else if (task->change_port_succeeded) {
            task->result.nat_class = UTP_NAT_CLASS_OPEN_PUBLIC;
        } else {
            task->result.nat_class = UTP_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL;
        }
    } else if (task->change_ip_port_succeeded) {
        // 未向辅助 endpoint 发包前即可收到其响应，已足以确认 Full Cone 过滤行为。
        task->result.nat_class = UTP_NAT_CLASS_FULL_CONE;
    } else if (!task->alternate_valid || task->secondary_response_count == 0u) {
        task->result.nat_class = UTP_NAT_CLASS_UNKNOWN;
    } else if (task->primary_mapping_changed || task->secondary_mapping_changed ||
               !utp_context_endpoint_same_ip(&task->result.primary_mapped_endpoint,
                                             &task->result.secondary_mapped_endpoint)) {
        task->result.nat_class = UTP_NAT_CLASS_SYMMETRIC_MULTI_LINE;
    } else if (!utp_context_endpoint_equal(&task->result.primary_mapped_endpoint,
                                           &task->result.secondary_mapped_endpoint)) {
        task->result.nat_class = UTP_NAT_CLASS_SYMMETRIC;
    } else if (task->change_port_succeeded) {
        task->result.nat_class = UTP_NAT_CLASS_IP_RESTRICTED;
    } else {
        task->result.nat_class = UTP_NAT_CLASS_PORT_RESTRICTED;
    }
    local_address = utp_context_address_is_unspecified(&context->bound_address)
                        ? (task->primary_local_valid ? &task->primary_local : NULL)
                        : &context->bound_address;
    if (local_address != NULL && task->result.primary_mapped_endpoint.family == local_address->family &&
        utp_context_endpoint_matches_local_address(&task->result.primary_mapped_endpoint, local_address) &&
        !task->primary_mapping_changed && !task->secondary_mapping_changed &&
        (task->secondary_response_count == 0u ||
         utp_context_endpoint_equal(&task->result.primary_mapped_endpoint, &task->result.secondary_mapped_endpoint))) {
        task->result.nat_class = task->change_port_succeeded || task->change_ip_port_succeeded
                                     ? UTP_NAT_CLASS_OPEN_PUBLIC
                                     : UTP_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL;
    }
}

static void utp_context_finish_nat_probe(utp_context_t* context, utp_status_t status, uint64_t now_us)
{
    utp_nat_probe_task_t*  task = &context->nat_probe;
    utp_on_nat_probe_fn    callback;
    void*                  user_data;
    utp_nat_probe_result_t result;

    if (!task->active) {
        return;
    }
    callback  = task->callback;
    user_data = task->user_data;
    if (status == UTP_STATUS_OK) {
        utp_context_nat_update_classification(context);
        if (utp_internal_log_enabled(&context->logger, UTP_LOG_LEVEL_DEBUG)) {
            char message[256];

            (void)snprintf(message, sizeof(message),
                           "NAT complete primary=%" PRIu8 " alternate=%" PRIu8 " change_port=%u change_ip=%u class=%u",
                           task->primary_response_count, task->secondary_response_count,
                           task->change_port_succeeded ? 1u : 0u, task->change_ip_port_succeeded ? 1u : 0u,
                           (uint32_t)task->result.nat_class);
            utp_context_log(context, UTP_LOG_LEVEL_DEBUG, message);
        }
        task->result.probe_time_us = now_us;
        task->result.expires_at_us = now_us > UINT64_MAX - UTP_CONTEXT_NAT_RESULT_LIFETIME_US
                                         ? UINT64_MAX
                                         : now_us + UTP_CONTEXT_NAT_RESULT_LIFETIME_US;
        context->nat_result        = task->result;
        context->nat_result_valid  = true;
        result                     = task->result;
    }
    utp_nat_probe_task_reset(task);
    utp_context_disable_udp_write_event_if_idle(context);
    if (callback != NULL) {
        callback(context, status, status == UTP_STATUS_OK ? &result : NULL, user_data);
    }
}

static uint64_t utp_context_nat_round_deadline(const utp_nat_probe_task_t* task)
{
    const uint64_t total_us = (uint64_t)task->phase_timeout_ms * UINT64_C(1000);
    uint64_t       elapsed_us;

    if (task->round == 1u) {
        elapsed_us = total_us / UINT64_C(6);
    } else if (task->round == 2u) {
        elapsed_us = total_us / UINT64_C(2);
    } else {
        elapsed_us = total_us;
    }
    return task->phase_started_us > UINT64_MAX - elapsed_us ? UINT64_MAX : task->phase_started_us + elapsed_us;
}

static utp_internal_error_t utp_context_send_nat_probe_batch(utp_context_t* context, uint64_t now_us)
{
    utp_nat_probe_task_t* task = &context->nat_probe;
    const utp_address_t*  target;

    if (!task->active || task->batch_sent_count >= UTP_NAT_PROBE_BATCH_SIZE) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    target = task->step == UTP_NAT_PROBE_STEP_ALTERNATE_BINDING ? &task->alternate_endpoint : &task->primary_endpoint;
    while (task->batch_sent_count < UTP_NAT_PROBE_BATCH_SIZE) {
        uint8_t              packet[UTP_NAT_PROBE_PACKET_SIZE];
        uint8_t              token[UTP_NAT_PROBE_TOKEN_SIZE];
        uint64_t             packet_number;
        utp_internal_error_t error;

        if (task->record_count >= UTP_NAT_PROBE_MAX_IN_FLIGHT || context->next_nat_probe_packet_number == 0u ||
            context->next_nat_probe_packet_number > UTP_PACKET_NUMBER_MAX) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        error = utp_crypto_random_bytes(token, sizeof(token));
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        packet_number = context->next_nat_probe_packet_number;
        error         = utp_nat_probe_encode_request(packet, packet_number, task->step, token);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_send_raw(context, target, NULL, packet, sizeof(packet));
        }
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            task->write_pending     = true;
            task->round_deadline_us = task->phase_deadline_us;
            return utp_context_enable_udp_write_event(context);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        utp_context_log_nat_endpoint(context, "BindingRequest", task->step, target, packet_number);
        task->records[task->record_count].packet_number = packet_number;
        task->records[task->record_count].sent_at_us    = now_us;
        for (size_t index = 0u; index < sizeof(token); ++index) {
            task->records[task->record_count].token[index] = token[index];
        }
        task->records[task->record_count].used          = true;
        task->records[task->record_count].response_mask = 0u;
        ++task->record_count;
        ++task->batch_sent_count;
        ++context->next_nat_probe_packet_number;
    }
    task->write_pending    = false;
    task->batch_sent_count = 0u;
    ++task->round;
    task->round_deadline_us = utp_context_nat_round_deadline(task);
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 开始一个 NAT 探测步骤，并立即发送该步骤的首轮两个请求。 */
static utp_internal_error_t utp_context_start_nat_step(utp_context_t* context, uint8_t step, uint64_t now_us)
{
    utp_nat_probe_task_t* task       = &context->nat_probe;
    const uint64_t        timeout_us = (uint64_t)task->phase_timeout_ms * UINT64_C(1000);

    task->step              = step;
    task->phase_started_us  = now_us;
    task->phase_deadline_us = now_us > UINT64_MAX - timeout_us ? UINT64_MAX : now_us + timeout_us;
    task->round_deadline_us = 0u;
    task->round             = 0u;
    utp_context_nat_clear_records(task);
    if (utp_internal_log_enabled(&context->logger, UTP_LOG_LEVEL_DEBUG)) {
        char message[320];

        (void)snprintf(message, sizeof(message), "NAT %s start timeout_ms=%" PRIu32,
                       utp_context_nat_step_name(step), task->phase_timeout_ms);
        utp_context_log(context, UTP_LOG_LEVEL_DEBUG, message);
    }
    return utp_context_send_nat_probe_batch(context, now_us);
}

/** @brief 根据当前阶段已获得的证据进入下一阶段或结束探测。 */
static utp_internal_error_t utp_context_advance_nat_probe(utp_context_t* context, uint64_t now_us)
{
    utp_nat_probe_task_t* task = &context->nat_probe;

    if (task->step == UTP_NAT_PROBE_STEP_PRIMARY_BINDING) {
        if (task->primary_response_count == 0u || !task->alternate_valid) {
            utp_context_finish_nat_probe(context, UTP_STATUS_OK, now_us);
            return UTP_INTERNAL_ERROR_OK;
        }
        if (task->change_ip_port_succeeded) {
            utp_context_finish_nat_probe(context, UTP_STATUS_OK, now_us);
            return UTP_INTERNAL_ERROR_OK;
        }
        return utp_context_start_nat_step(context, UTP_NAT_PROBE_STEP_ALTERNATE_BINDING, now_us);
    }
    utp_context_finish_nat_probe(context, UTP_STATUS_OK, now_us);
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 处理 NAT 探测中的不可恢复本地错误，并交付一次失败回调。 */
static void utp_context_fail_nat_probe(utp_context_t* context, utp_internal_error_t error, uint64_t now_us)
{
    utp_context_finish_nat_probe(context, utp_internal_error_to_status(error), now_us);
}

/** @brief 在当前步骤按 UTP 包号查找探测记录。 */
static utp_nat_probe_record_t* utp_context_find_nat_probe_record(utp_nat_probe_task_t* task, uint64_t packet_number)
{
    for (uint8_t index = 0u; index < task->record_count; ++index) {
        utp_nat_probe_record_t* record = &task->records[index];

        if (record->used && record->packet_number == packet_number) {
            return record;
        }
    }
    return NULL;
}

/** @brief 判断响应来源是否符合发起阶段的 NAT 服务协同计划。 */
static bool utp_context_nat_response_source_is_valid(const utp_nat_probe_task_t* task, const utp_address_t* peer,
                                                     const utp_nat_probe_response_t* response)
{
    if (!utp_address_equal(peer, &response->origin)) {
        return false;
    }
    if (task->step == UTP_NAT_PROBE_STEP_ALTERNATE_BINDING) {
        if (response->change_flags == UTP_NAT_PROBE_CHANGE_NONE) {
            return utp_address_equal(peer, &task->alternate_endpoint);
        }
        return response->change_flags == UTP_NAT_PROBE_CHANGE_PORT &&
               utp_context_address_same_ip(peer, &task->alternate_endpoint) &&
               peer->port != task->alternate_endpoint.port;
    }
    if (response->change_flags == UTP_NAT_PROBE_CHANGE_NONE) {
        return utp_address_equal(peer, &task->primary_endpoint);
    }
    return response->change_flags == UTP_NAT_PROBE_CHANGE_BOTH && response->has_alternate &&
           utp_context_address_same_ip(peer, &response->alternate) && peer->port != response->alternate.port &&
           !utp_context_address_same_ip(peer, &task->primary_endpoint) &&
           (!task->alternate_valid || utp_address_equal(&response->alternate, &task->alternate_endpoint));
}

static uint8_t utp_context_nat_response_mask(uint8_t change_flags)
{
    if (change_flags == UTP_NAT_PROBE_CHANGE_PORT) {
        return UTP_NAT_PROBE_RESPONSE_CHANGE_PORT;
    }
    if (change_flags == UTP_NAT_PROBE_CHANGE_BOTH) {
        return UTP_NAT_PROBE_RESPONSE_CHANGE_IP_PORT;
    }
    return UTP_NAT_PROBE_RESPONSE_PRIMARY;
}

static bool utp_context_nat_accept_alternate(utp_nat_probe_task_t* task, const utp_nat_probe_response_t* response)
{
    if (!response->has_alternate) {
        return response->change_flags != UTP_NAT_PROBE_CHANGE_BOTH;
    }
    if (response->alternate.family != task->primary_endpoint.family ||
        utp_context_address_is_unspecified(&response->alternate) ||
        utp_context_address_same_ip(&response->alternate, &task->primary_endpoint)) {
        return false;
    }
    if (task->alternate_valid && !utp_address_equal(&response->alternate, &task->alternate_endpoint)) {
        task->alternate_endpoint   = (utp_address_t){0};
        task->alternate_valid      = false;
        task->alternate_conflicted = true;
        return false;
    }
    if (!task->alternate_conflicted) {
        task->alternate_endpoint = response->alternate;
        task->alternate_valid    = true;
    }
    return !task->alternate_conflicted;
}

/** @brief 消费一条通过认证和来源校验的 NAT 探测响应。 */
static utp_internal_error_t utp_context_on_nat_probe_packet(utp_context_t* context, const utp_packet_header_t* header,
                                                            const uint8_t* payload, size_t payload_length,
                                                            const utp_address_t* peer, const utp_address_t* local,
                                                            uint64_t now_us)
{
    utp_nat_probe_task_t*    task = &context->nat_probe;
    utp_nat_probe_response_t response;
    utp_nat_probe_record_t*  record;
    utp_internal_error_t     error;
    uint8_t                  response_mask;
    uint64_t                 rtt_us;

    if (!task->active) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (header->type != UTP_PACKET_TYPE_NAT_PROBE || header->scid != 0u || header->dcid != 0u ||
        header->packet_number == 0u || header->reserve != 0u ||
        payload_length > UTP_NAT_PROBE_PACKET_SIZE - UTP_PACKET_HEADER_SIZE) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number,
                                              "invalid_header");
        return UTP_INTERNAL_ERROR_OK;
    }
    error = utp_nat_probe_decode_response(payload, payload_length, &response);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number, "decode_failed");
        return UTP_INTERNAL_ERROR_OK;
    }
    if (response.mapped.family != context->bound_address.family ||
        response.origin.family != context->bound_address.family) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number,
                                              "address_family_mismatch");
        return UTP_INTERNAL_ERROR_OK;
    }
    if (response.step != task->step) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number, "step_mismatch");
        return UTP_INTERNAL_ERROR_OK;
    }
    record = utp_context_find_nat_probe_record(task, header->packet_number);
    if (record == NULL) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number,
                                              "packet_number_mismatch");
        return UTP_INTERNAL_ERROR_OK;
    }
    if (response.token_length != sizeof(record->token) ||
        memcmp(response.token, record->token, sizeof(record->token)) != 0) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number,
                                              "token_mismatch");
        return UTP_INTERNAL_ERROR_OK;
    }
    if (!utp_context_nat_response_source_is_valid(task, peer, &response)) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number,
                                              "source_mismatch");
        return UTP_INTERNAL_ERROR_OK;
    }
    if (task->step == UTP_NAT_PROBE_STEP_PRIMARY_BINDING && !utp_context_nat_accept_alternate(task, &response)) {
        utp_context_log_nat_response_rejected(context, task->step, peer, local, header->packet_number,
                                              "alternate_endpoint_mismatch");
        return UTP_INTERNAL_ERROR_OK;
    }
    response_mask = utp_context_nat_response_mask(response.change_flags);
    if ((record->response_mask & response_mask) != 0u) {
        return UTP_INTERNAL_ERROR_OK;
    }
    record->response_mask |= response_mask;
    rtt_us                 = now_us > record->sent_at_us ? now_us - record->sent_at_us : 0u;
    utp_context_log_nat_response(context, task->step, peer, &response, header->packet_number, local, rtt_us);
    if (task->step == UTP_NAT_PROBE_STEP_PRIMARY_BINDING && response.change_flags == UTP_NAT_PROBE_CHANGE_NONE) {
        if (task->primary_response_count == 0u) {
            utp_context_endpoint_from_address(&task->result.primary_mapped_endpoint, &response.mapped);
            if (utp_context_address_is_unspecified(&context->bound_address) && local != NULL &&
                local->family == context->bound_address.family && !utp_context_address_is_unspecified(local)) {
                task->primary_local       = *local;
                task->primary_local_valid = true;
            }
        } else if (!utp_context_endpoint_equal_address(&task->result.primary_mapped_endpoint, &response.mapped)) {
            task->primary_mapping_changed = true;
        }
        ++task->primary_response_count;
        task->primary_rtt_sum_us += rtt_us;
        task->result.primary_rtt_ms =
            (int32_t)(task->primary_rtt_sum_us / task->primary_response_count / UINT64_C(1000));
        utp_context_nat_add_port_sample(&task->result, response.mapped.port);
    } else if (task->step == UTP_NAT_PROBE_STEP_PRIMARY_BINDING) {
        task->change_ip_port_succeeded = true;
    } else if (response.change_flags == UTP_NAT_PROBE_CHANGE_NONE) {
        if (task->secondary_response_count == 0u) {
            utp_context_endpoint_from_address(&task->result.secondary_mapped_endpoint, &response.mapped);
        } else if (!utp_context_endpoint_equal_address(&task->result.secondary_mapped_endpoint, &response.mapped)) {
            task->secondary_mapping_changed = true;
        }
        ++task->secondary_response_count;
        task->secondary_rtt_sum_us += rtt_us;
        task->result.secondary_rtt_ms =
            (int32_t)(task->secondary_rtt_sum_us / task->secondary_response_count / UINT64_C(1000));
        utp_context_nat_add_port_sample(&task->result, response.mapped.port);
    } else {
        task->change_port_succeeded = true;
    }
    if (task->primary_response_count != 0u && task->change_ip_port_succeeded) {
        utp_context_finish_nat_probe(context, UTP_STATUS_OK, now_us);
    } else if (task->step == UTP_NAT_PROBE_STEP_ALTERNATE_BINDING && task->secondary_response_count != 0u &&
               (task->change_port_succeeded || task->primary_mapping_changed || task->secondary_mapping_changed ||
                !utp_context_endpoint_equal(&task->result.primary_mapped_endpoint,
                                            &task->result.secondary_mapped_endpoint))) {
        utp_context_finish_nat_probe(context, UTP_STATUS_OK, now_us);
    }
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 推进 NAT 探测的重发轮次及阶段总超时。 */
static utp_internal_error_t utp_context_process_nat_probe_timer(utp_context_t* context, uint64_t now_us)
{
    utp_nat_probe_task_t* task = &context->nat_probe;
    utp_internal_error_t  error;

    if (context->nat_result_valid && context->nat_result.expires_at_us <= now_us) {
        context->nat_result_valid = false;
    }
    if (!task->active) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (task->phase_deadline_us <= now_us) {
        // 一个完整轮次的两包都未提交到内核，不能伪装为远端无响应。
        if (task->round == 0u) {
            utp_context_fail_nat_probe(context, UTP_INTERNAL_ERROR_IO, now_us);
            return UTP_INTERNAL_ERROR_OK;
        }
        if (utp_internal_log_enabled(&context->logger, UTP_LOG_LEVEL_DEBUG)) {
            char message[192];

            (void)snprintf(message, sizeof(message), "NAT %s timeout rounds=%" PRIu8 " responses=%" PRIu8,
                           utp_context_nat_step_name(task->step), task->round,
                           task->step == UTP_NAT_PROBE_STEP_PRIMARY_BINDING ? task->primary_response_count
                                                                            : task->secondary_response_count);
            utp_context_log(context, UTP_LOG_LEVEL_DEBUG, message);
        }
        error = utp_context_advance_nat_probe(context, now_us);
    } else if (!task->write_pending && task->round_deadline_us <= now_us && task->round < UTP_NAT_PROBE_MAX_ROUNDS) {
        error = utp_context_send_nat_probe_batch(context, now_us);
    } else {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_fail_nat_probe(context, error, now_us);
    }
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 在 UDP 重新可写后继续当前 NAT 探测轮次的未发送请求。 */
static utp_internal_error_t utp_context_retry_nat_probe_send(utp_context_t* context, uint64_t now_us)
{
    utp_nat_probe_task_t* task = &context->nat_probe;
    utp_internal_error_t  error;

    if (!task->active || !task->write_pending) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (task->phase_deadline_us <= now_us) {
        return utp_context_process_nat_probe_timer(context, now_us);
    }
    error = utp_context_send_nat_probe_batch(context, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_fail_nat_probe(context, error, now_us);
    }
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 在收到首个有效 1-RTT 包前，将 0-RTT 路径发送量限制为已认证接收量的三倍。 */
static bool utp_context_zero_rtt_amplification_allows(const utp_context_connection_slot_t* slot,
                                                      const utp_packet_out_t*              packet)
{
    uint64_t limit;
    uint64_t packet_size;

    if (!slot->zero_rtt_response_active || slot->connection.role != UTP_CONNECTION_ROLE_PASSIVE) {
        return true;
    }
    limit       = slot->zero_rtt_amplification_rx_bytes > UINT64_MAX / UINT64_C(3)
                      ? UINT64_MAX
                      : slot->zero_rtt_amplification_rx_bytes * UINT64_C(3);
    packet_size = (packet->po_flags & UTP_PO_ENCRYPTED) != 0u ? packet->encrypt_data_size : packet->data_size;
    return slot->zero_rtt_amplification_tx_bytes <= limit &&
           packet_size <= limit - slot->zero_rtt_amplification_tx_bytes;
}

static utp_internal_error_t utp_context_flush_connection_at(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                            uint64_t now_us)
{
    utp_connection_t* connection;

    if (context == NULL || slot == NULL || !slot->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection = &slot->connection;
    utp_send_control_pacer_tick_in(&connection->send_control, now_us);
    for (;;) {
        utp_packet_out_t*    packet = utp_connection_next_packet_to_send_at(connection, now_us);
        utp_internal_error_t error;

        if (packet == NULL) {
            connection->udp_write_pending = false;
            utp_send_control_pacer_tick_out(&connection->send_control);
            return UTP_INTERNAL_ERROR_OK;
        }
        if (!utp_context_zero_rtt_amplification_allows(slot, packet)) {
            // 新的已认证 0-RTT 请求会增加额度并再次触发 flush；总过期定时器负责最终清理。
            (void)utp_send_control_reschedule_packet(&connection->send_control, packet);
            utp_send_control_pacer_tick_out(&connection->send_control);
            return UTP_INTERNAL_ERROR_OK;
        }
        if (slot->zero_rtt_response_active && slot->zero_rtt_response_sent &&
            (packet->po_flags & UTP_PO_ZERO_RTT_RESPONSE) == 0u) {
            // 客户端尚未用 1-RTT CTRL 证实收到 HANDSHAKE；业务包和新票据必须留在队列，
            // 避免客户端在尚未获知对端 CID 时把它们视为协议错误。
            (void)utp_send_control_reschedule_packet(&connection->send_control, packet);
            utp_send_control_pacer_tick_out(&connection->send_control);
            return UTP_INTERNAL_ERROR_OK;
        }
#if defined(UTP_HAVE_SENDMMSG)
        if ((packet->po_flags & UTP_PO_ENCRYPTED) == 0u && !utp_connection_is_close_packet(connection, packet)) {
            utp_packet_out_t*      packets[UTP_UDP_SOCKET_BATCH_SIZE];
            utp_udp_send_message_t messages[UTP_UDP_SOCKET_BATCH_SIZE];
            utp_udp_send_slice_t   slices[UTP_UDP_SOCKET_BATCH_SIZE][UTP_PACKET_OUT_MAX_SLICES];
            size_t                 packet_count    = 0u;
            size_t                 sent_count      = 0u;
            size_t                 completed_count = 0u;

            for (;;) {
                error = utp_context_prepare_packet_send_message(connection, packet, slices[packet_count],
                                                                &messages[packet_count]);
                if (error != UTP_INTERNAL_ERROR_OK) {
                    (void)utp_send_control_reschedule_packet(&connection->send_control, packet);
                    break;
                }
                packets[packet_count] = packet;
                ++packet_count;
                if (packet_count == UTP_UDP_SOCKET_BATCH_SIZE) {
                    break;
                }
                packet = utp_connection_next_packet_to_send_at(connection, now_us);
                if (packet == NULL) {
                    break;
                }
                if (!utp_context_zero_rtt_amplification_allows(slot, packet) ||
                    (slot->zero_rtt_response_active && slot->zero_rtt_response_sent &&
                     (packet->po_flags & UTP_PO_ZERO_RTT_RESPONSE) == 0u) ||
                    (packet->po_flags & UTP_PO_ENCRYPTED) != 0u || utp_connection_is_close_packet(connection, packet)) {
                    error = utp_send_control_reschedule_packet(&connection->send_control, packet);
                    if (error != UTP_INTERNAL_ERROR_OK) {
                        break;
                    }
                    error = UTP_INTERNAL_ERROR_OK;
                    break;
                }
            }
            if (packet_count != 0u && error == UTP_INTERNAL_ERROR_OK) {
                error = utp_udp_socket_send_messages(&context->udp_socket, messages, packet_count, &sent_count);
            }
            while (completed_count < sent_count && error == UTP_INTERNAL_ERROR_OK) {
                error = utp_context_complete_packet_send(context, slot, packets[completed_count], now_us);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    ++completed_count;
                }
            }
            if (error != UTP_INTERNAL_ERROR_OK && completed_count < sent_count) {
                for (size_t index = packet_count; index > sent_count; --index) {
                    const utp_internal_error_t reschedule_error =
                        utp_send_control_reschedule_packet(&connection->send_control, packets[index - 1u]);

                    if (reschedule_error != UTP_INTERNAL_ERROR_OK) {
                        break;
                    }
                }
                utp_send_control_pacer_tick_out(&connection->send_control);
                return error;
            }
            if (error == UTP_INTERNAL_ERROR_OK && sent_count == packet_count) {
                continue;
            }
            if (error == UTP_INTERNAL_ERROR_OK || error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
                for (size_t index = packet_count; index > sent_count; --index) {
                    error = utp_send_control_reschedule_packet(&connection->send_control, packets[index - 1u]);
                    if (error != UTP_INTERNAL_ERROR_OK) {
                        break;
                    }
                }
                if (error == UTP_INTERNAL_ERROR_OK) {
                    if (!connection->udp_write_pending) {
                        utp_context_log_ids(context, UTP_LOG_LEVEL_DEBUG, "udp batch send blocked, packets rescheduled",
                                            connection->local_cid, connection->peer_cid);
                    }
                    connection->udp_write_pending = true;
                    error                         = utp_context_enable_udp_write_event(context);
                }
                utp_send_control_pacer_tick_out(&connection->send_control);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    return UTP_INTERNAL_ERROR_OK;
                }
                utp_context_report_terminal_send_error(context, slot, error, "failed to wait for udp writable");
                return error;
            }
            if (sent_count < packet_count) {
                utp_packet_out_t* failed_packet = packets[sent_count];

                utp_connection_on_packet_send_error(connection, failed_packet, error, now_us);
                utp_connection_on_packet_abandoned(connection, failed_packet);
                utp_send_control_forget_packet_attempts(&connection->send_control, failed_packet);
                utp_packet_out_pool_release(&connection->packet_pool, failed_packet);
                if (error != UTP_INTERNAL_ERROR_NOBUFS) {
                    for (size_t index = packet_count; index > sent_count + 1u; --index) {
                        const utp_internal_error_t reschedule_error =
                            utp_send_control_reschedule_packet(&connection->send_control, packets[index - 1u]);

                        if (reschedule_error != UTP_INTERNAL_ERROR_OK) {
                            error = reschedule_error;
                            break;
                        }
                    }
                }
            }
            utp_send_control_pacer_tick_out(&connection->send_control);
            if (error == UTP_INTERNAL_ERROR_NOBUFS) {
                utp_context_report_terminal_send_error(context, slot, error, "udp send ENOBUFS");
            }
            return error;
        }
#endif
        error = utp_context_send_packet(context, connection,
                                        packet->has_destination ? &packet->destination : &connection->peer, packet);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_complete_packet_send(context, slot, packet, now_us);
        } else if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            // 暂时不可写时保留 PacketOut 所有权，由一次性 writable 事件继续发送。
            if (utp_connection_is_close_packet(connection, packet)) {
                if (!connection->udp_write_pending) {
                    utp_context_log_ids(context, UTP_LOG_LEVEL_DEBUG, "udp send blocked, waiting for writable",
                                        connection->local_cid, connection->peer_cid);
                }
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
                if (!connection->udp_write_pending) {
                    utp_context_log_ids(context, UTP_LOG_LEVEL_DEBUG, "udp send blocked, packet rescheduled",
                                        connection->local_cid, connection->peer_cid);
                }
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
                utp_connection_on_packet_send_error(connection, packet, error, now_us);
                utp_connection_on_packet_abandoned(connection, packet);
                utp_send_control_forget_packet_attempts(&connection->send_control, packet);
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

static utp_internal_error_t utp_context_flush_connection(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    return utp_context_flush_connection_at(context, slot, utp_context_now_us());
}

utp_internal_error_t utp_context_flush_public_connection(utp_context_t* context, utp_connection_t* connection)
{
    utp_context_connection_slot_t* slot;
    utp_internal_error_t           error;
    uint64_t                       now_us;

    if (context == NULL || connection == NULL || connection->context != context) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    slot = utp_context_find_connection_slot(context, connection->local_cid);
    if (slot == NULL || &slot->connection != connection) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    if (connection->user_callback_depth != 0u) {
        connection->public_flush_pending = true;
        return UTP_INTERNAL_ERROR_OK;
    }
    now_us                           = utp_context_now_us();
    connection->last_public_flush_us = now_us;
    error                            = utp_context_flush_connection_at(context, slot, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        if (slot->terminal_error_queued) {
            const utp_internal_error_t refresh_error = utp_context_refresh_timer(context, now_us);

            if (refresh_error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, refresh_error,
                                       "terminal error timer refresh failed");
            }
        }
        return error;
    }
    return utp_context_refresh_timer(context, now_us);
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
                                                            size_t payload_length, uint64_t* out_packet_number)
{
    uint8_t              packet[UTP_PACKET_MTU_FLOOR];
    utp_packet_header_t  header;
    size_t               wire_payload_length;
    size_t               encrypted_length;
    bool                 encrypt;
    utp_internal_error_t error;

    uint64_t             packet_number;

    if (out_packet_number != NULL) {
        *out_packet_number = 0u;
    }
    if (context == NULL || pending == NULL || pending->next_packet_number == 0u ||
        pending->next_packet_number > UTP_PACKET_NUMBER_MAX || (payload == NULL && payload_length != 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    packet_number = pending->next_packet_number;
    // Handshake 发出前对端尚无服务端公钥；发出后 pending 的关闭包必须与数据面一样经过 AEAD。
    encrypt = pending->crypto_ready && pending->handshake_sent && packet_type != UTP_PACKET_TYPE_INITIAL &&
              packet_type != UTP_PACKET_TYPE_HANDSHAKE;
    if (encrypt && payload_length > SIZE_MAX - UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    wire_payload_length = payload_length + (encrypt ? UTP_CRYPTO_AEAD_TAG_SIZE : 0u);
    if (wire_payload_length > sizeof(packet) - UTP_PACKET_HEADER_SIZE || wire_payload_length > UINT16_MAX) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }
    header = (utp_packet_header_t){
        pending->local_cid, pending->peer_cid, packet_number, (uint16_t)wire_payload_length, packet_type, 0u};
    error = utp_proto_encode_header(packet, sizeof(packet), &header);
    if (error == UTP_INTERNAL_ERROR_OK && encrypt) {
        error = utp_crypto_aead_seal(&pending->tx_aead, packet_number, payload, payload_length, packet,
                                     UTP_PACKET_HEADER_SIZE, packet + UTP_PACKET_HEADER_SIZE,
                                     sizeof(packet) - UTP_PACKET_HEADER_SIZE, &encrypted_length);
        if (error == UTP_INTERNAL_ERROR_OK && encrypted_length != wire_payload_length) {
            error = UTP_INTERNAL_ERROR_PROTOCOL;
        }
    } else if (error == UTP_INTERNAL_ERROR_OK && payload_length != 0u) {
        memcpy(packet + UTP_PACKET_HEADER_SIZE, payload, payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_send_raw(context, &pending->peer, &pending->local, packet,
                                     UTP_PACKET_HEADER_SIZE + wire_payload_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        ++pending->next_packet_number;
        if (out_packet_number != NULL) {
            *out_packet_number = packet_number;
        }
    }
    return error;
}

static utp_internal_error_t utp_context_send_pending_handshake(utp_context_t* context, utp_pending_incoming_t* pending,
                                                               uint64_t* out_packet_number)
{
    uint8_t              payload[UTP_FRAME_VERSION_SIZE + UTP_FRAME_CRYPTO_SIZE + UTP_FRAME_TRANSPORT_PARAMS_SIZE +
                    UTP_FRAME_ACK_FREQUENCY_SIZE + UTP_ACK_FRAME_HEADER_SIZE + UTP_FRAME_HANDSHAKE_DELAY_SIZE];
    size_t               payload_length;
    utp_internal_error_t error;

    if (out_packet_number == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_context_encode_version_frame(payload, sizeof(payload), &payload_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (pending->crypto_configured) {
        error = utp_pending_incoming_encode_crypto(pending, payload + payload_length, sizeof(payload) - payload_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        payload_length += UTP_FRAME_CRYPTO_SIZE;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_frame_transport_params_encode(payload + payload_length, sizeof(payload) - payload_length,
                                                  &context->local_transport_params);
        if (error == UTP_INTERNAL_ERROR_OK) {
            payload_length += UTP_FRAME_TRANSPORT_PARAMS_SIZE;
            error           = utp_frame_ack_frequency_encode(payload + payload_length, sizeof(payload) - payload_length,
                                                             &context->local_ack_frequency);
            if (error == UTP_INTERNAL_ERROR_OK) {
                payload_length += UTP_FRAME_ACK_FREQUENCY_SIZE;
            }
        }
    }
    if (pending->latest_initial_packet_number == 0u || pending->latest_initial_received_us == 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    error = utp_context_append_handshake_feedback(payload, sizeof(payload), &payload_length,
                                                  pending->latest_initial_packet_number,
                                                  pending->latest_initial_received_us, utp_context_now_us());
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_context_send_pending_packet(context, pending, UTP_PACKET_TYPE_HANDSHAKE, payload, payload_length,
                                           out_packet_number);
}

/** @brief 发送普通被动 HANDSHAKE；UDP 暂不可写时保留 pending，等待 writable 事件重试。 */
static utp_internal_error_t utp_context_send_or_defer_pending_handshake(utp_context_t*              context,
                                                                        utp_context_pending_slot_t* slot,
                                                                        uint64_t                    now_us)
{
    uint64_t             packet_number = 0u;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || !slot->used || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_context_send_pending_handshake(context, &slot->pending, &packet_number);
    if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
        slot->pending.handshake_write_pending = true;
        return utp_context_enable_udp_write_event(context);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    slot->pending.handshake_write_pending = false;
    return utp_pending_incoming_mark_handshake_sent(&slot->pending, packet_number, now_us);
}

static void utp_context_send_pending_close(utp_context_t* context, utp_pending_incoming_t* pending, uint16_t error_code)
{
    uint8_t                            payload[UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE];
    const utp_frame_connection_close_t close = {error_code, NULL, 0u};

    if (utp_frame_connection_close_encode(payload, sizeof(payload), &close) == UTP_INTERNAL_ERROR_OK) {
        (void)utp_context_send_pending_packet(context, pending, UTP_PACKET_TYPE_CONNECTION_CLOSE, payload,
                                              sizeof(payload), NULL);
    }
}

/** @brief 接受指定的普通 pending，避免回调重入时误取队列中的其他连接。 */
static utp_internal_error_t utp_context_accept_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot)
{
    utp_internal_error_t error;
    uint64_t             now_us;

    if (context == NULL || slot == NULL || !slot->used || !slot->queued) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    slot->queued = false;
    error        = utp_pending_incoming_accept(&slot->pending);
    now_us       = utp_context_now_us();
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_send_or_defer_pending_handshake(context, slot, now_us);
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
        utp_context_release_pending_slot(context, slot);
        return error;
    }
    utp_context_log_ids(context, UTP_LOG_LEVEL_INFO, "incoming connection accepted", slot->pending.local_cid,
                        slot->pending.peer_cid);
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_context_report_connected(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (!slot->connected_reported && utp_connection_is_connected(&slot->connection)) {
        slot->connect_pending     = false;
        slot->connect_deadline_us = 0u;
        slot->connected_reported  = true;
        utp_context_log_ids(context, UTP_LOG_LEVEL_INFO, "connection established", slot->connection.local_cid,
                            slot->connection.peer_cid);
        // 握手已完成，不再需要保存用于构造 0-RTT 重传的应用数据。
        utp_allocator_free(NULL, slot->zero_rtt_early_data);
        slot->zero_rtt_early_data      = NULL;
        slot->zero_rtt_early_data_size = 0u;
        if (context->on_connected != NULL) {
            context->on_connected(&slot->connection, context->on_connected_user_data);
        }
    }
}

/** @brief 在握手响应真正发出后报告连接并签发新的恢复凭证。 */
static utp_internal_error_t utp_context_complete_connected_side_effects(utp_context_t*                 context,
                                                                        utp_context_connection_slot_t* slot)
{
    if (!utp_connection_is_connected(&slot->connection)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    utp_context_report_connected(context, slot);
    if (!slot->used || !utp_connection_is_connected(&slot->connection)) {
        return UTP_INTERNAL_ERROR_OK;
    }
    return utp_context_queue_session_token(context, slot);
}

/** @brief 计算 0-RTT 响应的指数退避间隔，指数按已经完成的重传次数增长。 */
static uint64_t utp_context_zero_rtt_response_delay(const utp_context_t* context, uint8_t retries)
{
    uint64_t delay_us = (uint64_t)context->handshake_timeout_ms * UINT64_C(1000);
    uint8_t  exponent = retries > 8u ? 8u : retries;

    while (exponent-- != 0u && delay_us <= UINT64_MAX / 2u) {
        delay_us *= 2u;
    }
    return delay_us;
}

/** @brief 固定一次 0-RTT attempt 的总寿命，重复请求不得延长该截止时间。 */
static uint64_t utp_context_zero_rtt_expire_deadline(const utp_context_t* context, uint64_t now_us)
{
    uint64_t deadline = now_us;
    uint16_t attempt;

    for (attempt = 0u; attempt <= (uint16_t)context->handshake_max_retries; ++attempt) {
        const uint64_t delay_us =
            utp_context_zero_rtt_response_delay(context, attempt > UINT8_MAX ? UINT8_MAX : (uint8_t)attempt);

        if (delay_us > UINT64_MAX - deadline) {
            return UINT64_MAX;
        }
        deadline += delay_us;
    }
    return deadline;
}

/** @brief 在 0-RTT HANDSHAKE 真正写入 UDP 后提交连接状态并仅投递一次 early 数据。 */
static utp_internal_error_t utp_context_complete_zero_rtt_response(utp_context_t*                 context,
                                                                   utp_context_connection_slot_t* slot, uint64_t now_us)
{
    utp_internal_error_t error = UTP_INTERNAL_ERROR_OK;

    if (context == NULL || slot == NULL || !slot->used || now_us == 0u || !slot->zero_rtt_response_active ||
        !slot->zero_rtt_response_queued || !utp_connection_is_connected(&slot->connection)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    slot->zero_rtt_response_queued = false;
    if (slot->zero_rtt_response_sent) {
        if (slot->zero_rtt_response_retries != UINT8_MAX) {
            ++slot->zero_rtt_response_retries;
        }
    } else {
        slot->zero_rtt_response_sent = true;
    }
    if (!slot->zero_rtt_early_delivered && slot->zero_rtt_early_packet != NULL) {
        error = utp_connection_on_plaintext_packet_in_received(&slot->connection, slot->zero_rtt_early_packet,
                                                               slot->zero_rtt_early_wire_size, &slot->connection.peer,
                                                               now_us);
        if (error == UTP_INTERNAL_ERROR_OK) {
            slot->zero_rtt_early_delivered = true;
        }
        utp_packet_in_release(slot->zero_rtt_early_packet);
        slot->zero_rtt_early_packet    = NULL;
        slot->zero_rtt_early_wire_size = 0u;
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        const utp_status_t status = utp_internal_error_to_status(error);
        const char*        reason = utp_status_string(status);
        const uint16_t close_code = status < 0 && status >= -((int32_t)UINT16_MAX) ? (uint16_t)(-status) : UINT16_C(1);

        (void)utp_connection_retire_handshake_flight(&slot->connection, now_us);
        slot->zero_rtt_response_active      = false;
        slot->zero_rtt_response_queued      = false;
        slot->zero_rtt_response_deadline_us = 0u;
        slot->zero_rtt_expire_deadline_us   = 0u;
        if (utp_context_is_peer_protocol_error(error)) {
            return utp_context_close_on_peer_protocol_error(context, slot, error);
        }
        utp_context_report_connection_error(context, slot, status, 0u, (const uint8_t*)reason, strlen(reason), false);
        error = utp_connection_queue_close(&slot->connection, close_code);
        return error == UTP_INTERNAL_ERROR_OK ? utp_context_flush_connection(context, slot) : error;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_complete_connected_side_effects(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK && slot->zero_rtt_response_active) {
        const uint64_t delay_us = utp_context_zero_rtt_response_delay(context, slot->zero_rtt_response_retries);

        slot->zero_rtt_response_deadline_us = delay_us > UINT64_MAX - now_us ? UINT64_MAX : now_us + delay_us;
        if (slot->zero_rtt_expire_deadline_us != 0u &&
            slot->zero_rtt_response_deadline_us > slot->zero_rtt_expire_deadline_us) {
            slot->zero_rtt_response_deadline_us = slot->zero_rtt_expire_deadline_us;
        }
    }
    return error;
}

static void utp_context_report_connection_error(utp_context_t* context, utp_context_connection_slot_t* slot,
                                                utp_status_t status, uint16_t peer_error_code, const uint8_t* reason,
                                                size_t reason_length, bool peer_initiated)
{
    if (!slot->connection_error_reported) {
        // reason 可能直接引用当前 PacketIn，必须在释放接收包之前同步完成回调。
        const utp_connection_error_info_t info = {
            status, peer_error_code, reason, reason_length, peer_initiated,
        };

        slot->connection_error_reported = true;
        utp_context_log_close(context, slot, status, peer_error_code, peer_initiated);
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
    uint8_t*             payload;
    size_t               payload_capacity;
    size_t               payload_length;
    size_t               early_data_length;
    uint32_t             local_cid;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || peer == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    payload          = context->encrypt_send_buffer;
    payload_capacity = sizeof(context->encrypt_send_buffer);
    if (slot->connection.local_cid != 0u) {
        // 同一主动 attempt 的 Initial 重试必须沿用 CID，服务端才能命中既有 pending 而不重复回调 accept。
        local_cid = slot->connection.local_cid;
        utp_context_unregister_connection_slot(context, slot);
        utp_connection_cleanup(&slot->connection);
        error = UTP_INTERNAL_ERROR_OK;
    } else {
        error = utp_context_alloc_cid(context, &local_cid);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_ACTIVE, local_cid, 0u, peer,
                                    UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_configure_connection(context, &slot->connection);
    }
    if (error == UTP_INTERNAL_ERROR_OK && slot->connect_attempt.encryption != UTP_ENCRYPTION_NONE) {
        uint8_t crypto_type;

        if (!utp_context_crypto_type_from_encryption(slot->connect_attempt.encryption, &crypto_type)) {
            error = UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        } else if (slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE) {
            error = utp_connection_configure_zero_rtt_crypto(
                &slot->connection, slot->zero_rtt_resumption_psk, slot->zero_rtt_session_token,
                slot->zero_rtt_session_token + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE, crypto_type);
        } else {
            error = utp_connection_configure_crypto(&slot->connection, crypto_type);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_register_connection_slot(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK && (slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN ||
                                           slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE)) {
        utp_frame_session_token_t token;

        early_data_length        = 0u;
        token.payload            = slot->zero_rtt_session_token;
        token.payload_length     = UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE;
        token.expires_at_seconds = slot->zero_rtt_expires_at_seconds;
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_frame_session_token_encode(payload, payload_capacity, &token);
        }
        payload_length = UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE;
        if (error == UTP_INTERNAL_ERROR_OK && slot->connection.zero_rtt_encrypted) {
            const size_t token_length = payload_length;

            error = utp_connection_encode_crypto(&slot->connection, payload + payload_length,
                                                 payload_capacity - payload_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                payload_length += UTP_FRAME_CRYPTO_SIZE;
                error = utp_context_append_zero_rtt_parameters(payload, payload_capacity, &payload_length, false, 0u);
            }
            if (error == UTP_INTERNAL_ERROR_OK && (slot->zero_rtt_early_data_size != 0u || slot->zero_rtt_early_fin)) {
                const uint16_t target_size =
                    utp_mtu_packet_size_from_mtu(slot->connection.mtu_discovery.mtu_min, slot->connection.peer.family);
                const size_t fixed_length = UTP_PACKET_HEADER_SIZE + UTP_CRYPTO_AEAD_TAG_SIZE + payload_length;

                if (target_size < fixed_length + UTP_FRAME_STREAM_HEADER_SIZE + UTP_FRAME_PING_SIZE) {
                    error = UTP_INTERNAL_ERROR_LIMIT;
                } else {
                    early_data_length = slot->zero_rtt_early_data_size;
                    if (early_data_length >
                        (size_t)target_size - fixed_length - UTP_FRAME_STREAM_HEADER_SIZE - UTP_FRAME_PING_SIZE) {
                        early_data_length =
                            (size_t)target_size - fixed_length - UTP_FRAME_STREAM_HEADER_SIZE - UTP_FRAME_PING_SIZE;
                    }
                    error = utp_connection_reserve_zero_rtt_stream(&slot->connection, slot->zero_rtt_early_data,
                                                                   slot->zero_rtt_early_data_size, early_data_length,
                                                                   slot->zero_rtt_early_fin);
                }
            }
            if (error == UTP_INTERNAL_ERROR_OK && (slot->zero_rtt_early_data_size != 0u || slot->zero_rtt_early_fin)) {
                error = utp_frame_stream_header_encode(
                    payload + payload_length, payload_capacity - payload_length,
                    slot->zero_rtt_early_fin && early_data_length == slot->zero_rtt_early_data_size
                        ? UTP_STREAM_FLAG_FIN
                        : UTP_STREAM_FLAG_NONE,
                    0u, 0u, (uint16_t)early_data_length);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    memcpy(payload + payload_length + UTP_FRAME_STREAM_HEADER_SIZE, slot->zero_rtt_early_data,
                           early_data_length);
                    payload_length += UTP_FRAME_STREAM_HEADER_SIZE + early_data_length;
                }
            }
            if (error == UTP_INTERNAL_ERROR_OK && payload_length < payload_capacity) {
                payload[payload_length++] = UTP_FRAME_TYPE_PING;
                error = utp_context_pad_zero_rtt_payload(&slot->connection, payload, payload_capacity, token_length,
                                                         true, &payload_length);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_connection_queue_early_packet(&slot->connection, UTP_PACKET_TYPE_0RTT, payload,
                                                          payload_length, (uint16_t)token_length, true);
            }
        } else if (error == UTP_INTERNAL_ERROR_OK &&
                   (slot->zero_rtt_early_data_size != 0u || slot->zero_rtt_early_fin)) {
            const uint16_t target_size =
                utp_mtu_packet_size_from_mtu(slot->connection.mtu_discovery.mtu_min, slot->connection.peer.family);
            const size_t fixed_length = UTP_PACKET_HEADER_SIZE + payload_length;

            if (target_size < fixed_length + UTP_FRAME_STREAM_HEADER_SIZE) {
                error = UTP_INTERNAL_ERROR_LIMIT;
            } else {
                early_data_length = slot->zero_rtt_early_data_size;
                if (early_data_length > (size_t)target_size - fixed_length - UTP_FRAME_STREAM_HEADER_SIZE) {
                    early_data_length = (size_t)target_size - fixed_length - UTP_FRAME_STREAM_HEADER_SIZE;
                }
                error = utp_connection_reserve_zero_rtt_stream(&slot->connection, slot->zero_rtt_early_data,
                                                               slot->zero_rtt_early_data_size, early_data_length,
                                                               slot->zero_rtt_early_fin);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_frame_stream_header_encode(
                    payload + payload_length, payload_capacity - payload_length,
                    slot->zero_rtt_early_fin && early_data_length == slot->zero_rtt_early_data_size
                        ? UTP_STREAM_FLAG_FIN
                        : UTP_STREAM_FLAG_NONE,
                    0u, 0u, (uint16_t)early_data_length);
            }
            if (error == UTP_INTERNAL_ERROR_OK) {
                memcpy(payload + payload_length + UTP_FRAME_STREAM_HEADER_SIZE, slot->zero_rtt_early_data,
                       early_data_length);
                payload_length += UTP_FRAME_STREAM_HEADER_SIZE + early_data_length;
            }
        }
        if (error == UTP_INTERNAL_ERROR_OK && !slot->connection.zero_rtt_encrypted) {
            error = utp_context_pad_zero_rtt_payload(&slot->connection, payload, payload_capacity, 0u, false,
                                                     &payload_length);
        }
        if (error == UTP_INTERNAL_ERROR_OK && !slot->connection.zero_rtt_encrypted) {
            error = utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_0RTT, payload, payload_length, true);
        }
    } else if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_encode_version_frame(payload, payload_capacity, &payload_length);
        if (error == UTP_INTERNAL_ERROR_OK && slot->connection.crypto_configured) {
            error = utp_connection_encode_crypto(&slot->connection, payload + payload_length,
                                                 payload_capacity - payload_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                payload_length += UTP_FRAME_CRYPTO_SIZE;
            }
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_encode_transport_params(&slot->connection, payload + payload_length,
                                                           payload_capacity - payload_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                payload_length += UTP_FRAME_TRANSPORT_PARAMS_SIZE;
                error           = utp_connection_encode_ack_frequency(&slot->connection, payload + payload_length,
                                                                      payload_capacity - payload_length);
                if (error == UTP_INTERNAL_ERROR_OK) {
                    payload_length += UTP_FRAME_ACK_FREQUENCY_SIZE;
                }
            }
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error =
                utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_INITIAL, payload, payload_length, true);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_flush_connection(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connect_deadline_us = utp_context_connect_deadline(now_us, slot->connect_attempt.timeout_ms);
        utp_context_log_ids(context, UTP_LOG_LEVEL_INFO, "connection attempt started", slot->connection.local_cid,
                            slot->connection.peer_cid);
    }
    return error;
}

/** @brief 重试同一主动握手；保留 CID、临时密钥和发送账本，仅重新发送未确认的握手包。 */
static utp_internal_error_t utp_context_retry_connect_attempt(utp_context_t*                 context,
                                                              utp_context_connection_slot_t* slot, uint64_t now_us)
{
    utp_internal_error_t error = UTP_INTERNAL_ERROR_OK;

    if (context == NULL || slot == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN ||
        slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE) {
        // 0-RTT 重试必须带回 SESSION_TOKEN，不能复用普通发送账本中的裸包。
        const utp_address_t peer = slot->connection.peer;

        return utp_context_start_connect_attempt(context, slot, &peer, now_us);
    }
    if (utp_send_control_unacked_packet_count(&slot->connection.send_control) != 0u) {
        error = utp_connection_on_retransmission_timeout(&slot->connection, now_us);
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
    utp_context_log(context, UTP_LOG_LEVEL_WARNING, "connection attempt failed");
    utp_context_report_connect_error(context, status, message, &slot->connect_attempt);
    utp_context_release_connection_slot(context, slot);
}

static utp_internal_error_t utp_context_send_handshake_done(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    uint8_t                     payload[UTP_FRAME_HANDSHAKE_DONE_SIZE + UTP_FRAME_HANDSHAKE_DELAY_SIZE];
    utp_connection_t*           connection;
    utp_frame_handshake_done_t  done;
    utp_frame_handshake_delay_t delay;
    utp_internal_error_t        error;

    if (context == NULL || slot == NULL || !slot->used) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    connection                       = &slot->connection;
    done.ack_handshake_packet_number = connection->peer_handshake_packet_number;
    const uint64_t now_us            = utp_context_now_us();
    const uint64_t delay_us =
        now_us > connection->peer_handshake_received_us ? now_us - connection->peer_handshake_received_us : 0u;
    delay.delay_time_us = delay_us > UINT32_MAX ? UINT32_MAX : (uint32_t)delay_us;

    error = utp_frame_handshake_done_encode(payload, sizeof(payload), &done);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_frame_handshake_delay_encode(payload + UTP_FRAME_HANDSHAKE_DONE_SIZE,
                                                 sizeof(payload) - UTP_FRAME_HANDSHAKE_DONE_SIZE, &delay);
    }
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
    // ACK 保持 pending，由 flush 决定与 STREAM/control 合包还是单独发送。
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
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;
    uint64_t         deadline = 0u;

    if (!TAILQ_EMPTY(&context->terminal_error_slots)) {
        return now_us;
    }
    if (context->nat_probe.active) {
        utp_context_take_deadline(&deadline, context->nat_probe.phase_deadline_us);
        utp_context_take_deadline(&deadline, context->nat_probe.round_deadline_us);
    }
    if (context->nat_result_valid) {
        utp_context_take_deadline(&deadline, context->nat_result.expires_at_us);
    }
    // Context 只注册一个 timer，每次从全部连接和握手项中选取最近期限。
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        const utp_context_connection_slot_t* slot       = utp_context_connection_slot_from_node(node);
        const utp_connection_t*              connection = &slot->connection;

        if (connection->state == UTP_CONNECTION_STATE_CLOSING || connection->state == UTP_CONNECTION_STATE_DRAINING) {
            utp_context_take_deadline(&deadline, utp_connection_close_retransmission_deadline(connection));
            utp_context_take_deadline(&deadline, utp_connection_close_deadline(connection));
            if (slot->connect_pending && !utp_connection_is_connected(connection)) {
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
        if (slot->connect_pending) {
            utp_context_take_deadline(&deadline, slot->connect_deadline_us);
        }
        if (slot->zero_rtt_response_active) {
            utp_context_take_deadline(&deadline, slot->zero_rtt_response_deadline_us);
            utp_context_take_deadline(&deadline, slot->zero_rtt_expire_deadline_us);
        }
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        const utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);

        utp_context_take_deadline(&deadline, utp_pending_incoming_handshake_deadline(&slot->pending));
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
                                                              size_t wire_packet_length, void* user_data)
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
    error = utp_connection_on_plaintext_packet_in_received(replay->connection, packet_in, wire_packet_length,
                                                           replay->peer, replay->now_us);
    utp_packet_in_release(packet_in);
    return error;
}

static utp_internal_error_t utp_context_promote_pending(utp_context_t*              context,
                                                        utp_context_pending_slot_t* pending_slot, uint8_t* packet,
                                                        size_t packet_length, size_t wire_packet_length,
                                                        utp_packet_in_t* packet_in, const utp_address_t* peer)
{
    utp_context_connection_slot_t* slot;
    utp_internal_error_t           error;
    bool                           promotion_committed;

    promotion_committed = false;
    slot                = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_PASSIVE, pending_slot->pending.local_cid,
                                pending_slot->pending.peer_cid, peer, UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connection.local = pending_slot->pending.local;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_configure_connection(context, &slot->connection);
    }
    if (error == UTP_INTERNAL_ERROR_OK && pending_slot->pending.peer_transport_params_received) {
        error =
            utp_connection_apply_peer_transport_params(&slot->connection, &pending_slot->pending.peer_transport_params);
    }
    if (error == UTP_INTERNAL_ERROR_OK && pending_slot->pending.peer_ack_frequency_received) {
        const utp_frame_ack_frequency_t* frequency = &pending_slot->pending.peer_ack_frequency;

        utp_connection_apply_peer_ack_frequency(&slot->connection, frequency, 0u);
    }
    if (error == UTP_INTERNAL_ERROR_OK && pending_slot->pending.crypto_ready) {
        error = utp_connection_adopt_crypto(&slot->connection, pending_slot->pending.crypto_type,
                                            &pending_slot->pending.tx_aead, &pending_slot->pending.rx_aead);
        if (error == UTP_INTERNAL_ERROR_OK) {
            promotion_committed = true;
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_send_control_adopt_next_packet_number(&slot->connection.send_control,
                                                          pending_slot->pending.next_packet_number);
    }
    if (error == UTP_INTERNAL_ERROR_OK && pending_slot->pending.handshake_rtt_sample_us != 0u) {
        error = utp_rtt_stats_update(&slot->connection.send_control.rtt_stats,
                                     pending_slot->pending.handshake_rtt_sample_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_register_connection_slot(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK && !pending_slot->pending.crypto_ready) {
        promotion_committed = true;
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(context, slot);
        if (promotion_committed) {
            utp_context_release_pending_slot(context, pending_slot);
        }
        return error;
    }
    const uint64_t now_us = utp_context_now_us();
    if (packet_in != NULL) {
        error = utp_connection_on_plaintext_packet_in_received(&slot->connection, packet_in, wire_packet_length, peer,
                                                               now_us);
    } else {
        utp_packet_in_t* promoted_packet;

        error = packet_length > UINT16_MAX ? UTP_INTERNAL_ERROR_OVERFLOW
                                           : utp_packet_in_pool_acquire(&context->packet_in_pool, &promoted_packet);
        if (error == UTP_INTERNAL_ERROR_OK) {
            memcpy(promoted_packet->data, packet, packet_length);
            promoted_packet->length = (uint16_t)packet_length;
            error                   = utp_connection_on_plaintext_packet_in_received(&slot->connection, promoted_packet,
                                                                                     wire_packet_length, peer, now_us);
            utp_packet_in_release(promoted_packet);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_queue_ack_if_due(&slot->connection, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_context_replay_t replay = {
            context,
            &slot->connection,
            peer,
            now_us,
        };

        error = utp_pending_incoming_replay(&pending_slot->pending, utp_context_replay_pending_packet, &replay);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(context, slot);
        // 已开始处理 HandshakeDone，pending 已经不再是可重试的完整握手状态。
        utp_context_release_pending_slot(context, pending_slot);
        return error;
    }
    utp_context_release_pending_slot(context, pending_slot);
    utp_context_report_connected(context, slot);
    error = utp_context_queue_session_token(context, slot);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_context_flush_connection(context, slot);
}

static utp_internal_error_t utp_context_on_connection_packet(utp_context_t*                 context,
                                                             utp_context_connection_slot_t* slot,
                                                             const utp_packet_header_t* header, uint8_t* packet,
                                                             size_t packet_length, utp_packet_in_t* packet_in,
                                                             const utp_address_t* peer, const utp_address_t* local,
                                                             uint64_t* inout_now_us)
{
    uint64_t             now_us = *inout_now_us;
    utp_internal_error_t error;

    if (!slot->connect_pending && slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE &&
        (slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN ||
         slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE) &&
        header->type == UTP_PACKET_TYPE_HANDSHAKE) {
        // 完成后的迟到响应属于已退休的 early epoch，不能交给 1-RTT 明文握手解析。
        return UTP_INTERNAL_ERROR_OK;
    }
    if (slot->connect_pending && slot->connection.zero_rtt_encrypted &&
        slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE && header->type == UTP_PACKET_TYPE_HANDSHAKE) {
        size_t decrypted_length = packet_length;

        error = utp_connection_on_zero_rtt_handshake(&slot->connection, packet_in == NULL ? packet : packet_in->data,
                                                     &decrypted_length, peer, now_us);
        if (error == UTP_INTERNAL_ERROR_OK && packet_in != NULL) {
            packet_in->length = (uint16_t)decrypted_length;
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error == UTP_INTERNAL_ERROR_AUTH || error == UTP_INTERNAL_ERROR_CRYPTO ? UTP_INTERNAL_ERROR_OK
                                                                                          : error;
        }
        if (local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
            slot->connection.local = *local;
        }
        error = utp_connection_queue_ack(&slot->connection, now_us);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        error = utp_context_complete_connected_side_effects(context, slot);
        if (error == UTP_INTERNAL_ERROR_OK) {
            utp_crypto_secure_clear(slot->zero_rtt_resumption_psk, sizeof(slot->zero_rtt_resumption_psk));
            utp_crypto_aead_cleanup(&slot->connection.early_tx_aead);
            utp_crypto_aead_cleanup(&slot->connection.early_rx_aead);
            slot->connection.zero_rtt_encrypted = false;
        }
        return error == UTP_INTERNAL_ERROR_OK ? utp_context_flush_connection(context, slot) : error;
    }
    utp_connection_set_received_local(&slot->connection, local);
    if (packet_in != NULL) {
        error = utp_connection_on_packet_in_received(&slot->connection, packet_in, peer, now_us);
    } else {
        error = utp_connection_on_packet_received(&slot->connection, packet, packet_length, peer, now_us);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        if (error == UTP_INTERNAL_ERROR_AUTH || error == UTP_INTERNAL_ERROR_CRYPTO) {
            return UTP_INTERNAL_ERROR_OK;
        }
        if (utp_context_is_peer_protocol_error(error)) {
            return utp_context_close_on_peer_protocol_error(context, slot, error);
        }
        return error;
    }
    if (local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED &&
        utp_address_equal(peer, &slot->connection.peer)) {
        slot->connection.local = *local;
    }
    if (slot->connection.role == UTP_CONNECTION_ROLE_PASSIVE && slot->zero_rtt_response_active &&
        (header->type == UTP_PACKET_TYPE_CTRL || header->type == UTP_PACKET_TYPE_CONNECTION_CLOSE)) {
        // 首个有效 1-RTT 包证明客户端已收到响应，立即退休所有未发出的响应副本。
        error = utp_connection_retire_handshake_flight(&slot->connection, now_us);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        slot->zero_rtt_response_active      = false;
        slot->zero_rtt_response_queued      = false;
        slot->zero_rtt_response_deadline_us = 0u;
        slot->zero_rtt_expire_deadline_us   = 0u;
        if (slot->connection.zero_rtt_encrypted) {
            utp_crypto_aead_cleanup(&slot->connection.early_tx_aead);
            utp_crypto_aead_cleanup(&slot->connection.early_rx_aead);
            slot->connection.zero_rtt_encrypted = false;
        }
    }
    if (slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE && header->type == UTP_PACKET_TYPE_HANDSHAKE &&
        slot->connection.peer_handshake_packet_number == header->packet_number &&
        slot->connect_attempt.type != UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN &&
        slot->connect_attempt.type != UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE) {
        error = utp_context_send_handshake_done(context, slot);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if (slot->connect_pending && slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE &&
        (slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN ||
         slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE) &&
        header->type == UTP_PACKET_TYPE_HANDSHAKE) {
        error = utp_connection_queue_ack(&slot->connection, now_us);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    error = utp_context_queue_ack_if_due(&slot->connection, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    utp_context_report_connected(context, slot);
    error = utp_context_queue_session_token(context, slot);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (!slot->connect_pending && slot->connection.peer_close_received && !slot->connection.local_close_started) {
        const utp_status_t status = slot->connection.peer_close_error_code == 0u ? UTP_STATUS_OK : UTP_STATUS_CLOSED;

        utp_context_report_connection_error(context, slot, status, slot->connection.peer_close_error_code,
                                            slot->connection.peer_close_reason,
                                            slot->connection.peer_close_reason_length, true);
        slot->connection.peer_close_reason        = NULL;
        slot->connection.peer_close_reason_length = 0u;
    }
    if (slot->connection.last_public_flush_us > now_us) {
        now_us = slot->connection.last_public_flush_us;
    }
    *inout_now_us = now_us;
    return utp_context_flush_connection_at(context, slot, now_us);
}

static utp_internal_error_t utp_context_on_pending_packet(utp_context_t* context, utp_context_pending_slot_t* slot,
                                                          uint8_t* packet, size_t packet_length,
                                                          utp_packet_in_t* packet_in, const utp_address_t* peer,
                                                          const utp_address_t* local)
{
    const size_t                  wire_packet_length = packet_length;
    utp_pending_incoming_result_t result;
    utp_internal_error_t          error;

    if (!slot->pending.accepted) {
        return UTP_INTERNAL_ERROR_OK;
    }
    if (slot->pending.crypto_ready) {
        error = utp_pending_incoming_decrypt_packet(&slot->pending, packet, &packet_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error == UTP_INTERNAL_ERROR_AUTH || error == UTP_INTERNAL_ERROR_CRYPTO ? UTP_INTERNAL_ERROR_OK
                                                                                          : error;
        }
        if (packet_in != NULL) {
            packet_in->length = (uint16_t)packet_length;
        }
    }
    error = utp_pending_incoming_on_packet(&slot->pending, packet, packet_length, wire_packet_length, peer,
                                           utp_context_now_us(), &result);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
        utp_pending_incoming_set_local(&slot->pending, local);
    }
    if (result == UTP_PENDING_INCOMING_PROMOTE) {
        error = utp_context_promote_pending(context, slot, packet, packet_length, wire_packet_length, packet_in, peer);
    }
    return error;
}

static utp_internal_error_t utp_context_on_initial_packet(utp_context_t* context, const utp_packet_view_t* view,
                                                          const utp_address_t* peer, const utp_address_t* local)
{
    utp_context_pending_slot_t* slot;
    utp_frame_crypto_t          peer_crypto;
    uint32_t                    local_cid;
    size_t                      frame_offset;
    bool                        has_crypto;
    utp_internal_error_t        error;

    if (view->header.scid == 0u || view->header.dcid != 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    slot = utp_context_find_pending_by_peer(context, view->header.scid, peer);
    if (slot != NULL) {
        error = utp_pending_incoming_record_initial(&slot->pending, view->header.packet_number, utp_context_now_us());
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
            utp_pending_incoming_set_local(&slot->pending, local);
        }
        if (slot->pending.handshake_sent) {
            uint64_t now_us = utp_context_now_us();

            if (slot->pending.handshake_retransmission_count > slot->pending.handshake_max_retries) {
                utp_context_log_ids(context, UTP_LOG_LEVEL_WARNING, "incoming handshake response retries exhausted",
                                    slot->pending.local_cid, slot->pending.peer_cid);
                utp_context_release_pending_slot(context, slot);
                return UTP_INTERNAL_ERROR_OK;
            }
            error = utp_context_send_or_defer_pending_handshake(context, slot, now_us);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_context_refresh_timer(context, now_us);
            }
            return error;
        }
        return UTP_INTERNAL_ERROR_OK;
    }
    // 服务端 HANDSHAKE 丢失时，客户端会重传 Initial；已晋升的同一来源地址和 SCID 不得再次交给应用 accept。
    if (utp_context_find_passive_connection_by_peer(context, peer, view->header.scid) != NULL) {
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
        if (error == UTP_INTERNAL_ERROR_OK && local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
            utp_pending_incoming_set_local(&slot->pending, local);
        }
    }
    frame_offset = 0u;
    has_crypto   = false;
    while (error == UTP_INTERNAL_ERROR_OK && frame_offset < view->payload_length) {
        const uint8_t* frame;
        uint8_t        frame_type;
        size_t         frame_length;

        error = utp_packet_view_next_frame(view, &frame_offset, &frame_type, &frame, &frame_length);
        if (error == UTP_INTERNAL_ERROR_OK && frame_type == UTP_FRAME_TYPE_CRYPTO) {
            if (has_crypto) {
                error = UTP_INTERNAL_ERROR_PROTOCOL;
            } else {
                error      = utp_frame_crypto_decode(&peer_crypto, frame, frame_length);
                has_crypto = error == UTP_INTERNAL_ERROR_OK;
            }
        } else if (error == UTP_INTERNAL_ERROR_OK && frame_type == UTP_FRAME_TYPE_TRANSPORT_PARAMS) {
            if (slot->pending.peer_transport_params_received) {
                error = UTP_INTERNAL_ERROR_PROTOCOL;
            } else {
                error = utp_frame_transport_params_decode(&slot->pending.peer_transport_params, frame, frame_length);
                slot->pending.peer_transport_params_received = error == UTP_INTERNAL_ERROR_OK;
            }
        } else if (error == UTP_INTERNAL_ERROR_OK && frame_type == UTP_FRAME_TYPE_ACK_FREQUENCY) {
            if (slot->pending.peer_ack_frequency_received) {
                error = UTP_INTERNAL_ERROR_PROTOCOL;
            } else {
                error = utp_frame_ack_frequency_decode(&slot->pending.peer_ack_frequency, frame, frame_length);
                slot->pending.peer_ack_frequency_received = error == UTP_INTERNAL_ERROR_OK;
            }
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK && has_crypto) {
        error = utp_pending_incoming_configure_crypto(&slot->pending, &peer_crypto);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_pending_incoming_record_initial(&slot->pending, view->header.packet_number, utp_context_now_us());
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        {
            uint16_t handshake_timeout_ms = context->handshake_timeout_ms;

            if (slot->pending.peer_transport_params_received &&
                (slot->pending.peer_transport_params.flags & UTP_TRANSPORT_PARAMS_FLAG_HANDSHAKE_TIMEOUT) != 0u &&
                slot->pending.peer_transport_params.handshake_timeout_ms != 0u &&
                slot->pending.peer_transport_params.handshake_timeout_ms < handshake_timeout_ms) {
                handshake_timeout_ms = slot->pending.peer_transport_params.handshake_timeout_ms;
            }
            error = utp_pending_incoming_set_handshake_policy(&slot->pending, handshake_timeout_ms,
                                                              context->handshake_max_retries);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_register_pending_slot(context, slot);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_pending_slot(context, slot);
        return error;
    }
    utp_new_connection_info_t info = {0};

    slot->queued = true;
    utp_context_endpoint_from_address(&info.remote, peer);
    info.local_cid = slot->pending.local_cid;
    info.peer_cid  = slot->pending.peer_cid;
    info.encryption =
        has_crypto ? utp_context_encryption_from_crypto_type(peer_crypto.crypto_type) : UTP_ENCRYPTION_NONE;
    context->callback_accept_pending   = slot;
    context->callback_accept_requested = false;

    bool accepted =
        context->on_new_connection == NULL || context->on_new_connection(&info, context->on_new_connection_user_data);
    context->callback_accept_pending = NULL;
    if (context->on_new_connection != NULL) {
        accepted = accepted && context->callback_accept_requested;
    }
    context->callback_accept_requested = false;
    if (!accepted) {
        utp_context_log_ids(context, UTP_LOG_LEVEL_INFO, "incoming connection rejected", slot->pending.local_cid,
                            slot->pending.peer_cid);
        utp_context_send_pending_close(context, &slot->pending, (uint16_t)(-UTP_STATUS_CANCELLED));
        utp_context_release_pending_slot(context, slot);
    } else if (context->on_new_connection != NULL) {
        error = utp_context_accept_pending_slot(context, slot);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    } else {
        utp_context_log_ids(context, UTP_LOG_LEVEL_INFO, "incoming connection queued", slot->pending.local_cid,
                            slot->pending.peer_cid);
    }
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 校验客户端 early 密文的固定帧序，并返回可交给普通流处理的尾部起点。 */
static utp_internal_error_t utp_context_validate_zero_rtt_client_frames(
    const uint8_t* payload, size_t payload_length, uint8_t crypto_type,
    uint8_t client_public_key[UTP_CRYPTO_X25519_KEY_SIZE], size_t* post_offset)
{
    static const uint8_t required[] = {
        UTP_FRAME_TYPE_CRYPTO,
        UTP_FRAME_TYPE_VERSION,
        UTP_FRAME_TYPE_TRANSPORT_PARAMS,
        UTP_FRAME_TYPE_ACK_FREQUENCY,
    };
    size_t offset       = 0u;
    bool   stream_seen  = false;
    bool   ping_seen    = false;
    bool   padding_seen = false;

    if (payload == NULL || client_public_key == NULL || post_offset == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (size_t index = 0u; index < sizeof(required); ++index) {
        uint8_t              type;
        size_t               length;
        utp_internal_error_t error = utp_frame_measure(payload + offset, payload_length - offset, &type, &length);

        if (error != UTP_INTERNAL_ERROR_OK || type != required[index]) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (type == UTP_FRAME_TYPE_CRYPTO) {
            utp_frame_crypto_t crypto;

            error = utp_frame_crypto_decode(&crypto, payload + offset, length);
            if (error != UTP_INTERNAL_ERROR_OK || crypto.crypto_type != crypto_type) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            memcpy(client_public_key, crypto.ephemeral_public_key, UTP_CRYPTO_X25519_KEY_SIZE);
        } else if (type == UTP_FRAME_TYPE_VERSION) {
            utp_frame_version_t version;

            error = utp_frame_version_decode(&version, payload + offset, length);
            if (error != UTP_INTERNAL_ERROR_OK || version.version != UTP_PROTOCOL_VERSION) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
        }
        offset += length;
    }
    *post_offset = offset;
    while (offset < payload_length) {
        uint8_t              type;
        size_t               length;
        utp_internal_error_t error = utp_frame_measure(payload + offset, payload_length - offset, &type, &length);

        if (error != UTP_INTERNAL_ERROR_OK || padding_seen) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (type == UTP_FRAME_TYPE_STREAM) {
            utp_frame_stream_t stream;

            error = utp_frame_stream_decode(&stream, payload + offset, length);
            if (error != UTP_INTERNAL_ERROR_OK || stream_seen || stream.stream_id != 0u) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            stream_seen = true;
        } else if (type == UTP_FRAME_TYPE_PING) {
            if (ping_seen) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            ping_seen = true;
        } else if (type == UTP_FRAME_TYPE_PADDING) {
            padding_seen = true;
        } else {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        offset += length;
    }
    return UTP_INTERNAL_ERROR_OK;
}

/** @brief 按最新 0-RTT 请求重新构造 HANDSHAKE；每次调用都会分配新包号并重新执行 AEAD。 */
static utp_internal_error_t utp_context_queue_zero_rtt_response(utp_context_t*                 context,
                                                                utp_context_connection_slot_t* slot, uint64_t now_us)
{
    uint8_t*             response;
    size_t               response_capacity;
    size_t               response_length = 0u;
    bool                 encrypted;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || !slot->used || now_us == 0u || !slot->zero_rtt_response_active ||
        slot->zero_rtt_response_queued || slot->zero_rtt_request_packet_number == 0u ||
        slot->zero_rtt_request_received_us == 0u) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    response          = context->encrypt_send_buffer;
    response_capacity = sizeof(context->encrypt_send_buffer);
    encrypted         = slot->connection.zero_rtt_encrypted;
    error             = slot->zero_rtt_response_sent ? UTP_INTERNAL_ERROR_OK
                                                     : utp_connection_begin_zero_rtt_response(&slot->connection);
    if (error == UTP_INTERNAL_ERROR_OK && encrypted) {
        error           = utp_connection_encode_crypto(&slot->connection, response, response_capacity);
        response_length = UTP_FRAME_CRYPTO_SIZE;
    } else if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_encode_version_frame(response, response_capacity, &response_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK && encrypted) {
        error = utp_context_append_zero_rtt_parameters(response, response_capacity, &response_length, false, 0u);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_append_handshake_feedback(response, response_capacity, &response_length,
                                                      slot->zero_rtt_request_packet_number,
                                                      slot->zero_rtt_request_received_us, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_pad_zero_rtt_payload(&slot->connection, response, response_capacity, 0u, encrypted,
                                                 &response_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_queue_zero_rtt_response(&slot->connection, response, response_length, encrypted);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->zero_rtt_response_queued      = true;
        slot->zero_rtt_response_deadline_us = 0u;
    }
    return error;
}

/** @brief 认证加密 0-RTT 后创建待接受连接，并仅在 early 响应写成功后投递其尾部帧。 */
static utp_internal_error_t utp_context_on_encrypted_zero_rtt_packet(
    utp_context_t* context, utp_packet_in_t* packet_in, const utp_address_t* peer, const utp_packet_view_t* view,
    const utp_frame_session_token_t* session_token, const uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE],
    uint8_t encryption_mode, const utp_address_t* local)
{
    utp_crypto_aead_t              early_rx = {0};
    utp_context_connection_slot_t* slot;
    utp_packet_header_t            header;
    uint8_t                        client_public_key[UTP_CRYPTO_X25519_KEY_SIZE];
    uint8_t                        crypto_type;
    size_t                         token_length;
    size_t                         plaintext_length;
    size_t                         post_offset;
    uint64_t                       received_at_us;
    uint64_t                       now_us;
    utp_internal_error_t           error;

    if (context == NULL || packet_in == NULL || peer == NULL || view == NULL || session_token == NULL ||
        resumption_psk == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_128 &&
        encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    crypto_type  = encryption_mode == UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256 ? UTP_CRYPTO_TYPE_AES_GCM_256
                                                                             : UTP_CRYPTO_TYPE_AES_GCM_128;
    token_length = UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + (size_t)session_token->payload_length;
    if (token_length >= view->payload_length || view->payload_length - token_length < UTP_CRYPTO_AEAD_TAG_SIZE) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    error =
        utp_crypto_derive_early_aead(&early_rx, resumption_psk, session_token->payload,
                                     session_token->payload + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE, crypto_type, true);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_aead_open(
            &early_rx, view->header.packet_number, packet_in->data + UTP_PACKET_HEADER_SIZE + token_length,
            view->payload_length - token_length, packet_in->data, UTP_PACKET_HEADER_SIZE + token_length,
            packet_in->data + UTP_PACKET_HEADER_SIZE + token_length, view->payload_length - token_length,
            &plaintext_length);
    }
    utp_crypto_aead_cleanup(&early_rx);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    received_at_us = utp_context_now_us();
    error = utp_context_validate_zero_rtt_client_frames(packet_in->data + UTP_PACKET_HEADER_SIZE + token_length,
                                                        plaintext_length, crypto_type, client_public_key, &post_offset);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (!utp_context_remember_zero_rtt_replay(context, session_token->payload + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE,
                                              session_token->payload, utp_context_now_seconds(),
                                              session_token->expires_at_seconds)) {
        slot = utp_context_find_zero_rtt_response(context, peer, session_token->payload);
        if (slot == NULL) {
            return UTP_INTERNAL_ERROR_AUTH;
        }
        if (local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
            slot->connection.local = *local;
        }
        if ((uint64_t)packet_in->length > UINT64_MAX - slot->zero_rtt_amplification_rx_bytes) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        slot->zero_rtt_amplification_rx_bytes += (uint64_t)packet_in->length;
        if (view->header.packet_number > slot->zero_rtt_request_packet_number) {
            slot->zero_rtt_request_packet_number = view->header.packet_number;
            slot->zero_rtt_request_received_us   = received_at_us;
        }
        if (!slot->zero_rtt_response_queued &&
            (!slot->zero_rtt_response_sent || slot->zero_rtt_response_retries < context->handshake_max_retries)) {
            error = utp_context_queue_zero_rtt_response(context, slot, utp_context_now_us());
        } else {
            error = UTP_INTERNAL_ERROR_OK;
        }
        return error == UTP_INTERNAL_ERROR_OK ? utp_context_flush_connection(context, slot) : error;
    }
    uint32_t local_cid;

    error = utp_context_alloc_cid(context, &local_cid);
    slot  = error == UTP_INTERNAL_ERROR_OK ? utp_context_alloc_connection_slot(context) : NULL;
    if (error != UTP_INTERNAL_ERROR_OK || slot == NULL) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_NOMEM : error;
    }
    error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_PASSIVE, local_cid, view->header.scid, peer,
                                UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    if (error == UTP_INTERNAL_ERROR_OK && local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
        slot->connection.local = *local;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        memcpy(slot->zero_rtt_session_token, session_token->payload, UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE);
        slot->zero_rtt_expires_at_seconds = session_token->expires_at_seconds;
        slot->zero_rtt_encryption_mode    = encryption_mode;
        error                             = utp_context_configure_connection(context, &slot->connection);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_configure_zero_rtt_crypto(&slot->connection, resumption_psk, session_token->payload,
                                                         session_token->payload + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE,
                                                         crypto_type);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_complete_zero_rtt_crypto(&slot->connection, client_public_key);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_register_connection_slot(context, slot);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(context, slot);
        return error;
    }
    utp_new_connection_info_t info = {0};

    utp_context_endpoint_from_address(&info.remote, peer);
    info.local_cid                     = local_cid;
    info.peer_cid                      = view->header.scid;
    info.encryption                    = utp_context_encryption_from_crypto_type(crypto_type);
    slot->zero_rtt_awaiting_accept     = context->on_new_connection != NULL;
    slot->zero_rtt_accepted            = context->on_new_connection == NULL;
    context->callback_accept_zero_rtt  = slot;
    context->callback_accept_requested = false;
    bool accepted =
        context->on_new_connection == NULL || context->on_new_connection(&info, context->on_new_connection_user_data);
    context->callback_accept_zero_rtt = NULL;
    if (context->callback_accept_requested) {
        slot->zero_rtt_accepted = true;
    }
    context->callback_accept_requested = false;
    slot->zero_rtt_awaiting_accept     = false;
    if (!accepted || !slot->zero_rtt_accepted) {
        utp_context_release_connection_slot(context, slot);
        return UTP_INTERNAL_ERROR_OK;
    }
    header                = view->header;
    header.payload_length = (uint16_t)(plaintext_length - post_offset);
    memmove(packet_in->data + UTP_PACKET_HEADER_SIZE,
            packet_in->data + UTP_PACKET_HEADER_SIZE + token_length + post_offset, header.payload_length);
    error = utp_proto_encode_header(packet_in->data, packet_in->capacity, &header);
    if (error == UTP_INTERNAL_ERROR_OK) {
        packet_in->length = (uint16_t)(UTP_PACKET_HEADER_SIZE + header.payload_length);
        if (!utp_packet_in_ref(packet_in)) {
            error = UTP_INTERNAL_ERROR_LIMIT;
        }
    }
    now_us = utp_context_now_us();
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->zero_rtt_early_packet           = packet_in;
        slot->zero_rtt_early_wire_size        = UTP_PACKET_HEADER_SIZE + view->payload_length;
        slot->zero_rtt_request_packet_number  = view->header.packet_number;
        slot->zero_rtt_request_received_us    = received_at_us;
        slot->zero_rtt_response_active        = true;
        slot->zero_rtt_amplification_rx_bytes = (uint64_t)slot->zero_rtt_early_wire_size;
        slot->zero_rtt_expire_deadline_us     = utp_context_zero_rtt_expire_deadline(context, now_us);
        error                                 = utp_context_queue_zero_rtt_response(context, slot, now_us);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_flush_connection(context, slot);
    }
    if (error != UTP_INTERNAL_ERROR_OK && slot->used && !slot->terminal_error_queued) {
        utp_context_release_connection_slot(context, slot);
    }
    return error;
}

/** @brief 验证并接收首个明文 0-RTT 包，恢复凭证本身始终经过 Context 根密钥保护。 */
static utp_internal_error_t utp_context_on_zero_rtt_packet(utp_context_t* context, utp_packet_in_t* packet_in,
                                                           const utp_address_t* peer, const utp_address_t* local)
{
    uint16_t minimum_packet_size;

    if (context == NULL || packet_in == NULL || peer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    minimum_packet_size = utp_mtu_packet_size_from_mtu(context->mtu_config.mtu_min, peer->family);
    utp_packet_view_t    view;
    utp_internal_error_t error = utp_proto_decode_header(&view.header, packet_in->data, packet_in->length);

    if (error != UTP_INTERNAL_ERROR_OK || view.header.type != UTP_PACKET_TYPE_0RTT || view.header.dcid != 0u ||
        view.header.scid == 0u || packet_in->length != UTP_PACKET_HEADER_SIZE + view.header.payload_length ||
        packet_in->length < minimum_packet_size) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    view.payload        = packet_in->data + UTP_PACKET_HEADER_SIZE;
    view.payload_length = view.header.payload_length;
    view.frame_types    = 0u;
    utp_frame_session_token_t session_token;
    size_t                    token_length;

    error = utp_frame_session_token_decode(&session_token, view.payload, view.payload_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    token_length = UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + (size_t)session_token.payload_length;
    if (token_length > view.payload_length) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    const uint64_t now_seconds = utp_context_now_seconds();
    if (session_token.payload_length != UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE ||
        session_token.expires_at_seconds < now_seconds) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    if (!context->resumption_keys_ready) {
        return UTP_INTERNAL_ERROR_AUTH;
    }
    uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE];
    uint8_t encryption_mode;

    error = utp_crypto_server_info_open(context->resumption_keys.ticket_seal_key, session_token.expires_at_seconds,
                                        session_token.payload + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE, resumption_psk,
                                        &encryption_mode);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        return UTP_INTERNAL_ERROR_AUTH;
    }
    if (encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_NONE) {
        error = utp_context_on_encrypted_zero_rtt_packet(context, packet_in, peer, &view, &session_token,
                                                         resumption_psk, encryption_mode, local);
        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        return error;
    }
    size_t offset       = token_length;
    bool   stream_seen  = false;
    bool   ping_seen    = false;
    bool   padding_seen = false;

    while (offset < view.payload_length) {
        const uint8_t* frame;
        uint8_t        frame_type;
        size_t         frame_length;

        error = utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (padding_seen) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (frame_type == UTP_FRAME_TYPE_STREAM) {
            utp_frame_stream_t stream;

            error = utp_frame_stream_decode(&stream, frame, frame_length);
            if (error != UTP_INTERNAL_ERROR_OK || stream_seen || stream.stream_id != 0u) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            stream_seen = true;
        } else if (frame_type == UTP_FRAME_TYPE_PING) {
            if (ping_seen) {
                return UTP_INTERNAL_ERROR_PROTOCOL;
            }
            ping_seen = true;
        } else if (frame_type == UTP_FRAME_TYPE_PADDING) {
            padding_seen = true;
        } else {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
    }
    const uint64_t received_at_us = utp_context_now_us();

    if (!utp_context_remember_zero_rtt_replay(context, session_token.payload + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE,
                                              session_token.payload, now_seconds, session_token.expires_at_seconds)) {
        utp_context_connection_slot_t* duplicate =
            utp_context_find_zero_rtt_response(context, peer, session_token.payload);

        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        if (duplicate == NULL) {
            return UTP_INTERNAL_ERROR_AUTH;
        }
        if (local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
            duplicate->connection.local = *local;
        }
        if ((uint64_t)packet_in->length > UINT64_MAX - duplicate->zero_rtt_amplification_rx_bytes) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        duplicate->zero_rtt_amplification_rx_bytes += (uint64_t)packet_in->length;
        if (view.header.packet_number > duplicate->zero_rtt_request_packet_number) {
            duplicate->zero_rtt_request_packet_number = view.header.packet_number;
            duplicate->zero_rtt_request_received_us   = received_at_us;
        }
        if (!duplicate->zero_rtt_response_queued &&
            (!duplicate->zero_rtt_response_sent ||
             duplicate->zero_rtt_response_retries < context->handshake_max_retries)) {
            error = utp_context_queue_zero_rtt_response(context, duplicate, utp_context_now_us());
        } else {
            error = UTP_INTERNAL_ERROR_OK;
        }
        return error == UTP_INTERNAL_ERROR_OK ? utp_context_flush_connection(context, duplicate) : error;
    }
    uint32_t local_cid;

    error = utp_context_alloc_cid(context, &local_cid);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        return error;
    }
    utp_new_connection_info_t info = {0};

    info.remote     = (utp_endpoint_t){peer->family, peer->port, peer->scope_id, {0u}};
    info.local_cid  = local_cid;
    info.peer_cid   = view.header.scid;
    info.encryption = UTP_ENCRYPTION_NONE;
    for (size_t index = 0u; index < sizeof(info.remote.address); ++index) {
        info.remote.address[index] = peer->address[index];
    }
    utp_context_connection_slot_t* slot = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    memcpy(slot->zero_rtt_resumption_psk, resumption_psk, sizeof(slot->zero_rtt_resumption_psk));
    memcpy(slot->zero_rtt_session_token, session_token.payload, UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE);
    utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
    slot->zero_rtt_expires_at_seconds = session_token.expires_at_seconds;
    slot->zero_rtt_encryption_mode    = encryption_mode;
    error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_PASSIVE, local_cid, view.header.scid, peer,
                                UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    if (error == UTP_INTERNAL_ERROR_OK && local != NULL && local->family != UTP_ADDRESS_FAMILY_UNSPECIFIED) {
        slot->connection.local = *local;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_configure_connection(context, &slot->connection);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_register_connection_slot(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->zero_rtt_awaiting_accept     = context->on_new_connection != NULL;
        slot->zero_rtt_accepted            = context->on_new_connection == NULL;
        context->callback_accept_zero_rtt  = slot;
        context->callback_accept_requested = false;

        const bool accepted = context->on_new_connection == NULL ||
                              context->on_new_connection(&info, context->on_new_connection_user_data);
        context->callback_accept_zero_rtt = NULL;
        if (context->callback_accept_requested) {
            slot->zero_rtt_accepted = true;
        }
        context->callback_accept_requested = false;
        slot->zero_rtt_awaiting_accept     = false;
        if (!accepted || !slot->zero_rtt_accepted) {
            utp_context_release_connection_slot(context, slot);
            return UTP_INTERNAL_ERROR_OK;
        }
    }
    const uint64_t now_us = utp_context_now_us();
    if (error == UTP_INTERNAL_ERROR_OK && !utp_packet_in_ref(packet_in)) {
        error = UTP_INTERNAL_ERROR_LIMIT;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->zero_rtt_early_packet           = packet_in;
        slot->zero_rtt_early_wire_size        = packet_in->length;
        slot->zero_rtt_request_packet_number  = view.header.packet_number;
        slot->zero_rtt_request_received_us    = received_at_us;
        slot->zero_rtt_response_active        = true;
        slot->zero_rtt_amplification_rx_bytes = (uint64_t)packet_in->length;
        slot->zero_rtt_expire_deadline_us     = utp_context_zero_rtt_expire_deadline(context, now_us);
        error                                 = utp_context_queue_zero_rtt_response(context, slot, now_us);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(context, slot);
        return error;
    }
    error = utp_context_flush_connection(context, slot);
    if (error != UTP_INTERNAL_ERROR_OK && slot->used && !slot->terminal_error_queued) {
        utp_context_release_connection_slot(context, slot);
    }
    return error;
}

static utp_internal_error_t utp_context_dispatch_packet(utp_context_t* context, uint8_t* packet, size_t packet_length,
                                                        utp_packet_in_t* packet_in, const utp_address_t* peer,
                                                        const utp_address_t* local, uint64_t now_us)
{
    utp_packet_header_t  header;
    utp_internal_error_t error = utp_proto_decode_header(&header, packet, packet_length);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    if (header.type == UTP_PACKET_TYPE_NAT_PROBE) {
        if (packet_length != UTP_PACKET_HEADER_SIZE + header.payload_length) {
            return UTP_INTERNAL_ERROR_OK;
        }
        return utp_context_on_nat_probe_packet(context, &header, packet + UTP_PACKET_HEADER_SIZE, header.payload_length,
                                               peer, local, now_us);
    }
    if (header.type == UTP_PACKET_TYPE_RENDEZVOUS) {
        // 半连接报文必须在 CID 查表前分流，后续由 NTRS 模块处理。
        return UTP_INTERNAL_ERROR_OK;
    }
    if (packet_length != UTP_PACKET_HEADER_SIZE + header.payload_length || header.packet_number == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    utp_context_connection_slot_t* connection_slot = utp_context_find_connection_slot(context, header.dcid);
    if (connection_slot != NULL) {
        return utp_context_on_connection_packet(context, connection_slot, &header, packet, packet_length, packet_in,
                                                peer, local, &now_us);
    }
    utp_context_pending_slot_t* pending_slot = utp_context_find_pending_slot(context, header.dcid);
    if (pending_slot != NULL) {
        return utp_context_on_pending_packet(context, pending_slot, packet, packet_length, packet_in, peer, local);
    }
    if (header.dcid == 0u && header.type == UTP_PACKET_TYPE_INITIAL) {
        utp_packet_view_t view;

        error = utp_packet_view_decode(&view, packet, packet_length);
        return error == UTP_INTERNAL_ERROR_OK ? utp_context_on_initial_packet(context, &view, peer, local) : error;
    }
    if (header.dcid == 0u && header.type == UTP_PACKET_TYPE_0RTT && packet_in != NULL) {
        return utp_context_on_zero_rtt_packet(context, packet_in, peer, local);
    }
    return UTP_INTERNAL_ERROR_OK;
}

static void utp_context_on_udp_readable(uint32_t events, void* user_data)
{
    utp_context_t* context = user_data;

    if ((events & UTP_EVENT_READABLE) == 0u || context == NULL) {
        return;
    }
#if defined(UTP_HAVE_RECVMMSG)
    for (;;) {
        utp_packet_in_t*          packet_ins[UTP_UDP_SOCKET_BATCH_SIZE] = {NULL};
        utp_udp_receive_message_t messages[UTP_UDP_SOCKET_BATCH_SIZE];
        size_t                    received_count = 0u;
        utp_internal_error_t      error;

        error = utp_packet_in_pool_acquire_many(&context->packet_in_pool, packet_ins, UTP_UDP_SOCKET_BATCH_SIZE);
        if (error == UTP_INTERNAL_ERROR_OK) {
            for (size_t index = 0u; index < UTP_UDP_SOCKET_BATCH_SIZE; ++index) {
                messages[index].data     = packet_ins[index]->data;
                messages[index].capacity = packet_ins[index]->capacity;
            }
            error = utp_udp_socket_receive_messages(&context->udp_socket, messages, UTP_UDP_SOCKET_BATCH_SIZE,
                                                    &received_count);
        }
        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            for (size_t index = 0u; index < UTP_UDP_SOCKET_BATCH_SIZE; ++index) {
                utp_packet_in_release(packet_ins[index]);
            }
            return;
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            for (size_t index = 0u; index < UTP_UDP_SOCKET_BATCH_SIZE; ++index) {
                utp_packet_in_release(packet_ins[index]);
            }
            utp_internal_log_error(&context->logger, &context->tag, error, "udp batch receive failed");
            return;
        }
        if (received_count == 0u) {
            for (size_t index = 0u; index < UTP_UDP_SOCKET_BATCH_SIZE; ++index) {
                utp_packet_in_release(packet_ins[index]);
            }
            return;
        }
        for (size_t index = 0u; index < received_count; ++index) {
            uint64_t now_us = utp_context_now_us();

            if (messages[index].error == UTP_INTERNAL_ERROR_OK) {
                packet_ins[index]->length = (uint16_t)messages[index].received_length;
                error = utp_context_dispatch_packet(context, packet_ins[index]->data, packet_ins[index]->length,
                                                    packet_ins[index], &messages[index].peer, &messages[index].local,
                                                    now_us);
            } else {
                error = messages[index].error;
            }
            utp_packet_in_release(packet_ins[index]);
            utp_context_drain_terminal_errors(context);
            if (error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, error, "udp packet handling failed");
            }
            error = utp_context_refresh_timer(context, now_us);
            if (error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, error, "context timer refresh failed");
            }
        }
        for (size_t index = received_count; index < UTP_UDP_SOCKET_BATCH_SIZE; ++index) {
            utp_packet_in_release(packet_ins[index]);
        }
    }
#else
    for (;;) {
        size_t               received_length = 0u;
        utp_address_t        peer;
        utp_address_t        local;
        utp_packet_in_t*     packet_in = NULL;
        utp_internal_error_t error;
        uint64_t             now_us = 0u;

        error = utp_packet_in_pool_acquire(&context->packet_in_pool, &packet_in);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_udp_socket_recv_from_ex(&context->udp_socket, packet_in->data, packet_in->capacity,
                                                &received_length, &peer, &local);
            if (error == UTP_INTERNAL_ERROR_OK) {
                packet_in->length = (uint16_t)received_length;
                now_us            = utp_context_now_us();
                error = utp_context_dispatch_packet(context, packet_in->data, packet_in->length, packet_in, &peer,
                                                    &local, now_us);
            }
            utp_packet_in_release(packet_in);
        }

        if (error == UTP_INTERNAL_ERROR_WOULD_BLOCK) {
            return;
        }
        if (now_us != 0u) {
            utp_internal_error_t refresh_error;

            utp_context_drain_terminal_errors(context);
            refresh_error = utp_context_refresh_timer(context, now_us);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = refresh_error;
            } else if (refresh_error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, refresh_error, "context timer refresh failed");
            }
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, error, "udp packet handling failed");
            return;
        }
    }
#endif
}

static void utp_context_on_udp_writable(uint32_t events, void* user_data)
{
    utp_context_t*   context = user_data;
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if ((events & UTP_EVENT_WRITABLE) == 0u || context == NULL) {
        return;
    }
    {
        const utp_internal_error_t error = utp_context_retry_nat_probe_send(context, utp_context_now_us());

        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, error, "nat probe writable retry failed");
        }
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);
        utp_internal_error_t           error;

        if (!slot->connection.udp_write_pending) {
            continue;
        }
        error = utp_context_flush_connection(context, slot);
        if (error == UTP_INTERNAL_ERROR_OK && utp_connection_is_connected(&slot->connection)) {
            error = utp_context_complete_connected_side_effects(context, slot);
        }
        if (error == UTP_INTERNAL_ERROR_OK && utp_connection_is_connected(&slot->connection)) {
            error = utp_context_flush_connection(context, slot);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, error, "udp writable retry failed");
        }
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);

        if (slot->pending.handshake_write_pending) {
            const utp_internal_error_t error =
                utp_context_send_or_defer_pending_handshake(context, slot, utp_context_now_us());

            if (error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, error,
                                       "pending handshake writable retry failed");
                utp_context_release_pending_slot(context, slot);
            }
        }
    }
    utp_context_drain_terminal_errors(context);
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
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);
        uint64_t                       deadline;
        utp_internal_error_t           error;

        if (slot->zero_rtt_response_active &&
            ((slot->zero_rtt_expire_deadline_us != 0u && slot->zero_rtt_expire_deadline_us <= now_us) ||
             (slot->zero_rtt_response_deadline_us != 0u && slot->zero_rtt_response_deadline_us <= now_us &&
              slot->zero_rtt_response_retries >= context->handshake_max_retries))) {
            static const uint8_t timeout_reason[] = "0-RTT handshake response timeout";

            if (slot->connected_reported) {
                utp_context_report_connection_error(context, slot, UTP_STATUS_TIMEOUT, 0u, timeout_reason,
                                                    sizeof(timeout_reason) - 1u, false);
            }
            utp_context_release_connection_slot(context, slot);
            continue;
        }
        if (slot->zero_rtt_response_active && !slot->zero_rtt_response_queued &&
            slot->zero_rtt_response_deadline_us != 0u && slot->zero_rtt_response_deadline_us <= now_us) {
            error = utp_context_queue_zero_rtt_response(context, slot, now_us);
            if (error == UTP_INTERNAL_ERROR_OK) {
                error = utp_context_flush_connection(context, slot);
            }
            if (error != UTP_INTERNAL_ERROR_OK) {
                if (slot->terminal_error_queued) {
                    continue;
                }
                if (slot->connected_reported) {
                    static const uint8_t retry_reason[] = "0-RTT handshake response retry failed";

                    utp_context_report_connection_error(context, slot, utp_internal_error_to_status(error), 0u,
                                                        retry_reason, sizeof(retry_reason) - 1u, false);
                }
                utp_context_release_connection_slot(context, slot);
                continue;
            }
        }

        if (slot->connect_pending && !utp_connection_is_connected(&slot->connection) &&
            ((utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_CLOSING ||
              utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_DRAINING) ||
             (slot->connect_deadline_us != 0u && slot->connect_deadline_us <= now_us))) {
            const bool closed_during_handshake =
                utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_CLOSING ||
                utp_connection_state(&slot->connection) == UTP_CONNECTION_STATE_DRAINING;

            if (closed_during_handshake) {
                utp_context_fail_pending_connect(context, slot, UTP_STATUS_CANCELLED,
                                                 "connection closed during handshake");
            } else if (slot->connect_retries_remaining > 0) {
                --slot->connect_retries_remaining;
                error = utp_context_retry_connect_attempt(context, slot, now_us);
                if (error != UTP_INTERNAL_ERROR_OK && slot->used && !slot->terminal_error_queued) {
                    utp_context_fail_pending_connect(context, slot, utp_internal_error_to_status(error),
                                                     "connect retry failed");
                }
            } else {
                utp_context_fail_pending_connect(context, slot, UTP_STATUS_TIMEOUT, "connect timeout");
            }
            continue;
        }
        if (utp_connection_close_deadline(&slot->connection) != 0u &&
            utp_connection_close_deadline(&slot->connection) <= now_us) {
            utp_context_release_connection_slot(context, slot);
            continue;
        }
        error = utp_connection_on_close_retransmission_timeout(&slot->connection, now_us);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_flush_connection(context, slot);
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            if (slot->terminal_error_queued) {
                continue;
            }
            return error;
        }
        error = utp_connection_on_keepalive_timeout(&slot->connection, now_us);
        if (error == UTP_INTERNAL_ERROR_TIMEOUT) {
            static const uint8_t timeout_reason[] = "keepalive timeout";

            utp_context_report_connection_error(context, slot, UTP_STATUS_TIMEOUT, 0u, timeout_reason,
                                                sizeof(timeout_reason) - 1u, false);
            utp_context_release_connection_slot(context, slot);
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
            if (slot->terminal_error_queued) {
                continue;
            }
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
                if (slot->terminal_error_queued) {
                    continue;
                }
                return error;
            }
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_context_process_pending_timers(utp_context_t* context, uint64_t now_us)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);
        uint64_t                    deadline;

        deadline = utp_pending_incoming_handshake_deadline(&slot->pending);
        if (deadline != 0u && deadline <= now_us) {
            utp_internal_error_t error;

            if (slot->pending.handshake_retransmission_count > slot->pending.handshake_max_retries) {
                utp_context_log_ids(context, UTP_LOG_LEVEL_WARNING, "incoming handshake response timeout",
                                    slot->pending.local_cid, slot->pending.peer_cid);
                utp_context_release_pending_slot(context, slot);
                continue;
            }
            error = utp_context_send_or_defer_pending_handshake(context, slot, now_us);
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
        error = utp_context_process_nat_probe_timer(context, now_us);
    }
    utp_context_drain_terminal_errors(context);
    {
        const utp_internal_error_t refresh_error = utp_context_refresh_timer(context, utp_context_now_us());

        if (error == UTP_INTERNAL_ERROR_OK) {
            error = refresh_error;
        } else if (refresh_error != UTP_INTERNAL_ERROR_OK) {
            utp_internal_log_error(&context->logger, &context->tag, refresh_error, "context timer refresh failed");
        }
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context timer handling failed");
    }
}

utp_status_t utp_context_create(const utp_context_options_t* options, utp_context_t** out_context)
{
    size_t peer_id_length;

    if (out_context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    *out_context   = NULL;
    peer_id_length = options != NULL && options->peer_id != NULL ? strlen(options->peer_id) : 0u;
    if (options == NULL || options->event_base == NULL || !utp_context_log_level_is_valid(options->log_level) ||
        options->stream_scheduler_mode > UTP_STREAM_SCHEDULER_DRR ||
        (options->cc_algorithm != UTP_CONGESTION_DEFAULT && options->cc_algorithm != UTP_CONGESTION_BBR &&
         options->cc_algorithm != UTP_CONGESTION_CUBIC) ||
        peer_id_length == 0u || peer_id_length > UTP_PEER_ID_MAX_LENGTH) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    utp_context_t* context = utp_allocator_alloc(NULL, sizeof(*context));

    if (context == NULL) {
        return UTP_STATUS_NOMEM;
    }
    context->connections                 = (utp_hash_table_t){0};
    context->passive_connections_by_peer = (utp_hash_table_t){0};
    context->pending_incoming            = (utp_hash_table_t){0};
    context->pending_incoming_by_peer    = (utp_hash_table_t){0};
    context->zero_rtt_replay             = (utp_hash_table_t){0};
    TAILQ_INIT(&context->free_connection_slots);
    TAILQ_INIT(&context->terminal_error_slots);
    TAILQ_INIT(&context->free_pending_slots);
    utp_event_init(&context->udp_event);
    utp_event_init(&context->udp_write_event);
    utp_event_init(&context->timer_event);
    utp_udp_socket_init(&context->udp_socket);
    context->bound_address                = (utp_address_t){0};
    context->next_nat_probe_packet_number = 1u;
    memcpy(context->peer_id, options->peer_id, peer_id_length);
    context->peer_id[peer_id_length] = '\0';
    context->peer_id_length          = (uint8_t)peer_id_length;
    utp_nat_probe_task_reset(&context->nat_probe);
    context->nat_result                       = (utp_nat_probe_result_t){0};
    context->nat_result_valid                 = false;
    context->packet_in_pool.allocator         = NULL;
    context->packet_in_pool.blocks            = NULL;
    context->packet_in_pool.free_packets      = NULL;
    context->packet_in_pool.packet_capacity   = 0u;
    context->packet_in_pool.free_count        = 0u;
    context->packet_in_pool.grow_capacity     = 0u;
    context->packet_in_pool.block_capacity    = 0u;
    context->packet_in_pool.max_free_capacity = 0u;
    context->packet_in_pool.buffer_capacity   = 0u;
    context->packet_in_pool.dynamic           = false;

    context->on_connected                  = NULL;
    context->on_connected_user_data        = NULL;
    context->on_connect_error              = NULL;
    context->on_connect_error_user_data    = NULL;
    context->on_new_connection             = NULL;
    context->on_new_connection_user_data   = NULL;
    context->on_connection_error           = NULL;
    context->on_connection_error_user_data = NULL;
    context->callback_accept_pending       = NULL;
    context->callback_accept_zero_rtt      = NULL;
    context->callback_accept_requested     = false;
    context->next_cid                      = (uint32_t)options->context_id;
    context->next_cid                      = context->next_cid == 0u ? 1u : context->next_cid;
    context->stream_scheduler_mode         = options->stream_scheduler_mode;
    context->cc_algorithm                  = options->cc_algorithm;
    context->clock_granularity_us          = options->clock_granularity_us == 0u ? 1u : options->clock_granularity_us;
    context->bbr_config.initial_cwnd_mss   = options->bbr_init_cwnd_mss;
    context->bbr_config.minimum_cwnd_mss   = options->bbr_min_cwnd_mss;
    context->bbr_config.startup_high_gain  = options->bbr_startup_high_gain;
    context->bbr_config.cwnd_gain          = options->bbr_cwnd_gain;
    context->bbr_config.startup_growth_target         = options->bbr_startup_growth_target;
    context->bbr_config.startup_full_bandwidth_rounds = options->bbr_startup_full_bw_rounds;
    context->bbr_config.probe_rtt_ms                  = options->bbr_probe_rtt_ms;
    context->bbr_config.min_rtt_expiry_ms             = options->bbr_min_rtt_expiry_ms;
    for (uint32_t index = 0u; index < UTP_BBR_PACING_GAIN_COUNT; ++index) {
        context->bbr_config.pacing_gains[index] = options->bbr_pacing_gains[index];
    }
    context->cubic_config.beta                   = options->cubic_beta;
    context->cubic_config.cubic_c                = options->cubic_c;
    context->cubic_config.initial_cwnd_mss       = options->cubic_init_cwnd_mss;
    context->cubic_config.minimum_cwnd_mss       = options->cubic_min_cwnd_mss;
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
    context->zero_rtt_token_max_lifetime_seconds = options->zero_rtt_token_max_lifetime_seconds;
    context->zero_rtt_replay_cache_capacity      = options->zero_rtt_replay_cache_capacity == 0u
                                                       ? UTP_CONTEXT_ZERO_RTT_REPLAY_DEFAULT_CAPACITY
                                                       : options->zero_rtt_replay_cache_capacity;
    context->stream_terminal_capacity            = options->stream_terminal_capacity == 0u
                                                       ? UTP_CONNECTION_STREAM_TERMINAL_DEFAULT_CAPACITY
                                                       : options->stream_terminal_capacity;
    context->path_validation_buffer_capacity     = options->path_validation_buffer_capacity;
    context->handshake_timeout_ms                = options->handshake_timeout == 0u ? 800u : options->handshake_timeout;
    context->handshake_max_retries               = options->handshake_max_retries;
    context->local_transport_params.flags        = UTP_TRANSPORT_PARAMS_DEFAULT_FLAGS;
    context->local_transport_params.max_idle_timeout_ms =
        options->max_idle_timeout == 0u ? 30000u : options->max_idle_timeout;
    context->local_transport_params.handshake_timeout_ms = context->handshake_timeout_ms;
    context->local_transport_params.initial_max_streams_bidi =
        options->initial_max_streams_bidi == 0u ? 32u : options->initial_max_streams_bidi;
    context->local_transport_params.initial_max_streams_uni =
        options->initial_max_streams_uni == 0u ? 16u : options->initial_max_streams_uni;
    context->local_transport_params.ack_delay_exponent =
        options->ack_delay_exponent > UTP_TRANSPORT_PARAMS_MAX_ACK_EXPONENT ? UTP_TRANSPORT_PARAMS_MAX_ACK_EXPONENT
                                                                            : options->ack_delay_exponent;
    context->local_transport_params.initial_max_data =
        options->initial_max_data == 0u ? UINT64_C(8) * 1024u * 1024u : options->initial_max_data;
    context->local_transport_params.initial_max_stream_data_bidi_local =
        options->initial_max_stream_data_bidi_local == 0u ? UINT64_C(256) * 1024u
                                                          : options->initial_max_stream_data_bidi_local;
    context->local_transport_params.initial_max_stream_data_bidi_remote =
        options->initial_max_stream_data_bidi_remote == 0u ? UINT64_C(256) * 1024u
                                                           : options->initial_max_stream_data_bidi_remote;
    if (context->local_transport_params.initial_max_data > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL) {
        context->local_transport_params.initial_max_data = UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL;
    }
    if (context->local_transport_params.initial_max_stream_data_bidi_local > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL) {
        context->local_transport_params.initial_max_stream_data_bidi_local = UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL;
    }
    if (context->local_transport_params.initial_max_stream_data_bidi_remote > UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL) {
        context->local_transport_params.initial_max_stream_data_bidi_remote = UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL;
    }
    context->local_ack_frequency.ack_eliciting_threshold =
        options->ack_every_n_packets == 0u ? 4u : options->ack_every_n_packets;
    if (context->local_ack_frequency.ack_eliciting_threshold > UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD) {
        context->local_ack_frequency.ack_eliciting_threshold = UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD;
    }
    context->local_ack_frequency.reordering_threshold = 3u;
    context->local_ack_frequency.max_ack_delay_ms     = options->ack_delay == 0u ? 25u : options->ack_delay;
    if (context->local_ack_frequency.max_ack_delay_ms > UTP_ACK_FREQUENCY_MAX_DELAY_MS) {
        context->local_ack_frequency.max_ack_delay_ms = UTP_ACK_FREQUENCY_MAX_DELAY_MS;
    }
    context->enable_keepalive                      = options->enable_keepalive;
    context->keepalive_interval_ms                 = options->keepalive_interval;
    context->keepalive_timeout_ms                  = options->keepalive_timeout;
    context->keepalive_probes                      = options->keepalive_probes;
    context->resumption_key_explicit               = false;
    context->resumption_keys_ready                 = false;
    context->default_resumption_key_warning_logged = false;
    utp_crypto_default_resumption_key(context->resumption_root_key);
    utp_internal_error_t error = utp_hash_table_init(&context->connections, NULL, SIZE_MAX);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&context->passive_connections_by_peer, NULL, SIZE_MAX);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&context->pending_incoming, NULL,
                                    options->pending_incoming_limit == 0u ? UTP_CONTEXT_PENDING_INCOMING_DEFAULT_LIMIT
                                                                          : options->pending_incoming_limit);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&context->pending_incoming_by_peer, NULL, context->pending_incoming.max_entries);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&context->zero_rtt_replay, NULL, context->zero_rtt_replay_cache_capacity);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_crypto_derive_resumption_keys(&context->resumption_keys, context->resumption_root_key);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        context->resumption_keys_ready = true;
    }
    context->logger.sink  = options->log_sink;
    context->logger.level = options->log_level;
    char    fragment[32];
    int32_t fragment_length = snprintf(fragment, sizeof(fragment), "context %" PRIu64, options->context_id);
    if (fragment_length < 0 || (size_t)fragment_length >= sizeof(fragment)) {
        utp_hash_table_cleanup(&context->connections, NULL, NULL);
        utp_hash_table_cleanup(&context->passive_connections_by_peer, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming_by_peer, NULL, NULL);
        utp_hash_table_cleanup(&context->zero_rtt_replay, utp_context_free_replay_entry, NULL);
        utp_crypto_secure_clear(context->resumption_root_key, sizeof(context->resumption_root_key));
        utp_crypto_resumption_keys_clear(&context->resumption_keys);
        utp_allocator_free(NULL, context);
        return UTP_STATUS_OVERFLOW;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_log_tag_init(&context->tag, fragment, (size_t)fragment_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_event_loop_init(&context->event_loop, options->event_base, &context->logger, &context->tag);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        const uint16_t packet_in_minimum = utp_mtu_normalize(context->mtu_config.mtu_min, UTP_ADDRESS_FAMILY_IPV6);
        const uint16_t packet_in_capacity =
            context->mtu_config.mtu_max < packet_in_minimum ? packet_in_minimum : context->mtu_config.mtu_max;

        const uint32_t packet_in_max_free =
            options->packet_in_max_free == 0u ? UTP_CONTEXT_PACKET_IN_DEFAULT_MAX_FREE : options->packet_in_max_free;

        error = utp_packet_in_pool_init_dynamic(&context->packet_in_pool, NULL, UTP_CONTEXT_PACKET_IN_GROW_CAPACITY,
                                                UTP_CONTEXT_PACKET_IN_BLOCK_CAPACITY, packet_in_max_free,
                                                packet_in_capacity);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context initialization failed");
        utp_packet_in_pool_cleanup(&context->packet_in_pool);
        utp_hash_table_cleanup(&context->connections, NULL, NULL);
        utp_hash_table_cleanup(&context->passive_connections_by_peer, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming_by_peer, NULL, NULL);
        utp_hash_table_cleanup(&context->zero_rtt_replay, utp_context_free_replay_entry, NULL);
        utp_crypto_secure_clear(context->resumption_root_key, sizeof(context->resumption_root_key));
        utp_crypto_resumption_keys_clear(&context->resumption_keys);
        utp_event_loop_close(&context->event_loop);
        utp_allocator_free(NULL, context);
        return utp_internal_error_to_status(error);
    }
    *out_context = context;
    utp_context_log(context, UTP_LOG_LEVEL_INFO, "context created");
    return UTP_STATUS_OK;
}

void utp_context_destroy(utp_context_t* context)
{
    if (context != NULL) {
        utp_hash_iter_t  iter;
        utp_hash_node_t* node;

        utp_context_log(context, UTP_LOG_LEVEL_INFO, "context destroy started");
        utp_event_remove(&context->timer_event);
        utp_event_remove(&context->udp_write_event);
        utp_event_remove(&context->udp_event);
        utp_nat_probe_task_reset(&context->nat_probe);
        utp_hash_iter_init(&iter);
        while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
            utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);

            if (!slot->terminal_error_queued) {
                utp_context_send_destroy_close(context, slot);
            }
            utp_context_release_connection_slot(context, slot);
        }
        utp_hash_iter_init(&iter);
        while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
            utp_context_release_pending_slot(context, utp_context_pending_slot_from_node(node));
        }
        utp_hash_table_cleanup(&context->connections, NULL, NULL);
        utp_hash_table_cleanup(&context->passive_connections_by_peer, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming_by_peer, NULL, NULL);
        while (!TAILQ_EMPTY(&context->free_connection_slots)) {
            utp_context_connection_slot_t* slot = TAILQ_FIRST(&context->free_connection_slots);

            TAILQ_REMOVE(&context->free_connection_slots, slot, free_next);
            utp_allocator_free(NULL, slot);
        }
        while (!TAILQ_EMPTY(&context->free_pending_slots)) {
            utp_context_pending_slot_t* slot = TAILQ_FIRST(&context->free_pending_slots);

            TAILQ_REMOVE(&context->free_pending_slots, slot, free_next);
            utp_allocator_free(NULL, slot);
        }
        utp_udp_socket_close(&context->udp_socket);
        utp_packet_in_pool_cleanup(&context->packet_in_pool);
        utp_hash_table_cleanup(&context->zero_rtt_replay, utp_context_free_replay_entry, NULL);
        utp_crypto_secure_clear(context->resumption_root_key, sizeof(context->resumption_root_key));
        utp_crypto_resumption_keys_clear(&context->resumption_keys);
        utp_event_loop_close(&context->event_loop);
        utp_allocator_free(NULL, context);
    }
}

utp_status_t utp_context_bind(utp_context_t* context, const char* address, uint16_t port, const char* ifname,
                              uint16_t* out_port)
{
    if (out_port != NULL) {
        *out_port = 0u;
    }
    if (context == NULL || address == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_OPEN;
    }
    utp_address_t        requested;
    utp_address_t        local;
    utp_internal_error_t error = utp_address_parse(&requested, address, port);
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
        utp_internal_log_error(&context->logger, &context->tag, error, "udp bind failed");
        utp_event_remove(&context->udp_event);
        utp_udp_socket_close(&context->udp_socket);
        return utp_internal_error_to_status(error);
    }
    if (out_port != NULL) {
        *out_port = local.port;
    }
    context->bound_address = local;
    utp_context_log(context, UTP_LOG_LEVEL_INFO, "udp socket bound");
    return UTP_STATUS_OK;
}

utp_status_t utp_context_probe_nat(utp_context_t* context, const utp_nat_probe_options_t* options,
                                   utp_on_nat_probe_fn callback, void* user_data)
{
    utp_address_t        endpoint;
    utp_internal_error_t error;
    uint64_t             now_us;

    if (context == NULL || options == NULL || callback == NULL || options->nat_service_address == NULL ||
        options->nat_service_port == 0u) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (!utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_NOT_BOUND;
    }
    if (context->nat_probe.active) {
        return UTP_STATUS_IN_PROGRESS;
    }
    error = utp_address_parse(&endpoint, options->nat_service_address, options->nat_service_port);
    if (error != UTP_INTERNAL_ERROR_OK || endpoint.family != context->bound_address.family) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (options->phase_timeout_ms != 0u && options->phase_timeout_ms < UTP_NAT_PROBE_MIN_TIMEOUT) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    utp_nat_probe_task_reset(&context->nat_probe);
    context->nat_probe.primary_endpoint = endpoint;
    context->nat_probe.callback         = callback;
    context->nat_probe.user_data        = user_data;
    context->nat_probe.phase_timeout_ms =
        options->phase_timeout_ms == 0u ? UTP_NAT_PROBE_DEFAULT_TIMEOUT : options->phase_timeout_ms;
    context->nat_probe.result.address_family = context->bound_address.family;
    context->nat_probe.active                = true;
    now_us                                   = utp_context_now_us();
    error = utp_context_start_nat_step(context, UTP_NAT_PROBE_STEP_PRIMARY_BINDING, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_fail_nat_probe(context, error, now_us);
    }
    error = utp_context_refresh_timer(context, now_us);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    return UTP_STATUS_OK;
}

utp_status_t utp_context_cancel_nat_probe(utp_context_t* context)
{
    utp_internal_error_t error;
    uint64_t             now_us;

    if (context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (!context->nat_probe.active) {
        return UTP_STATUS_NOT_FOUND;
    }
    now_us = utp_context_now_us();
    utp_nat_probe_task_reset(&context->nat_probe);
    utp_context_disable_udp_write_event_if_idle(context);
    error = utp_context_refresh_timer(context, now_us);
    return error == UTP_INTERNAL_ERROR_OK ? UTP_STATUS_OK : utp_internal_error_to_status(error);
}

void utp_context_set_logger(utp_context_t* context, utp_log_sink_fn callback, utp_log_level_t level)
{
    if (context == NULL || !utp_context_log_level_is_valid(level)) {
        return;
    }
    context->logger.sink  = callback;
    context->logger.level = level;
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

void utp_context_set_resumption_key(utp_context_t* context, const uint8_t root_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE])
{
    if (context == NULL || root_key == NULL) {
        return;
    }
    utp_crypto_resumption_keys_t derived;
    utp_internal_error_t         error = utp_crypto_derive_resumption_keys(&derived, root_key);
    // 新 root 派生是否成功都必须先废止旧状态，禁止继续导出旧凭证。
    utp_context_invalidate_resumption_state(context);
    utp_context_clear_zero_rtt_replay(context);
    utp_crypto_secure_clear(context->resumption_root_key, sizeof(context->resumption_root_key));
    utp_crypto_resumption_keys_clear(&context->resumption_keys);
    context->resumption_keys_ready                 = false;
    context->resumption_key_explicit               = false;
    context->default_resumption_key_warning_logged = false;
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_resumption_keys_clear(&derived);
        utp_context_log(context, UTP_LOG_LEVEL_ERROR, "resumption key derivation failed");
        return;
    }
    memcpy(context->resumption_root_key, root_key, sizeof(context->resumption_root_key));
    memcpy(&context->resumption_keys, &derived, sizeof(context->resumption_keys));
    utp_crypto_resumption_keys_clear(&derived);
    context->resumption_key_explicit               = true;
    context->resumption_keys_ready                 = true;
    context->default_resumption_key_warning_logged = false;
}

utp_status_t utp_context_connect(utp_context_t* context, const utp_connect_options_t* options)
{
    if (context == NULL || options == NULL || options->address == NULL || options->port == 0u ||
        !utp_context_encryption_mode_is_valid(options->encryption)) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (!utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_NOT_BOUND;
    }
    utp_address_t        peer;
    utp_internal_error_t error = utp_address_parse(&peer, options->address, options->port);

    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    utp_context_connection_slot_t* existing = utp_context_find_connection_by_peer(context, &peer);

    if (existing != NULL) {
        return utp_connection_is_connected(&existing->connection) ? UTP_STATUS_SOCKET_CONNECTED
                                                                  : UTP_STATUS_IN_PROGRESS;
    }
    utp_context_connection_slot_t* slot = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        return UTP_STATUS_NOMEM;
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

    const uint64_t now_us = utp_context_now_us();
    error                 = utp_context_start_connect_attempt(context, slot, &peer, now_us);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_refresh_timer(context, now_us);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        const utp_status_t status = utp_internal_error_to_status(error);

        if (slot->terminal_error_queued) {
            const utp_internal_error_t refresh_error = utp_context_refresh_timer(context, now_us);

            if (refresh_error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, refresh_error,
                                       "terminal error timer refresh failed");
            }
        } else if (slot->used) {
            utp_context_fail_pending_connect(context, slot, status, "active connect failed");
        }
        return status;
    }
    return UTP_STATUS_OK;
}

utp_status_t utp_context_connect_0rtt(utp_context_t* context, const utp_connect_0rtt_options_t* options)
{
    if (context == NULL || options == NULL || options->address == NULL || options->port == 0u ||
        options->session_token == NULL || options->session_token_size == 0u ||
        (options->early_data == NULL && options->early_data_size != 0u)) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (!utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_NOT_BOUND;
    }
    if (!context->resumption_keys_ready) {
        return UTP_STATUS_STATE;
    }
    uint8_t              state_payload[UINT8_MAX];
    uint8_t              encryption_mode;
    uint64_t             expires_at_seconds;
    size_t               state_payload_length;
    utp_internal_error_t error = utp_crypto_local_resumption_state_open(
        context->resumption_keys.local_state_key, options->session_token, options->session_token_size, &encryption_mode,
        &expires_at_seconds, state_payload, sizeof(state_payload), &state_payload_length);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return utp_internal_error_to_status(error);
    }
    if (expires_at_seconds <= utp_context_now_seconds() ||
        state_payload_length < UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return UTP_STATUS_AUTH;
    }
    if (encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_NONE &&
        encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_128 &&
        encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_AES_GCM_256) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return UTP_STATUS_AUTH;
    }
    utp_address_t peer;

    error = utp_address_parse(&peer, options->address, options->port);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return utp_internal_error_to_status(error);
    }
    {
        const uint16_t target_size = utp_mtu_packet_size_from_mtu(context->mtu_config.mtu_min, peer.family);
        size_t         fixed_length =
            UTP_PACKET_HEADER_SIZE + UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE;
        size_t first_data_capacity;

        if (encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_NONE) {
            fixed_length += UTP_CRYPTO_AEAD_TAG_SIZE + UTP_FRAME_CRYPTO_SIZE + UTP_FRAME_VERSION_SIZE +
                            UTP_FRAME_TRANSPORT_PARAMS_SIZE + UTP_FRAME_ACK_FREQUENCY_SIZE + UTP_FRAME_PING_SIZE;
        }
        if ((size_t)target_size < fixed_length + UTP_FRAME_STREAM_HEADER_SIZE) {
            utp_crypto_secure_clear(state_payload, state_payload_length);
            return UTP_STATUS_OVERFLOW;
        }
        first_data_capacity = (size_t)target_size - fixed_length - UTP_FRAME_STREAM_HEADER_SIZE;
        if (options->early_data_size > first_data_capacity + UTP_STREAM_SEND_BUFFER_CAPACITY) {
            utp_crypto_secure_clear(state_payload, state_payload_length);
            return UTP_STATUS_OVERFLOW;
        }
    }
    utp_context_connection_slot_t* existing = utp_context_find_connection_by_peer(context, &peer);
    if (existing != NULL) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return utp_connection_is_connected(&existing->connection) ? UTP_STATUS_SOCKET_CONNECTED
                                                                  : UTP_STATUS_IN_PROGRESS;
    }
    utp_context_connection_slot_t* slot = utp_context_alloc_connection_slot(context);
    if (slot == NULL) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return UTP_STATUS_NOMEM;
    }
    utp_context_endpoint_from_address(&slot->connect_attempt.remote, &peer);
    slot->connect_attempt.timeout_ms         = options->timeout_ms == 0u ? 3000u : options->timeout_ms;
    slot->connect_attempt.retries            = options->retries;
    slot->connect_attempt.encryption         = (utp_encryption_mode_t)encryption_mode;
    slot->connect_attempt.type               = UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE;
    slot->connect_attempt.session_token_size = UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE;
    slot->connect_attempt.resumption_state_size =
        options->session_token_size > UINT32_MAX ? UINT32_MAX : (uint32_t)options->session_token_size;
    slot->connect_attempt.early_data_size = (uint32_t)options->early_data_size;
    slot->connect_attempt.early_fin       = options->early_fin;
    memcpy(slot->zero_rtt_resumption_psk, state_payload, sizeof(slot->zero_rtt_resumption_psk));
    memcpy(slot->zero_rtt_session_token + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE,
           state_payload + UTP_CRYPTO_RESUMPTION_PSK_SIZE, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE);
    utp_crypto_secure_clear(state_payload, state_payload_length);
    error = utp_crypto_random_bytes(slot->zero_rtt_session_token, UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(context, slot);
        return utp_internal_error_to_status(error);
    }
    slot->zero_rtt_expires_at_seconds = expires_at_seconds;
    slot->zero_rtt_encryption_mode    = encryption_mode;
    if (options->early_data_size != 0u) {
        slot->zero_rtt_early_data = utp_allocator_alloc(NULL, options->early_data_size);
        if (slot->zero_rtt_early_data == NULL) {
            utp_context_release_connection_slot(context, slot);
            return UTP_STATUS_NOMEM;
        }
        memcpy(slot->zero_rtt_early_data, options->early_data, options->early_data_size);
    }
    slot->zero_rtt_early_data_size  = options->early_data_size;
    slot->zero_rtt_early_fin        = options->early_fin;
    slot->connect_retries_remaining = options->retries < 0 ? 0 : options->retries;
    slot->connect_pending           = true;

    const uint64_t now_us = utp_context_now_us();
    error                 = utp_context_start_connect_attempt(context, slot, &peer, now_us);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_refresh_timer(context, now_us);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        const utp_status_t status = utp_internal_error_to_status(error);

        if (slot->terminal_error_queued) {
            const utp_internal_error_t refresh_error = utp_context_refresh_timer(context, now_us);

            if (refresh_error != UTP_INTERNAL_ERROR_OK) {
                utp_internal_log_error(&context->logger, &context->tag, refresh_error,
                                       "terminal error timer refresh failed");
            }
        } else if (slot->used) {
            utp_context_fail_pending_connect(context, slot, status, "0-rtt connect failed");
        }
        return status;
    }
    return UTP_STATUS_OK;
}

utp_status_t utp_context_accept(utp_context_t* context)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    if (!utp_udp_socket_is_open(&context->udp_socket)) {
        return UTP_STATUS_SOCKET_NOT_BOUND;
    }
    if (context->callback_accept_pending != NULL || context->callback_accept_zero_rtt != NULL) {
        if (context->callback_accept_requested) {
            return UTP_STATUS_WOULD_BLOCK;
        }
        context->callback_accept_requested = true;
        return UTP_STATUS_OK;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);
        utp_internal_error_t        error;

        if (!slot->queued) {
            continue;
        }
        error = utp_context_accept_pending_slot(context, slot);
        return utp_internal_error_to_status(error);
    }
    return UTP_STATUS_WOULD_BLOCK;
}
