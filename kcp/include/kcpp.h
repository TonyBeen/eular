#ifndef __KCP_PLUS_H__
#define __KCP_PLUS_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kcp_def.h>
#include <kcp_config.h>
#include <kcp_net_def.h>

EXTERN_C_BEGIN

struct KcpContext;
struct KcpConnection;
struct event;
struct event_base;
struct iovec;

#define KCP_P2P_MAX_CANDIDATES 8
#define KCP_NTRS_MAX_TEXT_LEN 128

// 重传率计算公式
// retransmission rate = rtx_bytes / (double)tx_bytes
typedef struct KcpStatistic {
    uint32_t        ping_count; // ping times
    uint32_t        pong_count; // pong times
    uint64_t        tx_bytes;   // transmit bytes
    uint64_t        rtx_bytes;  // retransmit bytes
    int32_t         srtt;       // smoothed round trip time (us)
    int32_t         rttvar;     // round trip time variance (us)
    int32_t         rto;        // retransmission timeout (us)

    // BBR runtime metrics
    int32_t         bbr_mode;         // 1:start_up 2:drain 3:probe_bw 4:probe_rtt
    int32_t         cwnd;             // packet unit
    uint32_t        target_cwnd;      // packet unit
    uint32_t        min_rtt_us;       // us
    uint64_t        btlbw_bytes_ps;   // bytes/s
    uint64_t        pacing_rate_bps;  // bytes/s
} kcp_statistic_t;

/// kcp callback

/**
 * @brief kcp连接回调函数
 *
 * @param kcp_connection kcp连接, 连接失败为空
 * @param user 用户数据
 * @param code 连接结果
 */
typedef void (*on_kcp_connected_t)(struct KcpConnection *kcp_connection, int32_t code);

/**
 * @brief kcp断连回调函数
 *
 * @param kcp_connection kcp连接
 * @param user 用户数据
 * @param code 断连原因
 */
typedef void (*on_kcp_closed_t)(struct KcpConnection *kcp_connection, int32_t code);

/**
 * @brief 客户端连接请求回调函数
 *
 * @param kcp_ctx kcp上下文
 * @param addr 连接请求地址
 *
 * @return bool 返回true表示接受连接, false表示拒绝连接
 */
typedef bool (*on_kcp_connect_t)(struct KcpContext *kcp_ctx, const sockaddr_t *addr);

/**
 * @brief KCP connection callback function
 *
 * @param kcp KCP context
 * @param kcp_connection KCP connection
 * @param code Connection result, NO_ERROR indicates success, others indicate failure
 */
typedef void (*on_kcp_accepted_t)(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code);

/**
 * @brief Error callback
 *
 * @param kcp_ctx KCP context
 * @param kcp_connection KCP connection(can be NULL)
 * @param code error code
 */
typedef void (*on_kcp_error_t)(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code);

/**
 * @brief read event callback
 *
 * @param kcp_connection KCP connection
 * @param size The size of data
 */
typedef void (*on_kcp_read_event_t)(struct KcpConnection *kcp_connection, int32_t size);

/**
 * @brief write event callback
 *
 * @param kcp_connection KCP connection
 * @param wnd The size of the send window
 */
typedef void (*on_kcp_write_event_t)(struct KcpConnection *kcp_connection, int32_t wnd);

typedef enum KcpP2PCandidateType {
    KCP_P2P_CANDIDATE_UNKNOWN = 0,
    KCP_P2P_CANDIDATE_HOST_LOCAL = 1,
    KCP_P2P_CANDIDATE_SRFLX_PRIMARY = 2,
    KCP_P2P_CANDIDATE_SRFLX_SECONDARY = 3,
} kcp_p2p_candidate_type_t;

typedef struct KcpP2PCandidate {
    sockaddr_t                addr;
    kcp_p2p_candidate_type_t  type;
    uint8_t                   priority;
} kcp_p2p_candidate_t;

typedef enum KcpNtrsNatClass {
    KCP_NTRS_NAT_UNKNOWN = 0,
    KCP_NTRS_NAT_OPEN_PUBLIC = 1,
    KCP_NTRS_NAT_FULL_CONE = 2,
    KCP_NTRS_NAT_IP_RESTRICTED = 3,
    KCP_NTRS_NAT_PORT_RESTRICTED = 4,
    KCP_NTRS_NAT_SYMMETRIC = 5,
} kcp_ntrs_nat_class_t;

typedef enum KcpNtrsMappingBehavior {
    KCP_NTRS_MAPPING_UNKNOWN = 0,
    KCP_NTRS_MAPPING_ENDPOINT_INDEPENDENT = 1,
    KCP_NTRS_MAPPING_ADDRESS_DEPENDENT = 2,
    KCP_NTRS_MAPPING_ADDRESS_AND_PORT_DEPENDENT = 3,
    KCP_NTRS_MAPPING_UNSTABLE = 4,
} kcp_ntrs_mapping_behavior_t;

