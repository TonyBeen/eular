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

// 发起连接关闭；发送错误通过 Context 的错误回调异步报告。
void          utp_connection_close(utp_connection_t* connection);
utp_status_t  utp_connection_create_stream(utp_connection_t* connection, utp_stream_type_t type,
                                           uint32_t* out_stream_id);
// 返回由 Connection 持有的借用指针。流完全关闭且接收数据消费完毕后即可被回收，
// Connection 销毁时也会失效；调用方不得在上述时机后继续持有该指针。
utp_stream_t* utp_connection_get_stream(utp_connection_t* connection, uint32_t stream_id);
bool          utp_connection_is_connected(const utp_connection_t* connection);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_CONNECTION_H
