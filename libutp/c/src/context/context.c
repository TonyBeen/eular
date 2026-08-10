#include "context/context.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
static utp_internal_error_t utp_context_accept_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot);
static void utp_context_release_connection_slot(utp_context_t* context, utp_context_connection_slot_t* slot);
static void utp_context_release_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot);
static void utp_context_report_connected(utp_context_t* context, utp_context_connection_slot_t* slot);
static utp_internal_error_t utp_context_queue_session_token(utp_context_t*                 context,
                                                            utp_context_connection_slot_t* slot);

/** @brief 返回 Unix 秒；票据时效必须使用墙上时间而非单调时钟。 */
static uint64_t             utp_context_now_seconds(void)
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

/** @brief 按本地 CID 比较 Connection 槽位。 */
static bool utp_context_connection_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_connection_slot_t* slot;

    (void)user_data;
    slot = (const utp_context_connection_slot_t*)((const uint8_t*)node - offsetof(utp_context_connection_slot_t, node));
    return key != NULL && slot->connection.local_cid == *(const uint32_t*)key;
}

/** @brief 从哈希节点取得动态 pending 槽位。 */
static utp_context_pending_slot_t* utp_context_pending_slot_from_node(utp_hash_node_t* node)
{
    return node == NULL ? NULL
                        : (utp_context_pending_slot_t*)((uint8_t*)node - offsetof(utp_context_pending_slot_t, node));
}

/** @brief 按本地 CID 比较 pending 槽位。 */
static bool utp_context_pending_slot_matches(const utp_hash_node_t* node, const void* key, void* user_data)
{
    const utp_context_pending_slot_t* slot;

    (void)user_data;
    slot = (const utp_context_pending_slot_t*)((const uint8_t*)node - offsetof(utp_context_pending_slot_t, node));
    return key != NULL && slot->pending.local_cid == *(const uint32_t*)key;
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
    const uint8_t early_attempt_nonce[UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE], uint64_t packet_number,
    uint64_t now_seconds, uint64_t expires_at_seconds)
{
    uint8_t              digest[UTP_CRYPTO_SHA256_SIZE];
    uint8_t              key[UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE];
    utp_internal_error_t error;

    if (context == NULL || encrypted_server_info == NULL || early_attempt_nonce == NULL || packet_number == 0u ||
        expires_at_seconds <= now_seconds) {
        return false;
    }
    error = utp_crypto_sha256(encrypted_server_info, UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE, digest);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    memcpy(key, digest, 16u);
    memcpy(key + 16u, early_attempt_nonce, UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE);
    for (size_t index = 0u; index < 8u; ++index) {
        key[32u + index] = (uint8_t)(packet_number >> (56u - index * 8u));
    }
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
    if (context == NULL || context->logger.sink == NULL || message == NULL || level < context->log_level ||
        level > UTP_LOG_LEVEL_ERROR) {
        return;
    }
    utp_internal_log(&context->logger, &context->tag, level, message);
}

static void utp_context_log_ids(utp_context_t* context, utp_log_level_t level, const char* event, uint32_t local_cid,
                                uint32_t peer_cid)
{
    char message[160];

    if (event == NULL) {
        return;
    }
    (void)snprintf(message, sizeof(message), "%s: local_cid=%" PRIu32 ", peer_cid=%" PRIu32, event, local_cid,
                   peer_cid);
    utp_context_log(context, level, message);
}

