#ifndef EULAR_UTP_CONTEXT_PENDING_INCOMING_H
#define EULAR_UTP_CONTEXT_PENDING_INCOMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/crypto.h"
#include "proto/frame.h"
#include "socket/address.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum utp_pending_incoming_result {
    UTP_PENDING_INCOMING_BUFFERED = 0,
    UTP_PENDING_INCOMING_PROMOTE
} utp_pending_incoming_result_t;

typedef utp_internal_error_t (*utp_pending_incoming_replay_fn)(const uint8_t* packet, size_t packet_length,
                                                               size_t wire_packet_length, const utp_address_t* peer,
                                                               const utp_address_t* local, void* user_data);

#define UTP_PENDING_HANDSHAKE_OUTPUT_CAPACITY 9u

typedef struct utp_pending_handshake_path {
    utp_address_t peer;
    utp_address_t local;
    uint64_t      initial_packet_number;
    uint64_t      initial_received_us;
    bool          timeout_retransmission;
} utp_pending_handshake_path_t;

typedef struct utp_pending_packet_path {
    utp_address_t peer;
    utp_address_t local;
} utp_pending_packet_path_t;

// storage 由 Context 持有并在创建 pending 项时传入。每个缓存包保存明文长度、线上长度和解密后的完整包，
// 因此接收热路径不需要动态分配，晋升后也能按实际 UDP 字节数记账。
typedef struct utp_pending_incoming {
    utp_address_t                peer;                           // 发起方地址
    utp_address_t                local;                          // 收包目的本地地址
    uint8_t*                     storage;                        // Context 提供的缓存存储，不拥有
    size_t                       storage_capacity;               // 缓存存储容量
    size_t                       storage_length;                 // 已缓存字节数
    size_t                       packet_limit;                   // 缓存包数上限
    size_t                       packet_count;                   // 已缓存包数
    uint32_t                     local_cid;                      // 待晋升连接的本端 CID
    uint32_t                     peer_cid;                       // 发起方 CID
    uint64_t                     first_handshake_packet_number;  // 首个响应握手包号
    uint64_t                     last_handshake_packet_number;   // 最近响应握手包号
    uint64_t                     next_packet_number;             // 晋升后下一发送包号
    uint64_t                     latest_initial_packet_number;   // 最新 Initial 包号
    uint64_t                     latest_initial_received_us;     // 最新 Initial 接收时刻
    utp_address_t                latest_initial_peer;            // 最新 Initial 来源地址
    utp_address_t                latest_initial_local;           // 最新 Initial 本地接收地址
    utp_pending_handshake_path_t handshake_outputs[UTP_PENDING_HANDSHAKE_OUTPUT_CAPACITY];
    utp_pending_packet_path_t    packet_paths[UTP_PENDING_HANDSHAKE_OUTPUT_CAPACITY];
    uint64_t                     last_handshake_sent_us;                // 最近 HANDSHAKE 发送时刻
    uint64_t                     handshake_rtt_sample_us;               // 被动握手 RTT 样本
    uint64_t                     handshake_retransmission_deadline_us;  // 握手重传截止时刻
    uint64_t                     handshake_base_delay_us;               // 首次握手处理延迟
    uint32_t                     handshake_retransmission_count;        // 已执行握手重传次数
    utp_crypto_key_pair_t        crypto_key_pair;                       // 服务端临时密钥对
    utp_crypto_aead_t            tx_aead;                               // 晋升后发送 AEAD
    utp_crypto_aead_t            rx_aead;                               // 晋升后接收 AEAD
    uint8_t                      crypto_type;                           // 协商加密类型
    uint8_t                      handshake_max_retries;                 // 最大握手重试次数
    uint8_t                      handshake_output_count;                // 等待 UDP writable 的路径快照数
    uint8_t                      packet_path_count;                     // 缓存入站包使用的路径数
    utp_frame_transport_params_t peer_transport_params;                 // 对端传输参数
    utp_frame_ack_frequency_t    peer_ack_frequency;                    // 对端 ACK 策略
    bool                         crypto_configured : 1;                 // 是否请求加密握手
    bool                         crypto_ready : 1;                      // AEAD 是否已完成派生
    bool                         accepted : 1;                          // 应用是否已接受连接
    bool                         handshake_sent : 1;                    // 是否至少发送过一次响应
    bool                         peer_transport_params_received : 1;    // 是否收齐对端传输参数
    bool                         peer_ack_frequency_received : 1;       // 是否收齐对端 ACK 策略
} utp_pending_incoming_t;

utp_internal_error_t utp_pending_incoming_init(utp_pending_incoming_t* pending, uint32_t local_cid, uint32_t peer_cid,
                                               const utp_address_t* peer, uint8_t* storage, size_t storage_capacity,
                                               size_t packet_limit);
void                 utp_pending_incoming_set_local(utp_pending_incoming_t* pending, const utp_address_t* local);
void                 utp_pending_incoming_reset(utp_pending_incoming_t* pending);
utp_internal_error_t utp_pending_incoming_accept(utp_pending_incoming_t* pending);
/** @brief 设置被动握手响应的基础超时和最大重传次数。 */
utp_internal_error_t utp_pending_incoming_set_handshake_policy(utp_pending_incoming_t* pending, uint16_t timeout_ms,
                                                               uint8_t max_retries);
/** @brief 记录最新有效 Initial 的包号、来源路径与接收时刻；旧包不会回退快照。 */
utp_internal_error_t utp_pending_incoming_record_initial(utp_pending_incoming_t* pending, const utp_address_t* peer,
                                                         const utp_address_t* local, uint64_t packet_number,
                                                         uint64_t received_at_us);
utp_internal_error_t utp_pending_incoming_configure_crypto(utp_pending_incoming_t*   pending,
                                                           const utp_frame_crypto_t* peer_crypto);
utp_internal_error_t utp_pending_incoming_encode_crypto(const utp_pending_incoming_t* pending, uint8_t* buffer,
                                                        size_t capacity);
utp_internal_error_t utp_pending_incoming_decrypt_packet(const utp_pending_incoming_t* pending, uint8_t* packet,
                                                         size_t* packet_length);
utp_internal_error_t utp_pending_incoming_mark_handshake_sent(utp_pending_incoming_t* pending,
                                                              uint64_t handshake_packet_number, uint64_t now_us,
                                                              bool timeout_retransmission);
uint64_t             utp_pending_incoming_handshake_deadline(const utp_pending_incoming_t* pending);
utp_internal_error_t utp_pending_incoming_on_packet(utp_pending_incoming_t* pending, const uint8_t* packet,
                                                    size_t packet_length, size_t wire_packet_length,
                                                    const utp_address_t* peer, const utp_address_t* local,
                                                    uint64_t received_at_us, utp_pending_incoming_result_t* result);
utp_internal_error_t utp_pending_incoming_replay(const utp_pending_incoming_t*  pending,
                                                 utp_pending_incoming_replay_fn replay, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONTEXT_PENDING_INCOMING_H
