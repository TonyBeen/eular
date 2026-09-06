#ifndef EULAR_UTP_CONTEXT_CONTEXT_H
#define EULAR_UTP_CONTEXT_CONTEXT_H

#include <utp/context.h>

#include "connection/connection.h"
#include "context/event_loop.h"
#include "context/pending_incoming.h"
#include "mtu/mtu.h"
#include "nat/nat.h"
#include "proto/packet_in.h"
#include "rendezvous/rendezvous.h"
#include "socket/udp.h"
#include "util/hash.h"
#include "util/log.h"

#define UTP_CONTEXT_PENDING_INCOMING_DEFAULT_LIMIT   1024u
#define UTP_CONTEXT_PACKET_LIMIT                     32u
#define UTP_CONTEXT_PACKET_IN_GROW_CAPACITY          64u
#define UTP_CONTEXT_PACKET_IN_BLOCK_CAPACITY         8u
#define UTP_CONTEXT_PACKET_IN_DEFAULT_MAX_FREE       256u
#define UTP_CONTEXT_PENDING_PACKET_LIMIT             16u
#define UTP_CONTEXT_PENDING_STORAGE_CAPACITY         32768u
#define UTP_CONTEXT_ZERO_RTT_REPLAY_DEFAULT_CAPACITY 4096u
#define UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE         32u
#define UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE \
    (UTP_CRYPTO_EARLY_ATTEMPT_NONCE_SIZE + UTP_CRYPTO_ENCRYPTED_SERVER_INFO_SIZE)
#define UTP_CONTEXT_NAT_RESULT_LIFETIME_US UINT64_C(300000000)
#define UTP_CONTEXT_NTRS_PACKET_CAPACITY   512u
#define UTP_CONTEXT_NTRS_CALIBRATION_CAPACITY \
    (UTP_PACKET_HEADER_SIZE + UTP_FRAME_RENDEZVOUS_HEADER_SIZE + UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + 8u)
#define UTP_CONTEXT_RENDEZVOUS_MAX_CANDIDATES            (UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES * 2u + 1u)
#define UTP_CONTEXT_RENDEZVOUS_PUNCH_CACHE_CAPACITY      16u
#define UTP_CONTEXT_RENDEZVOUS_PUNCH_CACHE_LIFETIME_US   UINT64_C(5000000)
#define UTP_CONTEXT_RENDEZVOUS_FORWARD_CACHE_CAPACITY    16u
#define UTP_CONTEXT_RENDEZVOUS_FORWARD_CACHE_LIFETIME_US UINT64_C(30000000)
#define UTP_CONTEXT_RENDEZVOUS_OUTPUT_CAPACITY           32u
#define UTP_CONTEXT_RENDEZVOUS_PACKET_CAPACITY           UTP_PACKET_MTU_FLOOR
#define UTP_CONTEXT_OBSERVED_ADDRESS_CAPACITY            4u
#define UTP_CONTEXT_ADDRESS_UPDATE_DEBOUNCE_MS           5000u

typedef struct utp_context_ntrs_registration {
    utp_address_t               endpoint;   // 当前 NTRS 目标或已注册 endpoint
    utp_on_ntrs_registered_fn   callback;   // 注册成功回调
    void*                       user_data;  // 回调用户数据
    utp_on_ntrs_unregistered_fn unregister_callback;
    void*                       unregister_user_data;
    uint8_t                     registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    uint8_t                     packet[UTP_CONTEXT_NTRS_PACKET_CAPACITY];  // REGISTER 逻辑包及其重传副本
    uint8_t       calibration_packets[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES]
                                     [UTP_CONTEXT_NTRS_CALIBRATION_CAPACITY];  // 各 calibration PING 的重传副本
    utp_address_t calibration_endpoints[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];
    uint64_t      request_id;     // REGISTER 幂等键
    uint64_t      packet_number;  // 本逻辑包的 Context 级包号
    uint64_t      last_activity_us;
    uint64_t      keepalive_packet_number;
    uint64_t      keepalive_deadline_us;
    uint64_t      calibration_packet_numbers[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];
    uint64_t      deadline_us;              // 当前等待 REGISTERED 的截止时刻
    uint64_t      calibration_deadline_us;  // 当前 calibration PONG 等待截止时刻
    size_t        packet_length;            // 完整 UTP 包长度
    uint8_t       calibration_packet_lengths[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];
    uint32_t      timeout_ms;  // 本轮等待时限
    uint32_t      keepalive_interval_ms;
    uint32_t      keepalive_timeout_ms;
    uint8_t       retries;                         // REGISTER/UNREGISTER 的额外重试次数
    uint8_t       retries_remaining;               // 尚可重传次数
    uint8_t       calibration_pending_mask;        // 等待 PONG 的 endpoint 位图
    uint8_t       calibration_write_pending_mask;  // 尚未写入内核的 endpoint 位图
    bool          pending : 1;                     // 正在等待 REGISTERED
    bool          registered : 1;                  // 已获得当前 token
    bool          write_pending : 1;               // UDP 暂不可写，等待 writable 事件
    bool          calibration_active : 1;          // REGISTERED 后正在收集 calibration PONG
    bool          unregistering : 1;               // 正在等待 UNREGISTERED
    bool          keepalive_pending : 1;           // 正在等待半连接 PONG
} utp_context_ntrs_registration_t;

