#ifndef EULAR_NTRS_TLS_STREAM_H
#define EULAR_NTRS_TLS_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct bufferevent bufferevent;
typedef struct event_base event_base;
typedef struct ssl_ctx_st SSL_CTX;

typedef struct utp_ntrs_tls_stream utp_ntrs_tls_stream_t;

typedef void (*utp_ntrs_tls_ready_fn)(void *user_data);
typedef void (*utp_ntrs_tls_plaintext_fn)(void *user_data, const uint8_t *data,
                                          size_t length);
typedef void (*utp_ntrs_tls_closed_fn)(void *user_data);

/** @brief 控制流事件回调。closed 回调中由调用方释放所属连接。 */
typedef struct utp_ntrs_tls_callbacks {
  utp_ntrs_tls_ready_fn ready;         // TLS 握手完成或明文 TCP 已连接
  utp_ntrs_tls_plaintext_fn plaintext; // TLS 解密或明文 TCP 的连续字节
  utp_ntrs_tls_closed_fn closed;       // 连接关闭或 TLS 协议错误
} utp_ntrs_tls_callbacks_t;

/** @brief 从 PEM 证书和私钥创建 TLS 1.3 服务端上下文。 */
SSL_CTX *utp_ntrs_tls_server_context_new(const char *certificate_path,
                                         const char *private_key_path);
/** @brief 创建不验证证书链的 TLS 1.3 客户端上下文。首期仅提供链路加密。 */
SSL_CTX *utp_ntrs_tls_client_context_new(void);
/** @brief 释放 TLS 上下文。 */
void utp_ntrs_tls_context_free(SSL_CTX *context);

/**
 * @brief 将一个已连接或连接中的 TCP fd 包装为非阻塞控制流。
 * @param context 非空时使用 TLS 1.3；空指针时使用明文 TCP。
 */
utp_ntrs_tls_stream_t *
utp_ntrs_tls_stream_new(event_base *base, int32_t fd, SSL_CTX *context,
                        bool server, const utp_ntrs_tls_callbacks_t *callbacks,
                        void *user_data);
/** @brief 释放控制流及其 TCP fd。 */
void utp_ntrs_tls_stream_free(utp_ntrs_tls_stream_t *stream);
/** @brief 客户端 TCP 三次握手完成后调用，启动 TLS ClientHello 或明文控制流。 */
bool utp_ntrs_tls_stream_connected(utp_ntrs_tls_stream_t *stream);
/** @brief 排队发送一段 TLS 明文或明文 TCP 字节；数据在函数返回前复制。 */
bool utp_ntrs_tls_stream_send(utp_ntrs_tls_stream_t *stream,
                              const uint8_t *data, size_t length);
/** @brief 尽快将待发送字节推入 TCP socket。 */
bool utp_ntrs_tls_stream_flush(utp_ntrs_tls_stream_t *stream);
/** @brief 获取底层 bufferevent，仅用于客户端建立 TCP 连接。 */
bufferevent *utp_ntrs_tls_stream_transport(utp_ntrs_tls_stream_t *stream);

#endif // EULAR_NTRS_TLS_STREAM_H
