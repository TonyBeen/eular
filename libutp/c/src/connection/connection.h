#ifndef EULAR_UTP_CONNECTION_CONNECTION_H
#define EULAR_UTP_CONNECTION_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/connection.h>
#include <utp/option.h>

#include "congestion/bbr.h"
#include "congestion/cubic.h"
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

#define UTP_CONNECTION_MAX_RECEIVE_RANGES               32u
#define UTP_CONNECTION_STREAM_TYPE_COUNT                2u
#define UTP_CONNECTION_RECV_REASSEMBLY_MEMORY_LIMIT     (16u * 1024u * 1024u)
#define UTP_CONNECTION_RECV_REASSEMBLY_FRAGMENT_LIMIT   4096u
#define UTP_CONNECTION_KEEPALIVE_INTERVAL_US            UINT64_C(30000000)
#define UTP_CONNECTION_KEEPALIVE_TIMEOUT_US             UINT64_C(1500000)
#define UTP_CONNECTION_KEEPALIVE_MAX_PROBES             3u
#define UTP_CONNECTION_SESSION_TOKEN_SIZE               UTP_CRYPTO_LOCAL_RESUMPTION_STATE_MAX_SIZE
#define UTP_CONNECTION_STREAM_TERMINAL_DEFAULT_CAPACITY 4096u
#define UTP_CONNECTION_STREAM_TERMINAL_INITIAL_CAPACITY 8u
#define UTP_CONNECTION_PATH_VALIDATION_BUFFER_CAPACITY  (16u * 1024u)

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

typedef enum utp_connection_ack_profile {
    UTP_CONNECTION_ACK_PROFILE_STABLE = 0,
    UTP_CONNECTION_ACK_PROFILE_LATENCY_SENSITIVE,
    UTP_CONNECTION_ACK_PROFILE_LOSSY,
} utp_connection_ack_profile_t;

typedef struct utp_connection_control_slot {
    utp_hash_node_t node;                  // 按控制帧语义索引的哈希节点
    uint64_t        value;                 // 控制帧携带的额度或限制值
    uint64_t        final_size;            // RESET_STREAM 最终偏移
    uint32_t        stream_id;             // 关联流 ID
    uint32_t        generation;            // 语义值更新代次
    uint32_t        in_flight_generation;  // 当前飞行帧代次
    uint16_t        error_code;            // RESET_STREAM 或 STOP_SENDING 错误码
    uint8_t         frame_type;            // 控制帧类型
    bool            pending : 1;           // 是否待构造发送
    bool            queued : 1;            // 是否已有排队包承载本代次
    bool            in_flight : 1;         // 是否已有飞行包承载本代次
} utp_connection_control_slot_t;

typedef struct utp_connection_pending_max_stream_data {
    utp_hash_node_t node;       // 按流 ID 索引的哈希节点
    uint64_t        value;      // 暂存的对端流发送额度
    uint32_t        stream_id;  // 尚未创建的本端流 ID
} utp_connection_pending_max_stream_data_t;

typedef struct utp_connection_stream_terminal {
    utp_hash_node_t                        node;                       // 按流 ID 索引的终态节点
    struct utp_connection_stream_terminal* older;                      // LRU 中更旧的记录
    struct utp_connection_stream_terminal* newer;                      // LRU 中更新的记录
    uint64_t                               peer_final_size;            // 对端最终偏移
    uint32_t                               stream_id;                  // 已退休流 ID
    bool                                   peer_final_size_known : 1;  // 是否可校验迟到终止帧
    bool                                   peer_reset : 1;             // 对端是否以 RESET 结束写方向
    bool                                   local_write_reset : 1;      // 本地写方向是否以 RESET 结束
    bool                                   stop_sending_received : 1;  // 是否处理过对端 STOP_SENDING
} utp_connection_stream_terminal_t;

typedef struct utp_terminal_block {
    struct utp_terminal_block*        next;      // 更早分配的终态槽位块
    utp_connection_stream_terminal_t* slots;     // 本块终态槽位数组所有权
    uint32_t                          capacity;  // 本块槽位总数
    uint32_t                          count;     // 本块已使用槽位数
} utp_terminal_block_t;

