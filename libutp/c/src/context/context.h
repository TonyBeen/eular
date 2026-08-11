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
#define UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE         32u
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
    utp_packet_in_t*           zero_rtt_early_packet;
    size_t                     zero_rtt_early_wire_size;
    uint64_t                   zero_rtt_request_packet_number;
    uint64_t                   zero_rtt_request_received_us;
    uint64_t                   zero_rtt_response_deadline_us;
    uint64_t                   zero_rtt_expire_deadline_us;
    uint64_t                   zero_rtt_amplification_rx_bytes;
    uint64_t                   zero_rtt_amplification_tx_bytes;
    uint8_t                    zero_rtt_session_token[UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE];
    uint8_t                    zero_rtt_resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE];
    uint8_t                    zero_rtt_early_data[UTP_CONTEXT_ZERO_RTT_EARLY_DATA_MAX];
    size_t                     zero_rtt_early_data_size;
    uint64_t                   zero_rtt_expires_at_seconds;
    int8_t                     connect_retries_remaining;
    uint8_t                    zero_rtt_response_retries;
    uint8_t                    zero_rtt_encryption_mode;
    bool                       zero_rtt_early_fin;
    bool                       zero_rtt_awaiting_accept;
    bool                       zero_rtt_accepted;
    bool                       zero_rtt_response_active;
    bool                       zero_rtt_response_queued;
    bool                       zero_rtt_response_sent;
    bool                       zero_rtt_early_delivered;
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
    // 握手构造和最终 UDP 加密串行复用该缓冲，排入 PacketOut 后不再引用其中数据。
    uint8_t                                  encrypt_send_buffer[UINT16_MAX];
    utp_hash_table_t                         connections;
    struct utp_context_connection_slot_tailq free_connection_slots;
    utp_hash_table_t                         pending_incoming;
    struct utp_context_pending_slot_tailq    free_pending_slots;
    uint32_t                                 next_cid;
    utp_stream_scheduler_mode_t              stream_scheduler_mode;
    utp_congestion_algorithm_t               cc_algorithm;
    uint32_t                                 clock_granularity_us;
    utp_bbr_config_t                         bbr_config;
    utp_cubic_config_t                       cubic_config;
    utp_mtu_config_t                         mtu_config;
    uint8_t                                  resumption_root_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE];
    utp_crypto_resumption_keys_t             resumption_keys;
    utp_hash_table_t                         zero_rtt_replay;
    uint32_t                                 zero_rtt_token_max_lifetime_seconds;
    uint32_t                                 zero_rtt_replay_cache_capacity;
    utp_frame_transport_params_t             local_transport_params;
    utp_frame_ack_frequency_t                local_ack_frequency;
    uint32_t                                 keepalive_interval_ms;
    uint32_t                                 keepalive_timeout_ms;
    uint16_t                                 handshake_timeout_ms;
    uint16_t                                 keepalive_probes;
    uint8_t                                  handshake_max_retries;
    utp_context_pending_slot_t*              callback_accept_pending;
    utp_context_connection_slot_t*           callback_accept_zero_rtt;
    bool                                     callback_accept_requested;
    bool                                     resumption_key_explicit;
    bool                                     resumption_keys_ready;
    bool                                     default_resumption_key_warning_logged;
    bool                                     enable_keepalive;
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
