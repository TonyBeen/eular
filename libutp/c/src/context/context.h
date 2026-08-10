#ifndef EULAR_UTP_CONTEXT_CONTEXT_H
#define EULAR_UTP_CONTEXT_CONTEXT_H

#include <utp/context.h>

#include "connection/connection.h"
#include "context/event_loop.h"
#include "context/pending_incoming.h"
#include "mtu/mtu.h"
#include "proto/packet_in.h"
#include "socket/udp.h"
#include "util/hash.h"
#include "util/log.h"

#define UTP_CONTEXT_MAX_PENDING_INCOMING             1024u
#define UTP_CONTEXT_PACKET_LIMIT                     32u
#define UTP_CONTEXT_PACKET_IN_LIMIT                  64u
#define UTP_CONTEXT_PACKET_IN_CAPACITY               65535u
#define UTP_CONTEXT_PENDING_PACKET_LIMIT             16u
#define UTP_CONTEXT_PENDING_STORAGE_CAPACITY         32768u
#define UTP_CONTEXT_ZERO_RTT_REPLAY_DEFAULT_CAPACITY 4096u
#define UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE         40u
#define UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE \
    (UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE + UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE)
#define UTP_CONTEXT_ZERO_RTT_EARLY_DATA_MAX                                                \
    (UTP_PACKET_MTU_FLOOR - UTP_PACKET_HEADER_SIZE - UTP_FRAME_SESSION_TOKEN_HEADER_SIZE - \
     UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE - UTP_FRAME_STREAM_HEADER_SIZE)

typedef struct utp_context_zero_rtt_replay_entry {
    utp_hash_node_t node;
    uint64_t        expires_at_seconds;
    uint8_t         key[UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE];
} utp_context_zero_rtt_replay_entry_t;

typedef struct utp_context_connection_slot {
    utp_hash_node_t node;
    TAILQ_ENTRY(utp_context_connection_slot) free_next;
    utp_connection_t           connection;
    utp_connect_attempt_info_t connect_attempt;
    uint64_t                   connect_deadline_us;
    uint8_t                    zero_rtt_session_token[UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE];
    uint8_t                    zero_rtt_resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE];
    uint8_t                    zero_rtt_early_data[UTP_CONTEXT_ZERO_RTT_EARLY_DATA_MAX];
    size_t                     zero_rtt_early_data_size;
    uint64_t                   zero_rtt_expires_at_seconds;
    int8_t                     connect_retries_remaining;
    uint8_t                    zero_rtt_encryption_mode;
    bool                       zero_rtt_early_fin;
    bool                       zero_rtt_awaiting_accept;
    bool                       zero_rtt_accepted;
    bool                       used;
    bool                       connected_reported;
    bool                       connection_error_reported;
    bool                       connect_pending;
} utp_context_connection_slot_t;
TAILQ_HEAD(utp_context_connection_slot_tailq, utp_context_connection_slot);

typedef struct utp_context_pending_slot {
    utp_hash_node_t node;
    TAILQ_ENTRY(utp_context_pending_slot) free_next;
    utp_pending_incoming_t pending;
    uint8_t                storage[UTP_CONTEXT_PENDING_STORAGE_CAPACITY];
    bool                   used;
    bool                   queued;
} utp_context_pending_slot_t;
TAILQ_HEAD(utp_context_pending_slot_tailq, utp_context_pending_slot);

struct utp_context {
    utp_event_loop_t                         event_loop;
    utp_event_t                              udp_event;
    utp_event_t                              udp_write_event;
    utp_event_t                              timer_event;
    utp_udp_socket_t                         udp_socket;
    utp_packet_in_pool_t                     packet_in_pool;
    // 加密只在最终 UDP 写入前进行，单个 Context 的事件循环串行复用该缓冲。
    uint8_t                                  encrypt_send_buffer[UINT16_MAX];
    utp_hash_table_t                         connections;
    struct utp_context_connection_slot_tailq free_connection_slots;
    utp_hash_table_t                         pending_incoming;
    struct utp_context_pending_slot_tailq    free_pending_slots;
    uint32_t                                 next_cid;
    utp_stream_scheduler_mode_t              stream_scheduler_mode;
    utp_mtu_config_t                         mtu_config;
    uint8_t                                  resumption_root_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE];
    utp_crypto_resumption_keys_t             resumption_keys;
    utp_hash_table_t                         zero_rtt_replay;
    uint32_t                                 zero_rtt_token_max_lifetime_seconds;
    uint32_t                                 zero_rtt_replay_cache_capacity;
    utp_context_pending_slot_t*              callback_accept_pending;
    utp_context_connection_slot_t*           callback_accept_zero_rtt;
    bool                                     callback_accept_requested;
    bool                                     resumption_key_explicit;
    bool                                     resumption_keys_ready;
    bool                                     default_resumption_key_warning_logged;
    utp_on_connected_fn                      on_connected;
    void*                                    on_connected_user_data;
    utp_on_connect_error_fn                  on_connect_error;
    void*                                    on_connect_error_user_data;
    utp_on_new_connection_fn                 on_new_connection;
    void*                                    on_new_connection_user_data;
    utp_on_connection_error_fn               on_connection_error;
    void*                                    on_connection_error_user_data;
    utp_logger_t                             logger;
    utp_log_tag_t                            tag;
};

utp_internal_error_t utp_context_flush_public_connection(utp_context_t* context, utp_connection_t* connection);

#endif  // EULAR_UTP_CONTEXT_CONTEXT_H