typedef struct utp_context_ntrs_address_update {
    uint8_t       packet[UTP_CONTEXT_RENDEZVOUS_PACKET_CAPACITY];  // ADDRESS_UPDATE 逻辑包及其重传副本
    utp_address_t samples[UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES];     // 等待 ADDRESS_UPDATED 的发送快照
    uint64_t      observed_at_unix_ms[UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES];
    uint64_t      update_id;          // 当前批次幂等键
    uint64_t      deadline_us;        // 等待 ADDRESS_UPDATED 的截止时刻
    uint64_t      flush_deadline_us;  // 未发送样本的聚合截止时刻
    size_t        packet_length;
    uint32_t      timeout_ms;      // 初始等待时限
    uint32_t      retry_delay_ms;  // 下一次等待时限，指数退避
    uint8_t       retries;         // 配置的额外重试次数
    uint8_t       retries_remaining;
    uint8_t       sample_count;
    bool          pending : 1;        // 正在等待 ADDRESS_UPDATED
    bool          write_pending : 1;  // UDP 暂不可写，等待统一 writable 事件
} utp_context_ntrs_address_update_t;

typedef struct utp_context_zero_rtt_replay_entry {
    utp_hash_node_t node;                                       // 按重放键索引的哈希节点
    uint64_t        expires_at_seconds;                         // 票据绝对过期时间
    uint8_t         key[UTP_CONTEXT_ZERO_RTT_REPLAY_KEY_SIZE];  // ticket/nonce/包号派生重放键
} utp_context_zero_rtt_replay_entry_t;