typedef enum KcpNtrsFilteringBehavior {
    KCP_NTRS_FILTERING_UNKNOWN = 0,
    KCP_NTRS_FILTERING_ENDPOINT_INDEPENDENT = 1,
    KCP_NTRS_FILTERING_ADDRESS_DEPENDENT = 2,
    KCP_NTRS_FILTERING_ADDRESS_AND_PORT_DEPENDENT = 3,
    KCP_NTRS_FILTERING_BLOCKED = 4,
} kcp_ntrs_filtering_behavior_t;

enum KcpNtrsNatFlags {
    KCP_NTRS_NAT_FLAG_NONE = 0,
    KCP_NTRS_NAT_FLAG_LOCAL_ADDR_PUBLIC = 1u << 0,
    KCP_NTRS_NAT_FLAG_UDP_BLOCKED = 1u << 1,
    KCP_NTRS_NAT_FLAG_PROBE_DEGRADED = 1u << 2,
    KCP_NTRS_NAT_FLAG_MAPPING_UNSTABLE = 1u << 3,
    KCP_NTRS_NAT_FLAG_MULTI_EXTERNAL_IP = 1u << 4,
};

typedef struct KcpNtrsNatInfo {
    sockaddr_t                       local_addr;
    sockaddr_t                       srflx_primary;
    sockaddr_t                       srflx_secondary;
    kcp_ntrs_nat_class_t             nat_class;
    uint16_t                         nat_flags;
    kcp_ntrs_mapping_behavior_t      mapping_behavior;
    kcp_ntrs_filtering_behavior_t    filtering_behavior;
} kcp_ntrs_nat_info_t;

typedef struct KcpNtrsConfig {
    const char *node_host;
    uint16_t    node_port;
    const char *peer_id;
    const char *device_id;
    const char *auth_token;
    const char *bind_ip;
    const char *bind_device;
    int32_t     connect_timeout_ms;
    int32_t     detect_rounds;
    int32_t     detect_retries;
    bool        verbose;
} kcp_ntrs_config_t;

typedef struct KcpNtrsSessionSignal {
    char                    peer_id[KCP_NTRS_MAX_TEXT_LEN];
    kcp_ntrs_nat_class_t     peer_nat_class;
    uint16_t                 peer_nat_flags;
    uint32_t                 candidate_count;
    kcp_p2p_candidate_t      candidates[KCP_P2P_MAX_CANDIDATES];
} kcp_ntrs_session_signal_t;

typedef void (*on_kcp_ntrs_ready_t)(struct KcpContext *kcp_ctx,
                                    int32_t code,
                                    const kcp_ntrs_nat_info_t *nat,
                                    void *user_data);

typedef void (*on_kcp_ntrs_signal_t)(struct KcpContext *kcp_ctx,
                                     const kcp_ntrs_session_signal_t *signal,
                                     void *user_data);

/// kcp function

/**
 * @brief Create a new kcp control block.
 *
 * @param base event base.
 * @param user The user data.
 * @return struct KcpContext*
 */
KCP_PORT struct KcpContext *kcp_context_create(struct event_base *base, on_kcp_error_t cb, void *user);

/**
 * @brief destroy kcp control block.
 *
 * @param kcp_ctx The kcp control block.
 */
KCP_PORT void kcp_context_destroy(struct KcpContext *kcp_ctx);

/**
 * @brief configure kcp control block.
 *
 * @param kcp_connection KCP connection
 * @param flags The configuration flags.
 * @param config The kcp configuration.
 *
 * @return int32_t 0 if success, otherwise -1.
 */
KCP_PORT int32_t kcp_configure(struct KcpConnection *kcp_connection, em_config_key_t flags, const kcp_config_t *config);

/**
 * @brief Configure IO related parameters
 *
 * @param kcp_connection KCP connection
 * @param flags The configuration flags.
 * @param data The kcp configuration.
 * @return int32_t 0 if success, otherwise < 0. 
 */
KCP_PORT int32_t kcp_ioctl(struct KcpConnection *kcp_connection, em_ioctl_t flags, void *data);

/**
 * @brief 绑定本地地址和端口, 网卡
 *
 * @param kcp kcp控制块
 * @param host 本地地址
 * @param port 本地端口
 * @param nic 网卡名字
 * @return int32_t 成功返回0, 否则返回负值
 */
KCP_PORT int32_t kcp_bind(struct KcpContext *kcp_ctx, const sockaddr_t *addr, const char *nic);