typedef struct utp_connection_candidate_packet {
    struct utp_connection_candidate_packet* next;            // 下一条候选路径缓存报文
    utp_packet_in_t*                        packet;          // 已解密 PacketIn，持有一个引用
    uint64_t                                received_at_us;  // 原始接收时刻
    size_t                                  wire_size;       // 解密前 UDP 数据报长度
} utp_connection_candidate_packet_t;

typedef utp_on_session_token_ready_fn utp_session_token_cb_t;

// Connection 私有的传输状态；CID 解复用和 UDP I/O 由 Context 负责。
typedef struct utp_connection {
    struct utp_context*                context;                                   // 所属 Context，不拥有
    utp_on_incoming_stream_fn          on_incoming_stream;                        // 对端新流回调
    void*                              on_incoming_stream_user_data;              // 新流回调用户数据
    utp_session_token_cb_t             session_token_cb;                          // 恢复票据就绪回调
    void*                              session_token_cb_data;                     // 恢复票据回调用户数据
    utp_send_control_t                 send_control;                              // 发送、确认和重传状态
    utp_receive_history_t              receive_history;                           // 已认证接收包号历史
    utp_ack_scheduler_t                ack_scheduler;                             // ACK 调度状态
    utp_packet_out_pool_t              packet_pool;                               // 有界 PacketOut 对象池
    utp_mtu_discovery_t                mtu_discovery;                             // 路径 MTU 发现状态
    utp_bbr_t                          bbr_congestion;                            // BBR 算法状态
    utp_cubic_t                        cubic_congestion;                          // CUBIC 算法状态
    utp_crypto_key_pair_t              crypto_key_pair;                           // 本端握手临时密钥对
    utp_crypto_aead_t                  tx_aead;                                   // 1-RTT 发送 AEAD
    utp_crypto_aead_t                  rx_aead;                                   // 1-RTT 接收 AEAD
    utp_crypto_aead_t                  early_tx_aead;                             // 0-RTT 发送 AEAD
    utp_crypto_aead_t                  early_rx_aead;                             // 0-RTT 接收 AEAD
    utp_packet_out_t                   close_packet;                              // 专用 CONNECTION_CLOSE 包
    utp_hash_table_t                   streams;                                   // 所有存活或待回收流
    utp_hash_table_t                   control_slots;                             // 合并可靠控制帧槽位
    utp_hash_table_t                   pending_peer_max_stream_data;              // 未创建流的额度缓存
    utp_hash_table_t                   stream_terminals;                          // 已回收流的有界终态索引
    utp_terminal_block_t*              terminal_blocks;                           // 终态槽位分块所有权
    utp_terminal_block_t*              terminal_current_block;                    // 当前可分配终态槽位块
    utp_connection_candidate_packet_t* candidate_packet_head;                     // 候选路径缓存 FIFO 队首
    utp_connection_candidate_packet_t* candidate_packet_tail;                     // 候选路径缓存 FIFO 队尾
    utp_connection_stream_terminal_t*  stream_terminal_oldest;                    // LRU 最旧终态
    utp_connection_stream_terminal_t*  stream_terminal_newest;                    // LRU 最新终态
    utp_address_t                      peer;                                      // 当前已验证对端地址
    utp_address_t                      candidate_peer;                            // 正在验证的候选地址
    uint32_t                           local_cid;                                 // 本端连接 ID
    uint32_t                           peer_cid;                                  // 对端连接 ID
    uint32_t                           next_stream_id[UTP_STREAM_TYPES];          // 各流类型下一个本端 ID
    uint32_t                           stream_terminal_capacity;                  // 终态槽位配置上限
    uint32_t                           stream_terminal_allocated;                 // 已实际分配终态槽位数
    uint32_t                           stream_terminal_count;                     // 已用终态槽位数
    uint32_t                           path_validation_buffer_capacity;           // 候选路径缓存最大字节数
    uint64_t                           peer_max_data;                             // 对端通告的连接级发送额度
    uint64_t                           peer_initial_max_stream_data_bidi_local;   // 本端双向流发送额度
    uint64_t                           peer_initial_max_stream_data_bidi_remote;  // 对端双向流发送额度
    utp_frame_transport_params_t       local_transport_params;                    // 本端握手通告传输参数
    utp_frame_ack_frequency_t          local_ack_frequency;                       // 本端握手通告 ACK 策略
    uint64_t                           local_max_data_advertised;                 // 本端通告连接接收额度
    uint64_t                           stream_data_sent_total;                    // 已排队发送的流数据偏移总额
    uint64_t                           local_stream_data_received_total;          // 接收流数据最大偏移累计
    uint64_t                           local_stream_data_consumed_total;          // 应用已消费流数据累计
    uint64_t                           last_max_data_sent_us;                     // 最近 MAX_DATA 发送时刻
    uint64_t                           last_data_blocked_sent_us;                 // 最近 DATA_BLOCKED 发送时刻
    uint16_t                           packet_capacity;                           // PacketOut 初始包容量
    size_t                             recv_reassembly_memory_bytes;              // 所有流重组内存计费
    size_t                             recv_reassembly_fragment_count;            // 所有流重组分片计数
    size_t                             candidate_packet_bytes;                    // 候选路径缓存已占用线长
    uint64_t                           rx_bytes;                                  // 已认证且非重复接收字节数
    uint64_t                           tx_bytes;                                  // 实际发送字节数
    uint64_t                           rtx_bytes;                                 // 实际重传字节数
    uint64_t                           scheduler_select_total;                    // 流调度总选择次数
    uint64_t                           scheduler_select_strict;                   // Strict 选择次数
    uint64_t                           scheduler_select_drr;                      // DRR 选择次数
    uint64_t                           scheduler_strict_aging_promoted;           // Strict 老化提升次数
    uint64_t                           scheduler_mode_switches;                   // 调度模式切换次数
    uint64_t                           scheduler_drr_refills;                     // DRR 配额补充次数
    uint64_t                           scheduler_drr_consumes;                    // DRR 配额消耗次数
    uint64_t                           peer_handshake_packet_number;              // 已接收对端握手包号
    uint64_t                           peer_handshake_received_us;                // 对端握手接收时刻
    uint64_t                           retransmission_deadline_us;                // 普通数据重传截止时刻
    uint64_t                           close_deadline_us;                         // draining 结束时刻
    uint64_t                           close_last_sent_us;                        // 最近 CLOSE 发送时刻
    uint64_t                           close_pto_us;                              // CLOSE 重发 PTO
    uint64_t                           keepalive_deadline_us;                     // 保活探测截止时刻
    uint64_t                           last_peer_activity_us;                     // 最近有效对端活动时刻
    uint64_t                           path_challenge_deadline_us;                // 路径验证超时截止时刻
    uint64_t                           ack_profile_candidate_since_us;            // 候选 ACK 策略起始时刻
    uint64_t                           ack_profile_last_sent_us;                  // 最近 ACK_FREQUENCY 发送时刻
    uint64_t                           ack_profile_baseline_srtt_us;              // 策略评估 RTT 基线
    uint64_t                           ack_loss_window_start_us;                  // ACK 丢失窗口开始时刻
    uint64_t                           last_ack_frequency_apply_us;               // 最近应用对端 ACK 策略时刻
    uint64_t                           candidate_rx_bytes;                        // 候选路径已认证接收字节数
    uint64_t                           candidate_tx_bytes;                        // 候选路径已实际发送字节数
    uint64_t                           candidate_queued_bytes;                    // 候选路径已排队字节数
    uint32_t                           path_validation_generation;                // 当前路径验证代次
    uint16_t                           close_error_code;                          // 本端关闭错误码
    uint16_t                           peer_close_error_code;                     // 对端关闭错误码
    uint16_t                           peer_close_reason_length;                  // 对端关闭原因长度
    uint32_t                           keepalive_interval_ms;                     // 保活间隔
    uint32_t                           keepalive_timeout_ms;                      // 单次保活超时
    uint16_t                           local_max_streams[UTP_CONNECTION_STREAM_TYPE_COUNT];  // 本端允许对端创建流数
    uint16_t                           peer_max_streams[UTP_CONNECTION_STREAM_TYPE_COUNT];   // 对端允许本端创建流数
    uint8_t                            path_challenge[8];                                    // 当前路径挑战随机值
    uint8_t                            peer_crypto_public_key[UTP_CRYPTO_X25519_KEY_SIZE];   // 对端临时公钥
    uint8_t                            session_token[UTP_CONNECTION_SESSION_TOKEN_SIZE];     // 导出给客户端的恢复状态
    uint8_t close_packet_data[UTP_PACKET_HEADER_SIZE + UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE];  // CLOSE 内联缓冲
    uint8_t stream_scheduler_mode;                                    // Strict 或 DRR 调度模式
    utp_congestion_algorithm_t   congestion_algorithm;                // 当前拥塞控制算法
    uint8_t                      crypto_type;                         // 协商的加密套件
    uint8_t                      peer_ack_delay_exponent;             // 对端 ACK 延迟指数
    utp_frame_transport_params_t peer_transport_params;               // 已解析对端传输参数
    utp_frame_ack_frequency_t    peer_ack_frequency;                  // 已解析对端 ACK 策略
    uint32_t                     stream_scheduler_cursor;             // 流调度轮转游标
    uint8_t                      path_challenge_retry_count;          // 路径挑战已重试次数
    uint16_t                     keepalive_missed_probes;             // 连续未响应保活数
    uint16_t                     keepalive_probes;                    // 允许连续未响应探测数
    uint32_t                     ack_loss_count;                      // ACK 策略窗口中的丢失计数
    uint8_t                      ack_profile_current;                 // 当前 ACK 策略档位
    uint8_t                      ack_profile_candidate;               // 候选 ACK 策略档位
    uint64_t                     session_token_expires_at_seconds;    // 恢复状态绝对过期时间
    uint16_t                     session_token_size;                  // 当前恢复状态长度
    bool                         close_pending : 1;                   // CLOSE 包是否待发送
    bool                         udp_write_pending : 1;               // socket 写事件是否已注册
    bool                         local_close_started : 1;             // 是否已由本端开始关闭
    bool                         peer_close_received : 1;             // 是否已接收对端 CLOSE
    bool                         path_challenge_pending : 1;          // 路径挑战包是否正在飞行
    bool                         crypto_configured : 1;               // 是否配置加密
    bool                         crypto_ready : 1;                    // 1-RTT AEAD 是否就绪
    bool                         zero_rtt_encrypted : 1;              // 当前 0-RTT 是否使用 early AEAD
    bool                         session_token_issued : 1;            // 是否已向对端签发恢复票据
    bool                         peer_transport_params_received : 1;  // 是否已收到对端传输参数
    bool                         peer_ack_frequency_received : 1;     // 是否已收到对端 ACK 策略
    bool                         keepalive_enabled : 1;               // 是否启用保活
    const uint8_t*               peer_close_reason;                   // 对端 CLOSE 原包原因视图
    utp_connection_role_t        role;                                // 主动或被动角色
    utp_connection_state_t       state;                               // 连接生命周期状态
    utp_connection_path_state_t  path_state;                          // 路径验证状态
} utp_connection_t;

