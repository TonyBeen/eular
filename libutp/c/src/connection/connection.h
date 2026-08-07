#ifndef EULAR_UTP_CONNECTION_CONNECTION_H
#define EULAR_UTP_CONNECTION_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "congestion/bbr.h"
#include "connection/stream.h"
#include "context/ack_scheduler.h"
#include "context/send_control.h"
#include "crypto/crypto.h"
#include "mtu/mtu.h"
#include "proto/frame.h"
#include "proto/packet_in.h"
#include "socket/address.h"
#include "util/hash.h"
#include "util/receive_history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_CONNECTION_MAX_RECEIVE_RANGES             32u
#define UTP_CONNECTION_STREAM_TYPE_COUNT              2u
#define UTP_CONNECTION_RECV_REASSEMBLY_MEMORY_LIMIT   (16u * 1024u * 1024u)
#define UTP_CONNECTION_RECV_REASSEMBLY_FRAGMENT_LIMIT 4096u
#define UTP_CONNECTION_KEEPALIVE_INTERVAL_US          UINT64_C(30000000)
#define UTP_CONNECTION_KEEPALIVE_TIMEOUT_US           UINT64_C(1500000)
#define UTP_CONNECTION_KEEPALIVE_MAX_PROBES           3u
#define UTP_CONNECTION_SESSION_TOKEN_SIZE             64u

typedef enum utp_connection_role { UTP_CONNECTION_ROLE_ACTIVE = 0, UTP_CONNECTION_ROLE_PASSIVE } utp_connection_role_t;

typedef enum utp_connection_state {
    UTP_CONNECTION_STATE_NEW = 0,
    UTP_CONNECTION_STATE_INITIAL_SENT,
    UTP_CONNECTION_STATE_CONNECTED,
    UTP_CONNECTION_STATE_CLOSING,
    UTP_CONNECTION_STATE_DRAINING,
    UTP_CONNECTION_STATE_CLOSED
} utp_connection_state_t;

typedef enum utp_connection_path_state {
    UTP_CONNECTION_PATH_STATE_UNKNOWN = 0,
    UTP_CONNECTION_PATH_STATE_VALIDATED,
    UTP_CONNECTION_PATH_STATE_VALIDATING,
    UTP_CONNECTION_PATH_STATE_FAILED
} utp_connection_path_state_t;

typedef struct utp_connection_control_slot {
    utp_hash_node_t node;
    uint64_t        value;
    uint64_t        final_size;
    uint32_t        stream_id;
    uint32_t        generation;
    uint32_t        in_flight_generation;
    uint16_t        error_code;
    uint8_t         frame_type;
    bool            pending;
    bool            queued;
    bool            in_flight;
} utp_connection_control_slot_t;

typedef struct utp_connection_pending_max_stream_data {
    utp_hash_node_t node;
    uint64_t        value;
    uint32_t        stream_id;
} utp_connection_pending_max_stream_data_t;

// Connection 私有的传输状态；CID 解复用和 UDP I/O 由 Context 负责。
typedef struct utp_connection {
    struct utp_context*         context;
    utp_send_control_t          send_control;
    utp_receive_history_t       receive_history;
    utp_ack_scheduler_t         ack_scheduler;
    utp_packet_out_pool_t       packet_pool;
    utp_mtu_discovery_t         mtu_discovery;
    utp_bbr_t                   congestion;
    utp_crypto_key_pair_t       crypto_key_pair;
    utp_crypto_aead_t           tx_aead;
    utp_crypto_aead_t           rx_aead;
    utp_packet_out_t            close_packet;
    utp_hash_table_t            streams;
    utp_hash_table_t            control_slots;
    utp_hash_table_t            pending_peer_max_stream_data;
    utp_address_t               peer;
    utp_address_t               candidate_peer;
    uint32_t                    local_cid;
    uint32_t                    peer_cid;
    uint32_t                    next_stream_id[UTP_STREAM_TYPES];
    uint64_t                    peer_max_data;
    uint64_t                    local_max_data_advertised;
    uint64_t                    stream_data_sent_total;
    uint64_t                    local_stream_data_received_total;
    uint64_t                    local_stream_data_consumed_total;
    uint64_t                    last_max_data_sent_us;
    uint64_t                    last_data_blocked_sent_us;
    uint16_t                    packet_capacity;
    size_t                      recv_reassembly_memory_bytes;
    size_t                      recv_reassembly_fragment_count;
    uint64_t                    rx_bytes;
    uint64_t                    tx_bytes;
    uint64_t                    peer_handshake_packet_number;
    uint64_t                    retransmission_deadline_us;
    uint64_t                    close_deadline_us;
    uint64_t                    close_last_sent_us;
    uint64_t                    close_pto_us;
    uint64_t                    keepalive_deadline_us;
    uint64_t                    last_peer_activity_us;
    uint64_t                    path_challenge_deadline_us;
    uint64_t                    candidate_rx_bytes;
    uint64_t                    candidate_tx_bytes;
    uint64_t                    candidate_queued_bytes;
    uint32_t                    path_validation_generation;
    uint16_t                    close_error_code;
    uint16_t                    peer_close_error_code;
    uint16_t                    peer_close_reason_length;
    uint16_t                    local_max_streams[UTP_CONNECTION_STREAM_TYPE_COUNT];
    uint16_t                    peer_max_streams[UTP_CONNECTION_STREAM_TYPE_COUNT];
    uint8_t                     path_challenge[8];
    uint8_t                     peer_crypto_public_key[UTP_CRYPTO_X25519_KEY_SIZE];
    uint8_t                     session_token[UTP_CONNECTION_SESSION_TOKEN_SIZE];
    uint8_t                     close_packet_data[UTP_PACKET_HEADER_SIZE + UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE];
    uint8_t                     stream_scheduler_mode;
    uint8_t                     crypto_type;
    uint32_t                    stream_scheduler_cursor;
    uint8_t                     path_challenge_retry_count;
    uint16_t                    keepalive_missed_probes;
    uint16_t                    session_token_validity_seconds;
    uint8_t                     session_token_size;
    bool                        close_pending;
    bool                        udp_write_pending;
    bool                        local_close_started;
    bool                        peer_close_received;
    bool                        path_challenge_pending;
    bool                        crypto_configured;
    bool                        crypto_ready;
    bool                        session_token_issued;
    const uint8_t*              peer_close_reason;
    utp_connection_role_t       role;
    utp_connection_state_t      state;
    utp_connection_path_state_t path_state;
} utp_connection_t;