KCP_PORT int32_t kcp_context_udp_socket(struct KcpContext *kcp_ctx);

/**
 * @brief 监听连接请求
 *
 * @param kcp_ctx kcp上下文
 * @param backlog 最大连接数
 * @param cb syn连接回调
 * @return int32_t 成功返回0, 否则返回负值
 */
KCP_PORT int32_t kcp_listen(struct KcpContext *kcp_ctx, on_kcp_connect_t cb);

/**
 * @brief set accept callback
 *
 * @param kcp_ctx kcp context
 * @param cb callback
 * @return void
 */
KCP_PORT void kcp_set_accept_cb(struct KcpContext *kcp_ctx, on_kcp_accepted_t cb);

/**
 * @brief accept a connection
 *
 * @param kcp_ctx kcp context
 * @param timeout_ms timeout in milliseconds
 * @return int32_t 0 if success, otherwise < 0
 */
KCP_PORT int32_t kcp_accept(struct KcpContext *kcp_ctx, uint32_t timeout_ms);

/**
 * @brief 
 *
 * @param kcp_ctx 
 * @param addr 
 * @param timeout_ms 
 * @param cb 
 * @return KCP_PORT 
 */
KCP_PORT int32_t kcp_connect(struct KcpContext *kcp_ctx, const sockaddr_t *addr, uint32_t timeout_ms, on_kcp_connected_t cb);

/**
 * @brief Connect to multiple peer candidates using the same UDP socket.
 *
 * The first candidate that completes the KCP handshake becomes the selected
 * remote address. The normal kcp_connect() API is unchanged and is equivalent
 * to connecting with one candidate.
 */
KCP_PORT int32_t kcp_connect_candidates(struct KcpContext *kcp_ctx,
                                        const kcp_p2p_candidate_t *candidates,
                                        uint32_t candidate_count,
                                        uint32_t timeout_ms,
                                        on_kcp_connected_t cb);

KCP_PORT void kcp_ntrs_config_init(kcp_ntrs_config_t *config);

KCP_PORT int32_t kcp_ntrs_configure(struct KcpContext *kcp_ctx, const kcp_ntrs_config_t *config);

KCP_PORT int32_t kcp_ntrs_start(struct KcpContext *kcp_ctx,
                                on_kcp_ntrs_ready_t ready_cb,
                                on_kcp_ntrs_signal_t signal_cb,
                                void *user_data);

KCP_PORT int32_t kcp_ntrs_create_session(struct KcpContext *kcp_ctx,
                                         const char *dst_peer_id,
                                         uint32_t timeout_ms,
                                         on_kcp_connected_t cb);

KCP_PORT void kcp_ntrs_stop(struct KcpContext *kcp_ctx);

KCP_PORT void kcp_set_close_cb(struct KcpContext *kcp_ctx, on_kcp_closed_t cb);

/**
 * @brief Send FIN to peer.
 */
KCP_PORT void kcp_close(struct KcpConnection *kcp_connection);

/**
 * @brief Send RST to peer.
 */
KCP_PORT void kcp_shutdown(struct KcpConnection *kcp_connection);

KCP_PORT void kcp_set_write_event_cb(struct KcpConnection *kcp_connection, on_kcp_write_event_t cb);

/**
 * @brief send data to peer.
 * @param kcp_connection kcp connection
 * @param data the data to send.
 * @param size the size of data.
 *
 * @return int32_t return NO_ERROR if success, otherwise < 0.
 */
KCP_PORT int32_t kcp_send(struct KcpConnection *kcp_connection, const void *data, size_t size);

KCP_PORT void kcp_set_read_event_cb(struct KcpConnection *kcp_connection, on_kcp_read_event_t cb);

KCP_PORT int32_t kcp_recv(struct KcpConnection *kcp_connection, void *data, size_t size);

// --------------------------------------------------------------------------
/**
 * @brief get the kcp connection remote address.
 *
 * @param kcp_connection kcp connection
 * @param buf buffer
 * @param len buffer size
 * @return const char* return buf
 */
KCP_PORT const char *kcp_connection_remote_address(struct KcpConnection *kcp_connection, char *buf, size_t len);

KCP_PORT void *kcp_connection_user_data(struct KcpConnection *kcp_connection);

KCP_PORT int32_t kcp_connection_get_fd(struct KcpConnection *kcp_connection);

KCP_PORT int32_t kcp_connection_get_mtu(struct KcpConnection *kcp_connection);

KCP_PORT void kcp_connection_get_statistic(struct KcpConnection *kcp_connection, kcp_statistic_t *statistic);

EXTERN_C_END

#endif // __KCP_PLUS_H__