typedef struct utp_context_connection_slot {
    utp_hash_node_t node;                                          // 按本端 CID 索引的哈希节点
    utp_hash_node_t peer_node;                                     // 被动连接按来源地址和对端 CID 索引的哈希节点
    TAILQ_ENTRY(utp_context_connection_slot) free_next;            // 空闲槽位链表节点
    TAILQ_ENTRY(utp_context_connection_slot) terminal_error_next;  // 延迟终止事件链表节点
    utp_connection_t           connection;                         // 槽位持有的连接对象
    utp_connect_attempt_info_t connect_attempt;                    // 主动建连尝试描述
    uint64_t                   connect_deadline_us;                // 主动连接超时截止时刻
    utp_packet_in_t*           zero_rtt_early_packet;              // 被动 0-RTT 早数据 PacketIn 引用
    size_t                     zero_rtt_early_wire_size;           // 早数据原始 UDP 长度
    uint64_t                   zero_rtt_request_packet_number;     // 收到的 0-RTT 请求包号
    uint64_t                   zero_rtt_request_received_us;       // 0-RTT 请求接收时刻
    uint64_t                   zero_rtt_response_deadline_us;      // 0-RTT 响应重试截止时刻
    uint64_t                   zero_rtt_expire_deadline_us;        // 被动 0-RTT 状态清理时刻
    uint64_t                   zero_rtt_amplification_rx_bytes;    // 0-RTT 防放大接收额度
    uint64_t                   zero_rtt_amplification_tx_bytes;    // 0-RTT 防放大已发送字节
    uint8_t                    zero_rtt_session_token[UTP_CONTEXT_ZERO_RTT_TOKEN_PAYLOAD_SIZE];  // 原始票据 payload
    uint8_t                    zero_rtt_resumption_psk[UTP_CRYPTO_RESUMPTION_PSK_SIZE];          // 早期 AEAD PSK
    uint8_t                    rendezvous_id[UTP_RENDEZVOUS_ID_SIZE];  // 主动连接的 rendezvous 幂等键
    uint8_t                    rendezvous_punch_token[UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE];
    utp_address_t              rendezvous_candidates[UTP_CONTEXT_RENDEZVOUS_MAX_CANDIDATES];
    char                       target_peer_id[UTP_PEER_ID_MAX_LENGTH + 1u];  // 主动连接复制的目标路由标识
    uint8_t*                   zero_rtt_early_data;                          // 主动 0-RTT 重传期间持有的早期流数据
    size_t                     zero_rtt_early_data_size;                     // 缓存早期数据长度
    uint64_t                   zero_rtt_expires_at_seconds;                  // 票据绝对过期时间
    utp_status_t               terminal_error_status;                        // 待投递的本地终止错误
    const char*                terminal_error_reason;                        // 待投递错误原因，只借用静态字符串
    size_t                     terminal_error_reason_length;                 // 待投递错误原因长度
    int8_t                     connect_retries_remaining;                    // 主动连接剩余重试次数
    uint8_t                    zero_rtt_response_retries;                    // 0-RTT 响应已重试次数
    uint8_t                    zero_rtt_encryption_mode;                     // 票据指定加密模式
    uint8_t                    target_peer_id_length;                        // target_peer_id 的有效字节数
    uint8_t                    rendezvous_candidate_count;
    bool                       zero_rtt_early_fin : 1;         // 早期流数据是否带 FIN
    bool                       zero_rtt_awaiting_accept : 1;   // 是否等待 on_new_connection 决策
    bool                       zero_rtt_accepted : 1;          // 应用是否已接受 0-RTT
    bool                       zero_rtt_response_active : 1;   // 是否维护 0-RTT 响应重传状态
    bool                       zero_rtt_response_queued : 1;   // HANDSHAKE_DONE 是否已排队
    bool                       zero_rtt_response_sent : 1;     // HANDSHAKE_DONE 是否已实际发送
    bool                       zero_rtt_early_delivered : 1;   // 是否已将早数据投递给流
    bool                       used : 1;                       // 槽位是否正在使用
    bool                       connected_reported : 1;         // 是否已调用 on_connected
    bool                       connection_error_reported : 1;  // 是否已调用 connection error 回调
    bool                       connect_pending : 1;            // 是否有主动连接等待完成
    bool                       terminal_error_queued : 1;      // 是否已进入延迟终止事件队列
    bool                       terminal_error_suppressed : 1;  // 本地 close 是否取消该错误回调
    bool                       rendezvous_active : 1;          // 已收到 NTRS 候选计划，握手包向全部候选 fanout
} utp_context_connection_slot_t;
TAILQ_HEAD(utp_context_connection_slot_tailq, utp_context_connection_slot);

typedef struct utp_context_rendezvous_punch_cache_entry {
    utp_address_t endpoint;                                // 提前到达 PUNCH 的来源 endpoint
    uint8_t       token[UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE];  // PUNCH token
    uint64_t      expires_at_us;                           // 单调时钟过期时间
    bool          used;                                    // 是否占用
} utp_context_rendezvous_punch_cache_entry_t;

typedef struct utp_context_rendezvous_forward_cache_entry {
    uint8_t  rendezvous_id[UTP_RENDEZVOUS_ID_SIZE];
    uint64_t expires_at_us;
    bool     used;
} utp_context_rendezvous_forward_cache_entry_t;

typedef struct utp_context_rendezvous_output_entry {
    utp_address_t peer;
    uint8_t       packet[UTP_CONTEXT_RENDEZVOUS_PACKET_CAPACITY];
    uint16_t      packet_length;
} utp_context_rendezvous_output_entry_t;