/** @brief 初始化连接运行状态及其有界发送、接收资源。 */
utp_internal_error_t utp_connection_init(utp_connection_t* connection, utp_connection_role_t role, uint32_t local_cid,
                                         uint32_t peer_cid, const utp_address_t* peer, size_t packet_limit,
                                         uint16_t packet_capacity);
/** @brief 释放连接持有的流、包、加密与计时资源。 */
void                 utp_connection_cleanup(utp_connection_t* connection);
/** @brief 应用 Context 的 MTU 配置，并重置连接级 MTU 运行状态。 */
void                 utp_connection_set_mtu_config(utp_connection_t* connection, const utp_mtu_config_t* config);
/** @brief 设置有界流终态表容量；仅允许在创建流之前调用。 */
utp_internal_error_t utp_connection_set_stream_terminal_capacity(utp_connection_t* connection, uint32_t capacity);
void utp_connection_set_path_validation_buffer_capacity(utp_connection_t* connection, uint32_t capacity);
/** @brief 重置并选择连接使用的拥塞控制算法；仅允许在任何数据包入队前调用。 */
utp_internal_error_t utp_connection_set_congestion_algorithm(utp_connection_t*          connection,
                                                             utp_congestion_algorithm_t algorithm,
                                                             const utp_bbr_config_t*    bbr_config,
                                                             const utp_cubic_config_t*  cubic_config,
                                                             uint32_t                   clock_granularity_us);