static void utp_context_log_close(utp_context_t* context, const utp_context_connection_slot_t* slot,
                                  utp_status_t status, uint16_t peer_error_code, bool peer_initiated)
{
    char message[256];

    (void)snprintf(message, sizeof(message),
                   "connection closed: local_cid=%" PRIu32 ", status=%s, peer_initiated=%u, peer_error=%" PRIu16,
                   slot->connection.local_cid, utp_status_string(status), peer_initiated ? 1u : 0u, peer_error_code);
    utp_context_log(context, status == UTP_STATUS_OK ? UTP_LOG_LEVEL_INFO : UTP_LOG_LEVEL_WARNING, message);
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
    const uint8_t  encryption_mode    = slot->connection.crypto_configured
                                            ? (uint8_t)utp_context_encryption_from_crypto_type(slot->connection.crypto_type)
                                            : (uint8_t)UTP_ENCRYPTION_NONE;
    uint8_t        token_payload[UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE];
    uint8_t        payload[UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + UTP_CRYPTO_SESSION_TOKEN_PAYLOAD_SIZE];
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
           level == UTP_LOG_LEVEL_ERROR;
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
    slot->connection.local_cid        = 0u;
    slot->connection.peer_cid         = 0u;
    slot->connect_deadline_us         = 0u;
    slot->connect_retries_remaining   = 0;
    slot->zero_rtt_early_data_size    = 0u;
    slot->zero_rtt_expires_at_seconds = 0u;
    slot->zero_rtt_early_fin          = false;
    slot->zero_rtt_awaiting_accept    = false;
    slot->zero_rtt_accepted           = false;
    slot->zero_rtt_encryption_mode    = UTP_CRYPTO_ENCRYPTION_MODE_NONE;
    slot->used                        = true;
    slot->connected_reported          = false;
    slot->connection_error_reported   = false;
    slot->connect_pending             = false;
    return slot;
}

/** @brief 将完成初始化的 Connection 槽位注册到 CID 哈希表。 */
static utp_internal_error_t utp_context_register_connection_slot(utp_context_t*                 context,
                                                                 utp_context_connection_slot_t* slot)
{
    const uint32_t local_cid = slot == NULL ? 0u : slot->connection.local_cid;

    if (context == NULL || slot == NULL || !slot->used || local_cid == 0u || slot->node.table != NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return utp_hash_table_insert(&context->connections, &slot->node, local_cid, &local_cid,
                                 utp_context_connection_slot_matches, NULL);
}

/** @brief 从 CID 哈希表摘除 Connection，但保留槽位供重试重新初始化。 */
static void utp_context_unregister_connection_slot(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (context != NULL && slot != NULL && slot->node.table == &context->connections) {
        (void)utp_hash_table_remove(&context->connections, &slot->node);
    }
}

static void utp_context_release_connection_slot(utp_context_t* context, utp_context_connection_slot_t* slot)
{
    if (context != NULL && slot != NULL && slot->used) {
        utp_context_unregister_connection_slot(context, slot);
        if (slot->connection.local_cid != 0u) {
            utp_connection_cleanup(&slot->connection);
        }
        slot->connected_reported        = false;
        slot->connection_error_reported = false;
        slot->connect_deadline_us       = 0u;
        slot->connect_retries_remaining = 0;
        slot->connect_pending           = false;
        utp_crypto_secure_clear(slot->zero_rtt_resumption_psk, sizeof(slot->zero_rtt_resumption_psk));
        slot->zero_rtt_early_data_size    = 0u;
        slot->zero_rtt_expires_at_seconds = 0u;
        slot->zero_rtt_early_fin          = false;
        slot->zero_rtt_awaiting_accept    = false;
        slot->zero_rtt_accepted           = false;
        slot->zero_rtt_encryption_mode    = UTP_CRYPTO_ENCRYPTION_MODE_NONE;
        slot->used                        = false;
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
    // 先锁定回调只触发一次，再进入严格关闭状态；后续重复错误只会沿用同一个 close 包。
    utp_context_report_connection_error(context, slot, status, 0u, (const uint8_t*)reason, strlen(reason), false);
    close_error = utp_connection_queue_close(&slot->connection, close_code);
    if (close_error != UTP_INTERNAL_ERROR_OK) {
        return close_error;
    }
    return utp_context_flush_connection(context, slot);
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
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL || peer == NULL) {
        return NULL;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);

        if (slot->pending.peer_cid == peer_cid && utp_address_equal(&slot->pending.peer, peer)) {
            return slot;
        }
    }
    return NULL;
}

