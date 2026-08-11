#ifndef EULAR_UTP_C_CONNECTION_H
#define EULAR_UTP_C_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_connection utp_connection_t;
typedef struct utp_stream     utp_stream_t;

typedef enum utp_stream_type { UTP_STREAM_TYPE_BIDIRECTIONAL = 0, UTP_STREAM_TYPE_UNIDIRECTIONAL } utp_stream_type_t;

/** @brief 对端首次创建流时同步调用；Connection 和 Stream 均为借用指针，仅在其所属对象存续期间有效。 */
typedef void (*utp_on_incoming_stream_fn)(utp_connection_t* connection, utp_stream_t* stream, void* user_data);
/** @brief 本地恢复状态缓存就绪时同步调用；回调内可通过 utp_connection_export_session_token() 导出。 */
typedef void (*utp_on_session_token_ready_fn)(utp_connection_t* connection, void* user_data);

/** @brief 发起连接关闭；发送错误通过 Context 的错误回调异步报告。 */
void          utp_connection_close(utp_connection_t* connection);
/** @brief 创建本端发起流，并通过 @p out_stream_id 返回流标识。 */
utp_status_t  utp_connection_create_stream(utp_connection_t* connection, utp_stream_type_t type,
                                           uint32_t* out_stream_id);
/** @brief 返回由 Connection 持有的借用流指针，流回收或 Connection 销毁后失效。 */
utp_stream_t* utp_connection_get_stream(utp_connection_t* connection, uint32_t stream_id);
/** @brief 设置对端新建流回调；回调在 Context 事件循环线程的入站包处理期间同步执行，传入 NULL 可关闭通知。 */
void          utp_connection_set_on_incoming_stream(utp_connection_t* connection, utp_on_incoming_stream_fn callback,
                                                    void* user_data);
/** @brief 设置恢复状态就绪回调；若当前已缓存恢复状态，设置回调时会立即同步通知一次，传入 NULL 可关闭通知。 */
void utp_connection_set_on_session_token_ready(utp_connection_t* connection, utp_on_session_token_ready_fn callback,
                                               void* user_data);
/** @brief 判断连接是否处于可读写的 CONNECTED 状态。 */
bool utp_connection_is_connected(const utp_connection_t* connection);
/** @brief 导出最近接收的会话票据；缓冲区不足时返回 OVERFLOW 并给出所需长度。 */
utp_status_t utp_connection_export_session_token(const utp_connection_t* connection, uint8_t* buffer, size_t capacity,
                                                 size_t* out_length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_CONNECTION_H
