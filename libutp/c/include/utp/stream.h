#ifndef EULAR_UTP_C_STREAM_H
#define EULAR_UTP_C_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_stream utp_stream_t;

/** @brief 流变为可读或接收到连续 FIN 时同步调用。 */
typedef void (*utp_on_stream_readable_fn)(utp_stream_t* stream, void* user_data);
/** @brief 流发送缓冲重新获得可写空间时同步调用。 */
typedef void (*utp_on_stream_writable_fn)(utp_stream_t* stream, void* user_data);
/** @brief 流双向关闭且接收缓冲已排空时同步调用，仅一次。 */
typedef void (*utp_on_stream_closed_fn)(utp_stream_t* stream, void* user_data);

#define UTP_STREAM_PRIORITY_HIGHEST 0u
#define UTP_STREAM_PRIORITY_LOWEST  7u
#define UTP_STREAM_PRIORITY_DEFAULT 4u
#define UTP_STREAM_ERROR_CANCELLED  UINT16_C(1)

typedef enum utp_stream_shutdown {
    UTP_STREAM_SHUTDOWN_READ  = 0,  // 关闭本地读方向，并请求对端停止发送
    UTP_STREAM_SHUTDOWN_WRITE = 1,  // 关闭本地写方向，以 FIN 正常结束
    UTP_STREAM_SHUTDOWN_BOTH  = 2,  // 同时关闭本地读写方向
} utp_stream_shutdown_t;

typedef struct utp_stream_read_view {
    const uint8_t* data;    // 连续可读数据视图，commit 前有效
    uint64_t       offset;  // 数据在流内的起始偏移
    size_t         length;  // 可提交消费的数据长度
    bool           fin;     // 此视图末尾是否为对端 FIN
} utp_stream_read_view_t;

typedef struct utp_stream_write_view {
    uint8_t* data;    // 内部发送缓冲的可写视图，commit 前有效
    size_t   length;  // 本段可写容量
} utp_stream_write_view_t;

/** @brief 返回协议流 ID；无效流返回 UINT32_MAX。 */
uint32_t     utp_stream_id(const utp_stream_t* stream);
/** @brief 向流写入数据；数据会复制至流的发送缓冲区。 */
utp_status_t utp_stream_write(utp_stream_t* stream, const void* data, size_t length);
/** @brief 复制读取连续数据；暂缺数据返回 WOULD_BLOCK，读尽 FIN 返回 CLOSED，对端 RESET 返回 CANCELLED。 */
utp_status_t utp_stream_read(utp_stream_t* stream, void* buffer, size_t capacity, size_t* out_length);
/** @brief 关闭本地读、写或双向；读关闭会发送 STOP_SENDING，写关闭会在剩余数据后发送 FIN。 */
utp_status_t utp_stream_shutdown(utp_stream_t* stream, utp_stream_shutdown_t how);
/** @brief 异常中止本地写方向，丢弃待发数据并发送 RESET_STREAM；读方向保持可用。 */
utp_status_t utp_stream_reset(utp_stream_t* stream, uint16_t error_code);

/** @brief 获取内部发送缓冲的可写视图；commit 前不得再次 acquire。 */
utp_status_t utp_stream_acquire_write_views(utp_stream_t* stream, utp_stream_write_view_t* views, size_t view_capacity,
                                            size_t* out_view_count, size_t* out_capacity);
/** @brief 提交已写入视图的字节数。 */
utp_status_t utp_stream_commit_write_views(utp_stream_t* stream, size_t length);
/** @brief 获取零拷贝读视图；数据只在对应 commit 前有效。 */
utp_status_t utp_stream_acquire_read_view(utp_stream_t* stream, utp_stream_read_view_t* out_view);
/** @brief 提交已消费的零拷贝读视图范围。 */
utp_status_t utp_stream_commit_read_view(utp_stream_t* stream, uint64_t offset, size_t length);

/** @brief 返回当前可连续读取的字节数。 */
size_t       utp_stream_readable_bytes(const utp_stream_t* stream);
/** @brief 判断流的本地和对端方向是否均已结束。 */
bool         utp_stream_is_closed(const utp_stream_t* stream);
/** @brief 设置可读回调；若已有连续数据或 FIN，设置时立即同步通知。 */
void         utp_stream_set_on_readable(utp_stream_t* stream, utp_on_stream_readable_fn callback, void* user_data);
/** @brief 设置可写回调；若当前可写，设置时立即同步通知。 */
void         utp_stream_set_on_writable(utp_stream_t* stream, utp_on_stream_writable_fn callback, void* user_data);
/** @brief 设置双向关闭回调；仅对后续关闭状态变化通知。 */
void         utp_stream_set_on_closed(utp_stream_t* stream, utp_on_stream_closed_fn callback, void* user_data);
/** @brief 设置 Strict 或 DRR 调度使用的流优先级，范围为 0 至 7。 */
utp_status_t utp_stream_set_priority(utp_stream_t* stream, uint8_t priority);
/** @brief 返回流当前优先级。 */
uint8_t      utp_stream_priority(const utp_stream_t* stream);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_STREAM_H