/** @brief 初始化连接运行状态及其有界发送、接收资源。 */
utp_internal_error_t utp_connection_init(utp_connection_t* connection, utp_connection_role_t role, uint32_t local_cid,
                                         uint32_t peer_cid, const utp_address_t* peer, size_t packet_limit,
                                         uint16_t packet_capacity);
/** @brief 释放连接持有的流、包、加密与计时资源。 */
void                 utp_connection_cleanup(utp_connection_t* connection);
/** @brief 应用 Context 的 MTU 配置，并重置连接级 MTU 运行状态。 */
void                 utp_connection_set_mtu_config(utp_connection_t* connection, const utp_mtu_config_t* config);
/** @brief 为主动连接生成密钥对并选择握手加密算法。 */
utp_internal_error_t utp_connection_configure_crypto(utp_connection_t* connection, uint8_t crypto_type);
/** @brief 将本端临时公钥编码为 CRYPTO 帧。 */
utp_internal_error_t utp_connection_encode_crypto(const utp_connection_t* connection, uint8_t* buffer, size_t capacity);
/** @brief 接管 pending 阶段派生的双向 AEAD 上下文，调用后清空 @p tx 和 @p rx。 */
utp_internal_error_t utp_connection_adopt_crypto(utp_connection_t* connection, uint8_t crypto_type,
                                                 utp_crypto_aead_t* tx, utp_crypto_aead_t* rx);
/** @brief 将 PacketOut 展平为可发送线上字节；加密包会在此执行 AEAD 封装。 */
utp_internal_error_t utp_connection_encode_packet_wire(const utp_connection_t* connection,
                                                       const utp_packet_out_t* packet, uint8_t* buffer, size_t capacity,
                                                       size_t* out_length);

/** @brief 构造完整明文包并放入有界发送队列。 */
utp_internal_error_t utp_connection_queue_packet(utp_connection_t* connection, uint8_t packet_type,
                                                 const uint8_t* payload, size_t payload_length, bool track_on_send);
/** @brief 排入单独成包的 CONNECTION_CLOSE，并关闭本端普通发送。 */
utp_internal_error_t utp_connection_queue_close(utp_connection_t* connection, uint16_t error_code);
/** @brief 为 Context 同步销毁构造专用 CONNECTION_CLOSE，不进入发送队列。 */
utp_internal_error_t utp_connection_prepare_destroy_close(utp_connection_t* connection);
/** @brief 返回当前可发送包，必要时为重传包分配新包号。 */
utp_packet_out_t*    utp_connection_next_packet_to_send(utp_connection_t* connection);
/** @brief 按给定时间和拥塞、pacing 状态选择可发送包。 */
utp_packet_out_t*    utp_connection_next_packet_to_send_at(utp_connection_t* connection, uint64_t now_us);
/** @brief 标记 PacketOut 已成功写入 UDP；无需跟踪的包会在此归还对象池。 */
utp_internal_error_t utp_connection_on_packet_sent(utp_connection_t* connection, utp_packet_out_t* packet,
                                                   uint64_t now_us);
/** @brief 处理 UDP 发送失败，决定重试、MTU 回退或本地关闭。 */
void                 utp_connection_on_packet_send_error(utp_connection_t* connection, const utp_packet_out_t* packet,
                                                         utp_internal_error_t error, uint64_t now_us);