static utp_context_pending_slot_t* utp_context_alloc_pending_slot(utp_context_t* context)
{
    utp_context_pending_slot_t* slot;

    if (context == NULL || utp_hash_table_count(&context->pending_incoming) >= UTP_CONTEXT_MAX_PENDING_INCOMING) {
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
    slot->pending.local_cid = 0u;
    slot->used              = true;
    slot->queued            = false;
    return slot;
}

/** @brief 将 pending 注册到 CID 哈希表并计入 1024 项容量。 */
static utp_internal_error_t utp_context_register_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot)
{
    const uint32_t local_cid = slot == NULL ? 0u : slot->pending.local_cid;

    if (context == NULL || slot == NULL || !slot->used || local_cid == 0u || slot->node.table != NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    return utp_hash_table_insert(&context->pending_incoming, &slot->node, local_cid, &local_cid,
                                 utp_context_pending_slot_matches, NULL);
}

static void utp_context_release_pending_slot(utp_context_t* context, utp_context_pending_slot_t* slot)
{
    if (context != NULL && slot != NULL && slot->used) {
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

static utp_internal_error_t utp_context_send_packet(utp_context_t* context, const utp_connection_t* connection,
                                                    const utp_address_t* peer, const utp_packet_out_t* packet)
{
    if (packet == NULL || packet->raw_data == NULL || packet->data_size == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((packet->po_flags & UTP_PO_ENCRYPTED) != 0u) {
        size_t               wire_length;
        utp_internal_error_t error;

        error = utp_connection_encode_packet_wire(connection, packet, context->encrypt_send_buffer,
                                                  sizeof(context->encrypt_send_buffer), &wire_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        return utp_context_send_raw(context, peer, context->encrypt_send_buffer, wire_length);
    }
    if (packet->slice_count == 0u) {
        return utp_context_send_raw(context, peer, packet->raw_data, packet->data_size);
    }
    if (packet->slice_count > UTP_PACKET_OUT_MAX_SLICES) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_udp_send_slice_t slices[UTP_PACKET_OUT_MAX_SLICES];
    size_t               sent_length  = 0u;
    size_t               total_length = 0u;
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
    return utp_udp_socket_send_to_slices(&context->udp_socket, slices, packet->slice_count, peer, &sent_length);
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
    const utp_status_t status        = utp_internal_error_to_status(error);
    const size_t       reason_length = reason == NULL ? 0u : strlen(reason);

    if (slot->connect_pending && !utp_connection_is_connected(&slot->connection)) {
        utp_context_report_connect_error(context, status, reason, &slot->connect_attempt);
    } else {
        utp_context_report_connection_error(context, slot, status, 0u, (const uint8_t*)reason, reason_length, false);
    }
    utp_context_release_connection_slot(context, slot);
}

static bool utp_context_has_pending_udp_write(const utp_context_t* context)
{
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if (context == NULL) {
        return false;
    }
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        const utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);

        if (slot->connection.udp_write_pending) {
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
        error = utp_context_send_packet(context, connection,
                                        packet->has_destination ? &packet->destination : &connection->peer, packet);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_on_packet_sent(connection, packet, utp_context_now_us());
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
    utp_context_connection_slot_t* slot;
    utp_internal_error_t           error;

    if (context == NULL || connection == NULL || connection->context != context) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    slot = utp_context_find_connection_slot(context, connection->local_cid);
    if (slot == NULL || &slot->connection != connection) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
    }
    error = utp_context_flush_connection(context, slot);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return utp_context_refresh_timer(context, utp_context_now_us());
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
        error = utp_context_send_raw(context, &pending->peer, packet, UTP_PACKET_HEADER_SIZE + wire_payload_length);
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
    uint8_t              payload[UTP_FRAME_VERSION_SIZE + UTP_FRAME_CRYPTO_SIZE];
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
    return utp_context_send_pending_packet(context, pending, UTP_PACKET_TYPE_HANDSHAKE, payload, payload_length,
                                           out_packet_number);
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
    uint64_t             packet_number;
    uint64_t             now_us;

    if (context == NULL || slot == NULL || !slot->used || !slot->queued) {
        return UTP_INTERNAL_ERROR_NOT_FOUND;
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
    return utp_context_queue_session_token(context, slot);
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
    uint8_t              payload[UTP_PACKET_MTU_FLOOR];
    size_t               payload_length;
    uint32_t             local_cid;
    utp_internal_error_t error;

    if (context == NULL || slot == NULL || peer == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_context_alloc_cid(context, &local_cid);
    if (error == UTP_INTERNAL_ERROR_OK && slot->connection.local_cid != 0u) {
        utp_context_unregister_connection_slot(context, slot);
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
    if (error == UTP_INTERNAL_ERROR_OK && slot->connect_attempt.encryption != UTP_ENCRYPTION_NONE) {
        uint8_t crypto_type;

        if (!utp_context_crypto_type_from_encryption(slot->connect_attempt.encryption, &crypto_type)) {
            error = UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
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

        error                = utp_connection_reserve_zero_rtt_stream(&slot->connection, slot->zero_rtt_early_data_size,
                                                                      slot->zero_rtt_early_fin);
        token.payload        = slot->zero_rtt_session_token;
        token.payload_length = UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE;
        token.expires_at_seconds = slot->zero_rtt_expires_at_seconds;
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_frame_session_token_encode(payload, sizeof(payload), &token);
        }
        payload_length = UTP_FRAME_SESSION_TOKEN_HEADER_SIZE + UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE;
        if (error == UTP_INTERNAL_ERROR_OK && (slot->zero_rtt_early_data_size != 0u || slot->zero_rtt_early_fin)) {
            error =
                utp_frame_stream_header_encode(payload + payload_length, sizeof(payload) - payload_length,
                                               slot->zero_rtt_early_fin ? UTP_STREAM_FLAG_FIN : UTP_STREAM_FLAG_NONE,
                                               0u, 0u, (uint16_t)slot->zero_rtt_early_data_size);
            if (error == UTP_INTERNAL_ERROR_OK) {
                memcpy(payload + payload_length + UTP_FRAME_STREAM_HEADER_SIZE, slot->zero_rtt_early_data,
                       slot->zero_rtt_early_data_size);
                payload_length += UTP_FRAME_STREAM_HEADER_SIZE + slot->zero_rtt_early_data_size;
            }
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_0RTT, payload, payload_length, true);
        }
    } else if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_encode_version_frame(payload, sizeof(payload), &payload_length);
        if (error == UTP_INTERNAL_ERROR_OK && slot->connection.crypto_configured) {
            error = utp_connection_encode_crypto(&slot->connection, payload + payload_length,
                                                 sizeof(payload) - payload_length);
            if (error == UTP_INTERNAL_ERROR_OK) {
                payload_length += UTP_FRAME_CRYPTO_SIZE;
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

/** @brief 0-RTT 显式重试只催发原始 PacketOut，不改变 CID、包号、nonce 或线上字节。 */
static utp_internal_error_t utp_context_retry_zero_rtt_attempt(utp_context_t*                 context,
                                                               utp_context_connection_slot_t* slot, uint64_t now_us)
{
    utp_internal_error_t error = UTP_INTERNAL_ERROR_OK;

    if (context == NULL || slot == NULL || now_us == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
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

    // Context 只注册一个 timer，每次从全部连接和握手项中选取最近期限。
    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
        const utp_context_connection_slot_t* slot       = utp_context_connection_slot_from_node(node);
        const utp_connection_t*              connection = &slot->connection;

        if (connection->state == UTP_CONNECTION_STATE_CLOSING || connection->state == UTP_CONNECTION_STATE_DRAINING) {
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
        slot->connection.context = context;
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        utp_connection_set_mtu_config(&slot->connection, &context->mtu_config);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_set_stream_scheduler_mode(&slot->connection, (uint8_t)context->stream_scheduler_mode);
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
        if (error == UTP_INTERNAL_ERROR_AUTH || error == UTP_INTERNAL_ERROR_CRYPTO) {
            return UTP_INTERNAL_ERROR_OK;
        }
        if (utp_context_is_peer_protocol_error(error)) {
            return utp_context_close_on_peer_protocol_error(context, slot, error);
        }
        return error;
    }
    if (slot->connection.role == UTP_CONNECTION_ROLE_ACTIVE && header->type == UTP_PACKET_TYPE_HANDSHAKE &&
        slot->connection.peer_handshake_packet_number == header->packet_number) {
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
    return utp_context_flush_connection(context, slot);
}

static utp_internal_error_t utp_context_on_pending_packet(utp_context_t* context, utp_context_pending_slot_t* slot,
                                                          uint8_t* packet, size_t packet_length,
                                                          utp_packet_in_t* packet_in, const utp_address_t* peer)
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
    error = utp_pending_incoming_on_packet(&slot->pending, packet, packet_length, wire_packet_length, peer, &result);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    if (result == UTP_PENDING_INCOMING_PROMOTE) {
        error = utp_context_promote_pending(context, slot, packet, packet_length, wire_packet_length, packet_in, peer);
    }
    return error;
}

static utp_internal_error_t utp_context_on_initial_packet(utp_context_t* context, const utp_packet_view_t* view,
                                                          const utp_address_t* peer)
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
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK && has_crypto) {
        error = utp_pending_incoming_configure_crypto(&slot->pending, &peer_crypto);
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

/** @brief 验证并接收首个明文 0-RTT 包，恢复凭证本身始终经过 Context 根密钥保护。 */
static utp_internal_error_t utp_context_on_zero_rtt_packet(utp_context_t* context, utp_packet_in_t* packet_in,
                                                           const utp_address_t* peer)
{
    if (context == NULL || packet_in == NULL || peer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    utp_packet_view_t    view;
    utp_internal_error_t error = utp_packet_view_decode(&view, packet_in->data, packet_in->length);

    if (error != UTP_INTERNAL_ERROR_OK || view.header.type != UTP_PACKET_TYPE_0RTT || view.header.dcid != 0u ||
        view.header.scid == 0u || UTP_PACKET_HEADER_SIZE + view.payload_length > UTP_PACKET_MTU_FLOOR) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    utp_frame_session_token_t session_token;
    bool                      has_token = false;
    size_t                    offset    = 0u;
    while (offset < view.payload_length) {
        const uint8_t* frame;
        uint8_t        frame_type;
        size_t         frame_length;

        error = utp_packet_view_next_frame(&view, &offset, &frame_type, &frame, &frame_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
        if (frame_type == UTP_FRAME_TYPE_SESSION_TOKEN && !has_token) {
            error     = utp_frame_session_token_decode(&session_token, frame, frame_length);
            has_token = error == UTP_INTERNAL_ERROR_OK;
        } else if (frame_type != UTP_FRAME_TYPE_STREAM) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    const uint64_t now_seconds = utp_context_now_seconds();
    if (!has_token || session_token.payload_length != UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE ||
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
    if (error != UTP_INTERNAL_ERROR_OK || encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_NONE) {
        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        return UTP_INTERNAL_ERROR_AUTH;
    }
    if (!utp_context_remember_zero_rtt_replay(context, session_token.payload + UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE,
                                              session_token.payload, view.header.packet_number, now_seconds,
                                              session_token.expires_at_seconds)) {
        utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
        return UTP_INTERNAL_ERROR_AUTH;
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
    utp_crypto_secure_clear(resumption_psk, sizeof(resumption_psk));
    slot->zero_rtt_expires_at_seconds = session_token.expires_at_seconds;
    slot->zero_rtt_encryption_mode    = encryption_mode;
    error = utp_connection_init(&slot->connection, UTP_CONNECTION_ROLE_PASSIVE, local_cid, view.header.scid, peer,
                                UTP_CONTEXT_PACKET_LIMIT, UINT16_MAX);
    if (error == UTP_INTERNAL_ERROR_OK) {
        slot->connection.context = context;
        utp_connection_set_mtu_config(&slot->connection, &context->mtu_config);
        error = utp_connection_set_stream_scheduler_mode(&slot->connection, (uint8_t)context->stream_scheduler_mode);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_context_register_connection_slot(context, slot);
    }
    const uint64_t now_us = utp_context_now_us();
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_connection_on_packet_in_received(&slot->connection, packet_in, peer, now_us);
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
    if (error == UTP_INTERNAL_ERROR_OK) {
        uint8_t payload[UTP_FRAME_VERSION_SIZE];
        size_t  payload_length;

        error = utp_connection_begin_zero_rtt_response(&slot->connection);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_context_encode_version_frame(payload, sizeof(payload), &payload_length);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_connection_queue_packet(&slot->connection, UTP_PACKET_TYPE_HANDSHAKE, payload, payload_length,
                                                true);
        }
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_context_release_connection_slot(context, slot);
        return error;
    }
    error = utp_context_flush_connection(context, slot);
    if (error == UTP_INTERNAL_ERROR_OK && utp_connection_is_connected(&slot->connection)) {
        error = utp_context_complete_connected_side_effects(context, slot);
    }
    if (error == UTP_INTERNAL_ERROR_OK && utp_connection_is_connected(&slot->connection)) {
        error = utp_context_flush_connection(context, slot);
    }
    return error;
}

static utp_internal_error_t utp_context_dispatch_packet(utp_context_t* context, uint8_t* packet, size_t packet_length,
                                                        utp_packet_in_t* packet_in, const utp_address_t* peer)
{
    utp_packet_header_t  header;
    utp_internal_error_t error = utp_proto_decode_header(&header, packet, packet_length);

    if (error != UTP_INTERNAL_ERROR_OK || packet_length != UTP_PACKET_HEADER_SIZE + header.payload_length ||
        header.packet_number == 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    utp_context_connection_slot_t* connection_slot = utp_context_find_connection_slot(context, header.dcid);
    if (connection_slot != NULL) {
        return utp_context_on_connection_packet(context, connection_slot, &header, packet, packet_length, packet_in,
                                                peer);
    }
    utp_context_pending_slot_t* pending_slot = utp_context_find_pending_slot(context, header.dcid);
    if (pending_slot != NULL) {
        return utp_context_on_pending_packet(context, pending_slot, packet, packet_length, packet_in, peer);
    }
    if (header.dcid == 0u && header.type == UTP_PACKET_TYPE_INITIAL) {
        utp_packet_view_t view;

        error = utp_packet_view_decode(&view, packet, packet_length);
        return error == UTP_INTERNAL_ERROR_OK ? utp_context_on_initial_packet(context, &view, peer) : error;
    }
    if (header.dcid == 0u && header.type == UTP_PACKET_TYPE_0RTT && packet_in != NULL) {
        return utp_context_on_zero_rtt_packet(context, packet_in, peer);
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
    utp_context_t*   context = user_data;
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    if ((events & UTP_EVENT_WRITABLE) == 0u || context == NULL) {
        return;
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
                const utp_address_t peer     = slot->connection.peer;
                const bool          zero_rtt = slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN ||
                                      slot->connect_attempt.type == UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE;

                --slot->connect_retries_remaining;
                error = zero_rtt ? utp_context_retry_zero_rtt_attempt(context, slot, now_us)
                                 : utp_context_start_connect_attempt(context, slot, &peer, now_us);
                if (error != UTP_INTERNAL_ERROR_OK && slot->used) {
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
    utp_hash_iter_t  iter;
    utp_hash_node_t* node;

    utp_hash_iter_init(&iter);
    while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
        utp_context_pending_slot_t* slot = utp_context_pending_slot_from_node(node);
        uint64_t                    deadline;

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
    if (out_context == NULL) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    *out_context = NULL;
    if (options == NULL || options->event_base == NULL || !utp_context_log_level_is_valid(options->log_level) ||
        options->stream_scheduler_mode > UTP_STREAM_SCHEDULER_DRR) {
        return UTP_STATUS_INVALID_ARGUMENT;
    }
    utp_context_t* context = utp_allocator_alloc(NULL, sizeof(*context));

    if (context == NULL) {
        return UTP_STATUS_NOMEM;
    }
    context->connections      = (utp_hash_table_t){0};
    context->pending_incoming = (utp_hash_table_t){0};
    context->zero_rtt_replay  = (utp_hash_table_t){0};
    TAILQ_INIT(&context->free_connection_slots);
    TAILQ_INIT(&context->free_pending_slots);
    utp_event_init(&context->udp_event);
    utp_event_init(&context->udp_write_event);
    utp_event_init(&context->timer_event);
    utp_udp_socket_init(&context->udp_socket);
    context->packet_in_pool.allocator              = NULL;
    context->packet_in_pool.packets                = NULL;
    context->packet_in_pool.storage                = NULL;
    context->packet_in_pool.packet_capacity        = 0u;
    context->packet_in_pool.buffer_capacity        = 0u;
    context->on_connected                          = NULL;
    context->on_connected_user_data                = NULL;
    context->on_connect_error                      = NULL;
    context->on_connect_error_user_data            = NULL;
    context->on_new_connection                     = NULL;
    context->on_new_connection_user_data           = NULL;
    context->on_connection_error                   = NULL;
    context->on_connection_error_user_data         = NULL;
    context->callback_accept_pending               = NULL;
    context->callback_accept_zero_rtt              = NULL;
    context->callback_accept_requested             = false;
    context->next_cid                              = (uint32_t)options->context_id;
    context->next_cid                              = context->next_cid == 0u ? 1u : context->next_cid;
    context->log_level                             = options->log_level;
    context->stream_scheduler_mode                 = options->stream_scheduler_mode;
    context->mtu_config.enabled                    = options->enable_dplpmtud;
    context->mtu_config.mtu_min                    = options->mtu_min;
    context->mtu_config.mtu_max                    = options->mtu_max;
    context->mtu_config.mtu_base                   = options->mtu_base;
    context->mtu_config.probe_interval_seconds     = options->mtu_probe_interval;
    context->mtu_config.probe_step                 = options->mtu_probe_step;
    context->mtu_config.probe_timeout_ms           = options->mtu_probe_timeout;
    context->mtu_config.probe_retries              = options->mtu_probe_retries;
    context->mtu_config.blackhole_loss_threshold   = options->mtu_blackhole_loss_threshold;
    context->mtu_config.blackhole_loss_window_ms   = options->mtu_blackhole_loss_window_ms;
    context->mtu_config.blackhole_cooldown_ms      = options->mtu_blackhole_cooldown_ms;
    context->zero_rtt_token_max_lifetime_seconds   = options->zero_rtt_token_max_lifetime_seconds;
    context->zero_rtt_replay_cache_capacity        = options->zero_rtt_replay_cache_capacity == 0u
                                                         ? UTP_CONTEXT_ZERO_RTT_REPLAY_DEFAULT_CAPACITY
                                                         : options->zero_rtt_replay_cache_capacity;
    context->resumption_key_explicit               = false;
    context->resumption_keys_ready                 = false;
    context->default_resumption_key_warning_logged = false;
    utp_crypto_default_resumption_key(context->resumption_root_key);
    utp_internal_error_t error = utp_hash_table_init(&context->connections, NULL, SIZE_MAX);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_hash_table_init(&context->pending_incoming, NULL, UTP_CONTEXT_MAX_PENDING_INCOMING);
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
    context->logger.sink = options->log_sink;
    char    fragment[32];
    int32_t fragment_length = snprintf(fragment, sizeof(fragment), "context %" PRIu64, options->context_id);
    if (fragment_length < 0 || (size_t)fragment_length >= sizeof(fragment)) {
        utp_hash_table_cleanup(&context->connections, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming, NULL, NULL);
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
        error = utp_packet_in_pool_init(&context->packet_in_pool, NULL, UTP_CONTEXT_PACKET_IN_LIMIT,
                                        UTP_CONTEXT_PACKET_IN_CAPACITY);
    }
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_internal_log_error(&context->logger, &context->tag, error, "context initialization failed");
        utp_packet_in_pool_cleanup(&context->packet_in_pool);
        utp_hash_table_cleanup(&context->connections, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming, NULL, NULL);
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
        utp_hash_iter_init(&iter);
        while ((node = utp_hash_iter_next(&context->connections, &iter)) != NULL) {
            utp_context_connection_slot_t* slot = utp_context_connection_slot_from_node(node);

            utp_context_send_destroy_close(context, slot);
            utp_context_release_connection_slot(context, slot);
        }
        utp_hash_iter_init(&iter);
        while ((node = utp_hash_iter_next(&context->pending_incoming, &iter)) != NULL) {
            utp_context_release_pending_slot(context, utp_context_pending_slot_from_node(node));
        }
        utp_hash_table_cleanup(&context->connections, NULL, NULL);
        utp_hash_table_cleanup(&context->pending_incoming, NULL, NULL);
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
    utp_context_log(context, UTP_LOG_LEVEL_INFO, "udp socket bound");
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

        if (slot->used) {
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
    if (options->early_data_size > UTP_CONTEXT_ZERO_RTT_EARLY_DATA_MAX) {
        return UTP_STATUS_OVERFLOW;
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
    // 加密 0-RTT 数据面尚未接入前不得把加密恢复状态降级为明文发送。
    if (encryption_mode != UTP_CRYPTO_ENCRYPTION_MODE_NONE) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return UTP_STATUS_UNSUPPORTED;
    }
    utp_address_t peer;

    error = utp_address_parse(&peer, options->address, options->port);
    if (error != UTP_INTERNAL_ERROR_OK) {
        utp_crypto_secure_clear(state_payload, state_payload_length);
        return utp_internal_error_to_status(error);
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

        if (slot->used) {
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
