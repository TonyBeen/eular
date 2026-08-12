#ifndef EULAR_UTP_INTERNAL_RANGE_SET_H
#define EULAR_UTP_INTERNAL_RANGE_SET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/allocator.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_range {
    uint64_t start;  // 区间起始值（含）
    uint64_t end;    // 区间结束值（不含）
} utp_range_t;

typedef struct utp_range_set {
    utp_range_t*           ranges;      // 有序区间数组所有权
    size_t                 count;       // 当前区间数
    size_t                 capacity;    // 已分配区间容量
    size_t                 max_ranges;  // 区间数硬上限
    const utp_allocator_t* allocator;   // 分配器，不拥有
} utp_range_set_t;

utp_internal_error_t utp_range_set_init(utp_range_set_t* set, const utp_allocator_t* allocator, size_t max_ranges);
void                 utp_range_set_cleanup(utp_range_set_t* set);
void                 utp_range_set_clear(utp_range_set_t* set);
utp_internal_error_t utp_range_set_insert(utp_range_set_t* set, uint64_t start, uint64_t end);
bool                 utp_range_set_contains(const utp_range_set_t* set, uint64_t value);
const utp_range_t*   utp_range_set_at(const utp_range_set_t* set, size_t index);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_RANGE_SET_H