/** @brief 判断给定 PacketOut 是否为本连接的 CONNECTION_CLOSE。 */
bool                 utp_connection_is_close_packet(const utp_connection_t* connection, const utp_packet_out_t* packet);
/** @brief 释放从未写入 UDP 的包，并恢复其可靠 control 和流发送状态。 */
void                 utp_connection_on_packet_abandoned(utp_connection_t* connection, const utp_packet_out_t* packet);
/** @brief 校验来源地址与 CID，解密并处理普通接收包。 */
utp_internal_error_t utp_connection_on_packet_received(utp_connection_t* connection, uint8_t* packet,
                                                       size_t packet_length, const utp_address_t* peer,
                                                       uint64_t now_us);
/** @brief 处理 PacketIn 承载的接收包，使流重组可借用包内数据。 */
utp_internal_error_t utp_connection_on_packet_in_received(utp_connection_t* connection, utp_packet_in_t* packet,
                                                          const utp_address_t* peer, uint64_t now_us);
/** @brief 处理已经在 pending 阶段完成解密的 PacketIn。 */
utp_internal_error_t utp_connection_on_plaintext_packet_in_received(utp_connection_t* connection,
                                                                    utp_packet_in_t* packet, size_t wire_packet_length,
                                                                    const utp_address_t* peer, uint64_t now_us);
/** @brief 将当前 ACK 接收历史编码并排入发送队列。 */
utp_internal_error_t utp_connection_queue_ack(utp_connection_t* connection, uint64_t now_us);
/** @brief 返回等待发送 ACK 的接收包数量。 */
uint32_t             utp_connection_ack_pending_count(const utp_connection_t* connection);
/** @brief 返回 ACK 调度器的下一截止时间，未安排时为零。 */
uint64_t             utp_connection_ack_deadline(const utp_connection_t* connection);
/** @brief 根据当前发送账本确保已设置重传定时器。 */
utp_internal_error_t utp_connection_ensure_retransmission_deadline(utp_connection_t* connection, uint64_t now_us);
/** @brief 处理重传定时器到期，检测丢失并重新排队可重传数据。 */
utp_internal_error_t utp_connection_on_retransmission_timeout(utp_connection_t* connection, uint64_t now_us);
/** @brief 返回发送控制模块的重传截止时间，未安排时为零。 */
uint64_t             utp_connection_retransmission_deadline(const utp_connection_t* connection);
/** @brief 返回本端关闭等待截止时间，未关闭时为零。 */
uint64_t             utp_connection_close_deadline(const utp_connection_t* connection);
/** @brief 返回保活探测截止时间，未启用时为零。 */
uint64_t             utp_connection_keepalive_deadline(const utp_connection_t* connection);
/** @brief 处理保活超时并排队 PING，连续失败时进入 draining。 */
utp_internal_error_t utp_connection_on_keepalive_timeout(utp_connection_t* connection, uint64_t now_us);
/** @brief 返回 MTU 探测的下一截止时间，未安排时为零。 */
uint64_t             utp_connection_mtu_deadline(const utp_connection_t* connection, uint64_t now_us);
/** @brief 返回 pacing 模块允许下次发送的时间。 */
uint64_t             utp_connection_pacing_deadline(const utp_connection_t* connection);
/** @brief 处理 MTU 探测定时器并按状态排队探测包。 */
utp_internal_error_t utp_connection_on_mtu_timeout(utp_connection_t* connection, uint64_t now_us);
/** @brief 返回候选路径验证截止时间，未验证时为零。 */
uint64_t             utp_connection_path_validation_deadline(const utp_connection_t* connection);
/** @brief 处理 PATH_CHALLENGE 超时，重试或回退到原路径。 */
utp_internal_error_t utp_connection_on_path_validation_timeout(utp_connection_t* connection, uint64_t now_us);
/** @brief 设置流调度模式，支持 Strict 和 DRR。 */
utp_internal_error_t utp_connection_set_stream_scheduler_mode(utp_connection_t* connection, uint8_t mode);
/** @brief 创建本端发起流，并返回其唯一 stream_id。 */
utp_internal_error_t utp_connection_create_stream_internal(utp_connection_t* connection, bool bidirectional,
                                                           uint32_t* out_stream_id);
/** @brief 按 stream_id 查找已存在流，不创建对端流。 */
utp_stream_t*        utp_connection_find_stream_internal(utp_connection_t* connection, uint32_t stream_id);

/** @brief 返回当前连接状态；空指针视为 CLOSED。 */
utp_connection_state_t utp_connection_state(const utp_connection_t* connection);
/** @brief 判断连接是否处于可读写的 CONNECTED 状态。 */
bool                   utp_connection_is_connected(const utp_connection_t* connection);
/** @brief 导出连接缓存的会话票据。 */
utp_internal_error_t   utp_connection_export_session_token_internal(const utp_connection_t* connection, uint8_t* buffer,
                                                                    size_t capacity, size_t* out_length);
/** @brief 为 0-RTT 首个双向流建立本地发送状态，避免后续重用 stream_id 0。 */
utp_internal_error_t utp_connection_reserve_zero_rtt_stream(utp_connection_t* connection, size_t data_length, bool fin);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONNECTION_CONNECTION_H