/** @brief 设置本端协商参数和保活策略；仅允许在尚未创建流时调用。 */
utp_internal_error_t utp_connection_set_local_transport_config(utp_connection_t*                   connection,
                                                               const utp_frame_transport_params_t* params,
                                                               const utp_frame_ack_frequency_t*    frequency,
                                                               bool enable_keepalive, uint32_t keepalive_interval_ms,
                                                               uint32_t keepalive_timeout_ms,
                                                               uint16_t keepalive_probes);
/** @brief 编码本端传输参数。 */
utp_internal_error_t utp_connection_encode_transport_params(const utp_connection_t* connection, uint8_t* buffer,
                                                            size_t capacity);
/** @brief 编码本端初始 ACK 调度偏好。 */
utp_internal_error_t utp_connection_encode_ack_frequency(const utp_connection_t* connection, uint8_t* buffer,
                                                         size_t capacity);
/** @brief 应用对端握手传输参数；重复参数必须与首次接收内容完全一致。 */
utp_internal_error_t utp_connection_apply_peer_transport_params(utp_connection_t*                   connection,
                                                                const utp_frame_transport_params_t* params);
/** @brief 应用对端 ACK 调度偏好；后续控制包可更新该偏好。 */
void utp_connection_apply_peer_ack_frequency(utp_connection_t* connection, const utp_frame_ack_frequency_t* frequency,
                                             uint64_t now_us);