typedef struct utp_context_observed_address {
    utp_address_t endpoint;             // 对端从连接路径观察到的本端公网 endpoint
    uint64_t      observed_at_unix_ms;  // 该观测的墙钟时间，供 ADDRESS_UPDATE 透传
} utp_context_observed_address_t;

typedef struct utp_context_pending_slot {
    utp_hash_node_t node;                                                  // 按待处理本端 CID 索引的哈希节点
    utp_hash_node_t peer_node;                                             // 按来源地址和对端 CID 索引的哈希节点
    TAILQ_ENTRY(utp_context_pending_slot) free_next;                       // 空闲 pending 槽位链表节点
    utp_pending_incoming_t pending;                                        // 未接受被动握手状态
    uint8_t                storage[UTP_CONTEXT_PENDING_STORAGE_CAPACITY];  // 入站包有界缓存
    bool                   used : 1;                                       // 槽位是否正在使用
    bool                   queued : 1;                                     // 是否已进入 pending 哈希表
} utp_context_pending_slot_t;
TAILQ_HEAD(utp_context_pending_slot_tailq, utp_context_pending_slot);

struct utp_context {
    utp_event_loop_t                         event_loop;                       // 借用调用方 libevent 循环
    utp_event_t                              udp_event;                        // UDP 可读事件
    utp_event_t                              udp_write_event;                  // UDP 可写事件
    utp_event_t                              timer_event;                      // 协议定时器事件
    utp_udp_socket_t                         udp_socket;                       // Context 持有的 UDP socket
    utp_address_t                            bound_address;                    // bind 成功后的本地地址与地址族
    utp_packet_in_pool_t                     packet_in_pool;                   // 入站包对象池
    uint8_t                                  encrypt_send_buffer[UINT16_MAX];  // 握手构造和最终 UDP 加密串行复用缓冲
    utp_hash_table_t                         connections;                      // 活跃连接 CID 表
    utp_hash_table_t                         passive_connections_by_peer;      // 被动连接来源地址和对端 CID 表
    struct utp_context_connection_slot_tailq free_connection_slots;            // 空闲连接槽位
    struct utp_context_connection_slot_tailq terminal_error_slots;             // 待调度边界投递的本地终止事件
    utp_hash_table_t                         pending_incoming;                 // 等待 accept 的被动握手表
    utp_hash_table_t                         pending_incoming_by_peer;         // pending 来源地址和对端 CID 表
    struct utp_context_pending_slot_tailq    free_pending_slots;               // 空闲 pending 槽位
    uint32_t                                 next_cid;                         // 下一个自动分配 CID
    uint64_t                                 next_nat_probe_packet_number;     // Context NAT 探测包号命名空间
    uint64_t                                 next_rendezvous_packet_number;    // Context 半连接包号命名空间
    utp_address_t                            local_candidates[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];  // 已知本地候选地址
    utp_context_observed_address_t
                                 observed_addresses[UTP_CONTEXT_OBSERVED_ADDRESS_CAPACITY];  // 对端观察到的本端公网地址
    char                         peer_id[UTP_PEER_ID_MAX_LENGTH + 1u];  // 创建时复制的 Context 路由标识
    uint8_t                      peer_id_length;                        // peer_id 的有效字节数
    uint8_t                      local_candidate_count;                 // local_candidates 的有效项数
    uint8_t                      observed_address_count;                // observed_addresses 的有效项数
    uint8_t                      rendezvous_output_head;
    uint8_t                      rendezvous_output_count;
    utp_stream_scheduler_mode_t  stream_scheduler_mode;                                // 新连接默认流调度策略
    utp_congestion_algorithm_t   cc_algorithm;                                         // 新连接默认拥塞算法
    uint32_t                     clock_granularity_us;                                 // pacer 时钟粒度
    utp_bbr_config_t             bbr_config;                                           // BBR 默认配置
    utp_cubic_config_t           cubic_config;                                         // CUBIC 默认配置
    utp_mtu_config_t             mtu_config;                                           // MTU 发现默认配置
    uint8_t                      resumption_root_key[UTP_CRYPTO_RESUMPTION_KEY_SIZE];  // 恢复根密钥
    utp_crypto_resumption_keys_t resumption_keys;                                      // 派生票据与本地状态密钥
    utp_hash_table_t             zero_rtt_replay;                                      // 0-RTT 抗重放键表
    uint32_t                     zero_rtt_token_max_lifetime_seconds;                  // 票据最大有效期
    uint32_t                     zero_rtt_replay_cache_capacity;                       // 抗重放表容量
    uint32_t                     stream_terminal_capacity;                             // 新连接流终态表容量
    uint32_t                     path_validation_buffer_capacity;                      // 新连接候选路径缓存上限(bytes)
    utp_nat_probe_task_t         nat_probe;                                            // 当前 NAT 探测任务
    utp_nat_probe_result_t       nat_result;                                           // 最近一次完成的 NAT 探测缓存
    utp_context_ntrs_registration_t   ntrs_registration;    // Context 到单个 NTRS 的半连接注册
    utp_context_ntrs_address_update_t ntrs_address_update;  // Context 到 NTRS 的地址样本批量上报
    utp_context_rendezvous_punch_cache_entry_t
        rendezvous_punch_cache[UTP_CONTEXT_RENDEZVOUS_PUNCH_CACHE_CAPACITY];  // 未匹配 PUNCH 的短期缓存
    utp_context_rendezvous_forward_cache_entry_t
        rendezvous_forward_cache[UTP_CONTEXT_RENDEZVOUS_FORWARD_CACHE_CAPACITY];  // 已处理 FORWARD 的短期去重缓存
    utp_context_rendezvous_output_entry_t
        rendezvous_output[UTP_CONTEXT_RENDEZVOUS_OUTPUT_CAPACITY];             // 零 CID Rendezvous 延迟发送 FIFO
    utp_frame_transport_params_t   local_transport_params;                     // 新连接本端传输参数
    utp_frame_ack_frequency_t      local_ack_frequency;                        // 新连接本端 ACK 策略
    uint32_t                       keepalive_interval_ms;                      // 保活间隔
    uint32_t                       keepalive_timeout_ms;                       // 保活超时
    uint16_t                       handshake_timeout_ms;                       // 被动握手超时
    uint16_t                       keepalive_probes;                           // 连续保活探测次数
    uint8_t                        handshake_max_retries;                      // 被动握手最大重试次数
    utp_context_pending_slot_t*    callback_accept_pending;                    // 当前回调可接受的 pending 槽位
    utp_context_connection_slot_t* callback_accept_zero_rtt;                   // 当前回调可接受的 0-RTT 槽位
    bool                           callback_accept_requested : 1;              // on_new_connection 是否调用 accept
    bool                           resumption_key_explicit : 1;                // 根密钥是否由用户设置
    bool                           resumption_keys_ready : 1;                  // 恢复工作密钥是否就绪
    bool                           default_resumption_key_warning_logged : 1;  // 默认根密钥警告是否已输出
    bool                           enable_keepalive : 1;                       // 是否为新连接启用保活
    bool                           nat_result_valid : 1;                       // nat_result 是否仍可用于注册
    utp_on_connected_fn            on_connected;                               // 主动连接成功回调
    void*                          on_connected_user_data;                     // 成功回调用户数据
    utp_on_connect_error_fn        on_connect_error;                           // 主动连接失败回调
    void*                          on_connect_error_user_data;                 // 失败回调用户数据
    utp_on_new_connection_fn       on_new_connection;                          // 被动连接决策回调
    void*                          on_new_connection_user_data;                // 决策回调用户数据
    utp_on_connection_error_fn     on_connection_error;                        // 被动关闭或致命错误回调
    void*                          on_connection_error_user_data;              // 关闭回调用户数据
    utp_logger_t                   logger;                                     // Context 日志器
    utp_log_tag_t                  tag;                                        // Context 日志标签
};

utp_internal_error_t utp_context_flush_public_connection(utp_context_t* context, utp_connection_t* connection);
bool                 utp_context_suppress_terminal_error(utp_context_t* context, utp_connection_t* connection);
void                 utp_context_remember_observed_address(utp_context_t* context, const utp_address_t* address);

#endif  // EULAR_UTP_CONTEXT_CONTEXT_H
