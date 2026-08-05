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

#define UTP_STREAM_PRIORITY_HIGHEST 0u
#define UTP_STREAM_PRIORITY_LOWEST  7u
#define UTP_STREAM_PRIORITY_DEFAULT 4u

typedef struct utp_stream_read_view {
    const uint8_t* data;
    uint64_t       offset;
    size_t         length;
    bool           fin;
} utp_stream_read_view_t;

typedef struct utp_stream_write_view {
    uint8_t* data;
    size_t   length;
} utp_stream_write_view_t;

/** @brief 返回协议流 ID；无效流返回 UINT32_MAX。 */
uint32_t     utp_stream_id(const utp_stream_t* stream);
/** @brief 向流写入数据；数据会复制至流的发送缓冲区。 */
utp_status_t utp_stream_write(utp_stream_t* stream, const void* data, size_t length);
/** @brief 复制读取连续数据；暂缺数据返回 WOULD_BLOCK，读尽对端 FIN 后返回 CLOSED。 */
utp_status_t utp_stream_read(utp_stream_t* stream, void* buffer, size_t capacity, size_t* out_length);
/** @brief 关闭本地写方向，剩余数据发送完成后携带 FIN；读方向不受影响。 */
void         utp_stream_close(utp_stream_t* stream);
/** @brief 发送 RESET_STREAM 并立即终止本流。 */
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
/** @brief 判断流是否由对端 RESET_STREAM 终止。 */
bool         utp_stream_reset_by_peer(const utp_stream_t* stream);
/** @brief 设置 Strict 或 DRR 调度使用的流优先级，范围为 0 至 7。 */
utp_status_t utp_stream_set_priority(utp_stream_t* stream, uint8_t priority);
/** @brief 返回流当前优先级。 */
uint8_t      utp_stream_priority(const utp_stream_t* stream);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_STREAM_H
