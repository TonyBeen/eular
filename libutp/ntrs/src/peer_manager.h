#ifndef EULAR_NTRS_PEER_MANAGER_H
#define EULAR_NTRS_PEER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include <ntrs/service.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct event_base            event_base;
typedef struct ssl_ctx_st            SSL_CTX;
typedef struct utp_ntrs_peer_manager utp_ntrs_peer_manager_t;

typedef void (*utp_ntrs_peer_failed_fn)(void* user_data, const utp_ntrs_node_instance_t* remote);
typedef void (*utp_ntrs_peer_active_fn)(void* user_data, const utp_ntrs_node_instance_t* remote);
typedef void (*utp_ntrs_peer_forward_fn)(void* user_data, const utp_ntrs_node_instance_t* source,
                                         const utp_ntrs_forward_binding_response_t* forward);

/** @brief Node 间控制链路管理器的启动参数。 */
typedef struct utp_ntrs_peer_manager_options {
    event_base*              base;            // Node 所在 libevent loop
    SSL_CTX*                 client_tls;      // 出站 TLS 上下文，空指针表示明文 TCP
    SSL_CTX*                 server_tls;      // 入站 TLS 上下文，空指针表示明文 TCP
    utp_ntrs_node_instance_t local;           // 本 Node 实例
    utp_ntrs_endpoint_t      listen;          // control_endpoint
    const char*              interface_name;  // 绑定的 Linux 网卡；空指针表示不限制
    utp_ntrs_peer_active_fn  on_active;       // 唯一保留链路已可用的通知
    utp_ntrs_peer_failed_fn  on_failed;       // 已确认链路断开的通知
    utp_ntrs_peer_forward_fn on_forward;      // 协同 Node 请求直接发送组合 Binding 响应
    void*                    user_data;       // 回调上下文
} utp_ntrs_peer_manager_options_t;

/** @brief 启动 control_endpoint 的 TCP 或 TCP+TLS 监听。 */
utp_ntrs_peer_manager_t* utp_ntrs_peer_manager_start(const utp_ntrs_peer_manager_options_t* options);
/** @brief 停止监听并同步释放全部 Node 间连接。 */
void                     utp_ntrs_peer_manager_stop(utp_ntrs_peer_manager_t* manager);
/** @brief 确保到指定 Node 实例的控制链路存在；已有连接或拨号中的连接会复用。
 */
bool utp_ntrs_peer_manager_connect(utp_ntrs_peer_manager_t* manager, const utp_ntrs_node_instance_t* remote,
                                   const utp_ntrs_endpoint_t* endpoint);
/** @brief 经已激活的目标 Node 链路发送一次组合 Binding 响应请求。 */
bool utp_ntrs_peer_manager_send_forward(utp_ntrs_peer_manager_t* manager, const utp_ntrs_node_instance_t* target,
                                        const utp_ntrs_forward_binding_response_t* forward);
/** @brief 执行一次 10 秒粒度的 PING/PONG 保活和失效检查。 */
void utp_ntrs_peer_manager_tick(utp_ntrs_peer_manager_t* manager);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_NTRS_PEER_MANAGER_H
