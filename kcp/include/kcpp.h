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
 * @brief SYN连接请求回调函数
 *
 * @param kcp_ctx kcp上下文
 * @param addr 连接请求地址
 *
 * @return bool 返回true表示接受连接, false表示拒绝连接
 */
typedef bool (*on_kcp_syn_received_t)(struct KcpContext *kcp_ctx, const sockaddr_t *addr);

/**
 * @brief kcp建连回调函数
 *
 * @param kcp kcp上下文
 * @param kcp_connection kcp连接
 * @param code 建连结果, 0表示成功, 其他表示失败
 */
typedef void (*on_kcp_accepted_t)(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code);

/**
 * @brief 错误回调
 *
 * @param kcp_ctx kcp上下文
 * @param code 错误码
 */
typedef void (*on_kcp_error_t)(struct KcpContext *kcp_ctx, int32_t code);

/// kcp function

/**
 * @brief Create a new kcp control block.
 *
 * @param base event base.
 * @param user The user data.
 * @return struct KcpContext*
 */
KCP_PORT struct KcpContext *kcp_create(struct event_base *base, on_kcp_error_t cb, void *user);

/**
 * @brief destroy kcp control block.
 *
 * @param kcp The kcp control block.
 */
KCP_PORT void kcp_destroy(struct KcpContext *kcp_ctx);

/**
 * @brief configure kcp control block.
 *
 * @param kcp The kcp control block.
 * @param config The kcp configuration.
 *
 * @return int32_t 0 if success, otherwise -1.
 */
KCP_PORT int32_t kcp_configure(struct KcpConnection *kcp_connection, config_key_t flags, kcp_config_t *config);

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
KCP_PORT int32_t kcp_listen(struct KcpContext *kcp_ctx, int32_t backlog, on_kcp_syn_received_t cb);

KCP_PORT void kcp_set_accept_cb(struct KcpContext *kcp_ctx, on_kcp_accepted_t cb);

/**
 * @brief 
 *
 * @param kcp 
 * @param addr 
 * @param addrlen 
 * @return struct KcpConnection* 
 */
KCP_PORT int32_t kcp_accept(struct KcpContext *kcp_ctx, sockaddr_t *addr, uint32_t timeout_ms);

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
KCP_PORT void kcp_close(struct KcpConnection *kcp_connection, uint32_t timeout_ms);

/**
 * @brief Send RST to peer.
 */
KCP_PORT void kcp_shutdown(struct KcpConnection *kcp_connection);

/**
 * @brief send data to peer.
 * @param kcp The kcp control block.
 * @param data The data to send.
 * @param size The size of data.
 *
 * @return int32_t Return the byte size written to the sending queue.
 */
KCP_PORT int32_t kcp_write(struct KcpContext *kcp_ctx, const void *data, size_t size);

KCP_PORT int32_t kcp_writev(struct KcpContext *kcp_ctx, const struct iovec *iov, int32_t iovcnt);

KCP_PORT int32_t kcp_recd(struct KcpContext *kcp_ctx, void *data, size_t size);

KCP_PORT int32_t kcp_recdv(struct KcpContext *kcp_ctx, struct iovec *iov, int32_t iovcnt);

KCP_PORT int32_t kcp_update(struct KcpContext *kcp_ctx, uint32_t current);


/**
 * @brief 获取kcp上下文的用户数据
 *
 * @param kcp_ctx kcp上下文
 * @return void* 用户数据
 */
KCP_PORT void *kcp_get_user_data(struct KcpContext *kcp_ctx);

EXTERN_C_END

#endif // __KCP_PLUS_H__