/** @brief 为主动连接生成密钥对并选择握手加密算法。 */
utp_internal_error_t utp_connection_configure_crypto(utp_connection_t* connection, uint8_t crypto_type);
/** @brief 将本端临时公钥编码为 CRYPTO 帧。 */
utp_internal_error_t utp_connection_encode_crypto(const utp_connection_t* connection, uint8_t* buffer, size_t capacity);
/** @brief 接管 pending 阶段派生的双向 AEAD 上下文，调用后清空 @p tx 和 @p rx。 */
utp_internal_error_t utp_connection_adopt_crypto(utp_connection_t* connection, uint8_t crypto_type,
                                                 utp_crypto_aead_t* tx, utp_crypto_aead_t* rx);
/** @brief 配置加密 0-RTT 的临时密钥、early 双向 AEAD 与 X25519 密钥对。 */
utp_internal_error_t utp_connection_configure_zero_rtt_crypto(
    utp_connection_t* connection, const uint8_t resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE],
    const uint8_t early_attempt_nonce[UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE],
    const uint8_t encrypted_server_info[UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE], uint8_t crypto_type);
/** @brief 用已验证的对端 X25519 公钥安装 1-RTT 双向 AEAD。 */
utp_internal_error_t utp_connection_complete_zero_rtt_crypto(utp_connection_t* connection,
                                                             const uint8_t peer_public_key[UTP_CRYPTO_X25519_KEY_SIZE]);
/** @brief 将 PacketOut 展平为可发送线上字节；加密包会在此执行 AEAD 封装。 */
utp_internal_error_t utp_connection_encode_packet_wire(const utp_connection_t* connection,
                                                       const utp_packet_out_t* packet, uint8_t* buffer, size_t capacity,
                                                       size_t* out_length);

/** @brief 构造完整明文包并放入有界发送队列。 */
utp_internal_error_t utp_connection_queue_packet(utp_connection_t* connection, uint8_t packet_type,
                                                 const uint8_t* payload, size_t payload_length, bool track_on_send);
/** @brief 排入 early AEAD 包；@p prefix_length 指示不加密的 SESSION_TOKEN 前缀。 */
utp_internal_error_t utp_connection_queue_early_packet(utp_connection_t* connection, uint8_t packet_type,
                                                       const uint8_t* payload, size_t payload_length,
                                                       uint16_t prefix_length, bool track_on_send);
/** @brief 排入由 Context 独立管理重传的 0-RTT HANDSHAKE 响应。 */
utp_internal_error_t utp_connection_queue_zero_rtt_response(utp_connection_t* connection, const uint8_t* payload,
                                                            size_t payload_length, bool encrypted);
