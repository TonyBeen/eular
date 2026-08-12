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

typedef enum utp_stream_type {
    UTP_STREAM_TYPE_BIDIRECTIONAL = 0,
    UTP_STREAM_TYPE_UNIDIRECTIONAL,
    UTP_STREAM_TYPE_ALL = 0xff
} utp_stream_type_t;

#define UTP_CONNECTION_REMOTE_HOST_MAX_LENGTH 46u

/** @brief 连接的稳定描述信息；remote_host 为以 NUL 结尾的 IPv4 或 IPv6 文本。 */
typedef struct utp_connection_description {
    uint32_t local_cid;
    uint32_t peer_cid;
    char     remote_host[UTP_CONNECTION_REMOTE_HOST_MAX_LENGTH];
    uint16_t remote_port;
} utp_connection_description_t;

/** @brief 连接运行统计快照；时间单位为 us，带宽单位为 bytes/s。 */
typedef struct utp_connection_statistic {
    uint16_t pmtu;
    uint64_t rtt;
    uint64_t rttvar;
    uint64_t bw_estimate;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rtx_bytes;
    uint64_t scheduler_select_total;
    uint64_t scheduler_select_strict;
    uint64_t scheduler_select_drr;
    uint64_t scheduler_strict_aging_promoted;
    uint64_t scheduler_mode_switches;
    uint64_t scheduler_drr_refills;
    uint64_t scheduler_drr_consumes;
} utp_connection_statistic_t;

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
void    utp_connection_set_on_session_token_ready(utp_connection_t* connection, utp_on_session_token_ready_fn callback,
                                                  void* user_data);
/** @brief 判断连接是否处于可读写的 CONNECTED 状态。 */
bool    utp_connection_is_connected(const utp_connection_t* connection);
/** @brief 返回指定类型的现存流数；UTP_STREAM_TYPE_ALL 表示全部流，参数无效返回 -1。 */
int32_t utp_connection_stream_count(const utp_connection_t* connection, utp_stream_type_t type);
/** @brief 返回当前仍可由本端创建的流数；仅接受双向或单向流类型，参数无效返回 -1。 */
int32_t utp_connection_creatable_stream_count(const utp_connection_t* connection, utp_stream_type_t type);
/** @brief 获取连接运行统计快照。 */
utp_status_t utp_connection_get_statistic(const utp_connection_t*     connection,
                                          utp_connection_statistic_t* out_statistic);
/** @brief 获取连接 CID 与对端地址描述。 */
utp_status_t utp_connection_get_description(const utp_connection_t*       connection,
                                            utp_connection_description_t* out_description);
/** @brief 导出最近接收的会话票据；缓冲区不足时返回 OVERFLOW 并给出所需长度。 */
utp_status_t utp_connection_export_session_token(const utp_connection_t* connection, uint8_t* buffer, size_t capacity,
                                                 size_t* out_length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_CONNECTION_H
