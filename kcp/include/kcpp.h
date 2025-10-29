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
 * @param addr store remote address
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

KCP_PORT void kcp_set_close_cb(struct KcpContext *kcp_ctx, on_kcp_closed_t cb);

/**
 * @brief Send FIN to peer.
 */
KCP_PORT void kcp_close(struct KcpConnection *kcp_connection);

/**
 * @brief Send RST to peer.
 */
KCP_PORT void kcp_shutdown(struct KcpConnection *kcp_connection);

/**
 * @brief send data to peer.
 * @param kcp_connection kcp connection
 * @param data the data to send.
 * @param size the size of data.
 *
 * @return int32_t return NO_ERROR if success, otherwise < 0.
 */
KCP_PORT int32_t kcp_send(struct KcpConnection *kcp_connection, const void *data, size_t size);

KCP_PORT void set_kcp_read_event_cb(struct KcpConnection *kcp_connection, on_kcp_read_event_t cb);

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
const char *kcp_connection_remote_address(struct KcpConnection *kcp_connection, char *buf, size_t len);

int32_t kcp_connection_get_fd(struct KcpConnection *kcp_connection);

int32_t kcp_connection_get_mtu(struct KcpConnection *kcp_connection);

void kcp_connection_get_statistic(struct KcpConnection *kcp_connection, kcp_statistic_t *statistic);

EXTERN_C_END

#endif // __KCP_PLUS_H__