/** @brief 排入单独成包的 CONNECTION_CLOSE，并关闭本端普通发送。 */
utp_internal_error_t utp_connection_queue_close(utp_connection_t* connection, uint16_t error_code);
/** @brief 因对端协议错误立即封锁连接；无法构造关闭包时直接进入 draining。 */
void utp_connection_close_on_protocol_error(utp_connection_t* connection, uint16_t error_code, uint64_t now_us);
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
/** @brief 返回本端 CONNECTION_CLOSE 重传截止时间，当前无需重传时为零。 */
uint64_t             utp_connection_close_retransmission_deadline(const utp_connection_t* connection);
/** @brief 处理 CONNECTION_CLOSE 重传超时，重新构造专用关闭包。 */
utp_internal_error_t utp_connection_on_close_retransmission_timeout(utp_connection_t* connection, uint64_t now_us);
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
/** @brief 设置对端首次创建流时的同步通知回调。 */
void utp_connection_set_on_incoming_stream_internal(utp_connection_t* connection, utp_on_incoming_stream_fn callback,
                                                    void* user_data);
/** @brief 设置本地恢复状态就绪时的同步通知回调。 */
void utp_connection_set_session_token_callback(utp_connection_t* connection, utp_session_token_cb_t callback,
                                               void* user_data);
/** @brief 创建本端发起流，并返回其唯一 stream_id。 */
utp_internal_error_t   utp_connection_create_stream_internal(utp_connection_t* connection, bool bidirectional,
                                                             uint32_t* out_stream_id);
/** @brief 按 stream_id 查找已存在流，不创建对端流。 */
utp_stream_t*          utp_connection_find_stream_internal(utp_connection_t* connection, uint32_t stream_id);
/** @brief 按指定方向关闭流，并为读关闭可靠排入 STOP_SENDING。 */
utp_internal_error_t   utp_stream_shutdown_internal(utp_stream_t* stream, utp_stream_shutdown_t how);
/** @brief 异常中止本地写方向并可靠排入 RESET_STREAM。 */
utp_internal_error_t   utp_stream_reset_internal(utp_stream_t* stream, uint16_t error_code);

/** @brief 返回当前连接状态；空指针视为 CLOSED。 */
utp_connection_state_t utp_connection_state(const utp_connection_t* connection);
/** @brief 判断连接是否处于可读写的 CONNECTED 状态。 */
bool                   utp_connection_is_connected(const utp_connection_t* connection);
/** @brief 返回指定类型的现存流数；UTP_STREAM_TYPE_ALL 返回全部流。 */
int32_t                utp_connection_stream_count_internal(const utp_connection_t* connection, utp_stream_type_t type);
/** @brief 返回本端仍可创建的指定类型流数。 */
int32_t utp_connection_creatable_stream_count_internal(const utp_connection_t* connection, utp_stream_type_t type);
/** @brief 复制连接运行统计快照。 */
utp_internal_error_t utp_connection_get_statistic_internal(const utp_connection_t*     connection,
                                                           utp_connection_statistic_t* out_statistic);
/** @brief 复制连接 CID 和对端地址描述。 */
utp_internal_error_t utp_connection_get_description_internal(const utp_connection_t*       connection,
                                                             utp_connection_description_t* out_description);
/** @brief 导出连接缓存的会话票据。 */
utp_internal_error_t utp_connection_export_session_token_internal(const utp_connection_t* connection, uint8_t* buffer,
                                                                  size_t capacity, size_t* out_length);
/** @brief 为 0-RTT 首个双向流保留已发送前缀，并将余量写入普通 1-RTT 发送缓冲。 */
utp_internal_error_t utp_connection_reserve_zero_rtt_stream(utp_connection_t* connection, const uint8_t* data,
                                                            size_t data_length, size_t early_data_length, bool fin);
/** @brief 标记被动 0-RTT 正在等待首个 HANDSHAKE 响应成功写出。 */
utp_internal_error_t utp_connection_begin_zero_rtt_response(utp_connection_t* connection);
/** @brief 退休本连接仍在发送、未确认或待重排的握手 flight。 */
utp_internal_error_t utp_connection_retire_handshake_flight(utp_connection_t* connection, uint64_t now_us);
/** @brief 解密并严格校验加密 0-RTT 的服务端 HANDSHAKE 响应，随后进入 CONNECTED。 */
utp_internal_error_t utp_connection_on_zero_rtt_handshake(utp_connection_t* connection, uint8_t* packet,
                                                          size_t* packet_length, const utp_address_t* peer,
                                                          uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONNECTION_CONNECTION_H
