#ifndef EULAR_UTP_INTERNAL_BUFFER_H
#define EULAR_UTP_INTERNAL_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "util/allocator.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_buffer {
    uint8_t*               data;          // 动态数据缓冲所有权
    size_t                 length;        // 当前有效长度
    size_t                 capacity;      // 已分配容量
    size_t                 max_capacity;  // 允许扩容的上限
    const utp_allocator_t* allocator;     // 分配器，不拥有
} utp_buffer_t;

utp_internal_error_t utp_buffer_init(utp_buffer_t* buffer, const utp_allocator_t* allocator, size_t max_capacity);
void                 utp_buffer_cleanup(utp_buffer_t* buffer);
void                 utp_buffer_clear(utp_buffer_t* buffer);
utp_internal_error_t utp_buffer_reserve(utp_buffer_t* buffer, size_t capacity);
utp_internal_error_t utp_buffer_resize(utp_buffer_t* buffer, size_t length);
utp_internal_error_t utp_buffer_append(utp_buffer_t* buffer, const void* data, size_t length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_BUFFER_H
