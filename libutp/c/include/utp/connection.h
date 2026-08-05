#ifndef EULAR_UTP_C_CONNECTION_H
#define EULAR_UTP_C_CONNECTION_H

#include <stdbool.h>
#include <stdint.h>

#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_connection utp_connection_t;
typedef struct utp_stream     utp_stream_t;

typedef enum utp_stream_type { UTP_STREAM_TYPE_BIDIRECTIONAL = 0, UTP_STREAM_TYPE_UNIDIRECTIONAL } utp_stream_type_t;

/** @brief 发起连接关闭；发送错误通过 Context 的错误回调异步报告。 */
void          utp_connection_close(utp_connection_t* connection);
/** @brief 创建本端发起流，并通过 @p out_stream_id 返回流标识。 */
utp_status_t  utp_connection_create_stream(utp_connection_t* connection, utp_stream_type_t type,
                                           uint32_t* out_stream_id);
/** @brief 返回由 Connection 持有的借用流指针，流回收或 Connection 销毁后失效。 */
utp_stream_t* utp_connection_get_stream(utp_connection_t* connection, uint32_t stream_id);
/** @brief 判断连接是否处于可读写的 CONNECTED 状态。 */
bool          utp_connection_is_connected(const utp_connection_t* connection);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_CONNECTION_H
