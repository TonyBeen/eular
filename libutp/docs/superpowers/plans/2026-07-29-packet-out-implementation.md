# packet_out (发送包对象 + 池) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `utp_packet_out_t`(发送包对象)与 `utp_packet_out_pool_t`(两级池:定长结构体池 + 按大小分桶的数据缓冲池)到 `c/`,作为可靠性发送侧子项目 1/3 的落地实现。

**Architecture:** 单个模块 `c/src/internal/packet_out.h` + `c/src/packet_out.c`,内部用 `3rd/queue.h` 的 `TAILQ_ENTRY`/`TAILQ_HEAD` 复用同一个 `po_next` 挂载点承载"结构体池空闲链"与(未来 send_ctl 的)队列成员关系。数据缓冲区按调用方配置的 `(size,count)` 档位在 `pool_init` 时一次性预分配(free-list-in-array,每个桶一个 node 数组 + 一块连续 storage),`acquire` 选最小可容纳桶,`release` 原地复位并把结构体与缓冲区分别归还各自空闲链。`raw_data`/`encrypt_data` 共享同一块桶缓冲(不做 cpp 式的独立加密缓冲生命周期),因为 C 端 `crypto.h` 的 AEAD API 本就写入调用方提供的缓冲区。

**Tech Stack:** C11,`3rd/queue.h`(vendored BSD `sys/queue.h`,已确认零 GNU 扩展),Catch2(`.cc` 测试,风格对齐 `test_receive_history.cc`)。

## Global Constraints

- C11,`-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2`(GNU/Clang)或 `/W4 /WX`(MSVC)——`c/CMakeLists.txt:56-61`。
- 所有堆内存必须经 `utp_allocator_t`(`utp_allocator_alloc`/`utp_allocator_free`),不得直接调用 `malloc`/`free`——`c/STYLE.md`。
- established-connection 的收发路径不得分配内存,内存边界在创建时(此处即 `pool_init`)确定;`acquire`/`release` 之后不得再触发分配——`c/STYLE.md`。
- 不使用 VLA、编译器扩展、宏参数副作用、包处理路径中的递归——`c/STYLE.md`。
- 内部函数一律返回 `utp_internal_error_t`(`c/src/internal/error.h`),不是公开 API,不涉及 `utp_status_t`。
- 桶位最多 `UTP_PACKET_OUT_MAX_BUCKETS = 8` 档,单档容量上限为 `uint16_t` 的 65535——`docs/superpowers/specs/2026-07-29-packet-out-design.md` §4/§5.1。
- `frame_types` 复用 `c/src/internal/frame.h` 现有的 `UTP_FRAME_BIT(type)`,不新增帧类型枚举。

---

## Task 1: 池骨架 —— 结构体池 + 分桶缓冲池的 init/cleanup

**Files:**
- Create: `c/src/internal/packet_out.h`
- Create: `c/src/packet_out.c`
- Modify: `c/CMakeLists.txt:39-48`(库源文件/头文件列表)、`c/CMakeLists.txt:53-54`(`utp_c` 的 include 目录)、`c/CMakeLists.txt:75-84`(`utp_c_configure_cpp_test` 增加 3rd 目录)、`c/CMakeLists.txt:86-97`(新增测试可执行文件)、`c/CMakeLists.txt:104-114`(format 源文件列表)
- Test: `c/test/test_packet_out.cc`

**Interfaces:**
- Consumes: `utp_allocator_t`/`utp_allocator_alloc`/`utp_allocator_free`/`utp_allocator_resolve`(`c/src/internal/allocator.h`);`utp_internal_error_t`/`UTP_INTERNAL_ERROR_OK`/`_INVALID_ARGUMENT`/`_NOMEM`/`_LIMIT`(`c/src/internal/error.h`);`TAILQ_HEAD`/`TAILQ_ENTRY`/`TAILQ_INIT`/`TAILQ_INSERT_TAIL`/`TAILQ_REMOVE`/`TAILQ_EMPTY`/`TAILQ_FIRST`(`3rd/queue.h`)。
- Produces:
  - `utp_packet_out_bucket_config_t { uint16_t size; size_t count; }`
  - `utp_packet_out_pool_t`(完整定义,可栈上零初始化)
  - `utp_packet_out_t`(完整定义,含本任务新增的内部记账字段 `size_t bucket_index`)
  - `utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool, const utp_allocator_t *allocator, size_t struct_capacity, const utp_packet_out_bucket_config_t *buckets, size_t bucket_count)`
  - `void utp_packet_out_pool_cleanup(utp_packet_out_pool_t *pool)`
  - 供 Task 2/3 使用的完整函数原型(先声明,后续任务补实现):`utp_packet_out_pool_acquire`、`utp_packet_out_pool_release`、`utp_packet_out_add_send_attempt`、`utp_packet_out_clear_send_attempts`

- [ ] **Step 1: 写头文件 `c/src/internal/packet_out.h`**

```c
#ifndef EULAR_UTP_INTERNAL_PACKET_OUT_H
#define EULAR_UTP_INTERNAL_PACKET_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/allocator.h"
#include "internal/error.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_PACKET_OUT_MAX_FRAMES   8u
#define UTP_PACKET_OUT_MAX_SLICES   8u
#define UTP_PACKET_OUT_MAX_ATTEMPTS 4u
#define UTP_PACKET_OUT_MAX_BUCKETS  8u

#define UTP_PO_HELLO          0x0001u
#define UTP_PO_ENCRYPTED      0x0002u
#define UTP_PO_RESET_PACKNO   0x0004u
#define UTP_PO_NO_ENCRYPT     0x0008u
#define UTP_PO_MTU_PROBE      0x0010u
#define UTP_PO_UNACKED        0x0020u
#define UTP_PO_SCHED          0x0040u
#define UTP_PO_LOST           0x0080u
#define UTP_PO_LOSS_RECORDED  0x0100u
#define UTP_PO_KEEP_PLAINTEXT 0x0200u

#define UTP_POL_LOSS          0x0001u
#define UTP_POL_LIMITED       0x0002u
#define UTP_POL_FACKED        0x0004u
#define UTP_POL_TRACK_ON_SEND 0x0008u

#define UTP_FRAME_META_FIN 0x01u

#define UTP_PACKET_OUT_SLICE_RAW_OFFSET 0u
#define UTP_PACKET_OUT_SLICE_EXTERNAL   1u

typedef struct utp_packet_out_attempt {
    uint64_t packet_number;
    uint64_t sent_time_us;
} utp_packet_out_attempt_t;

typedef struct utp_frame_meta_info {
    void    *owner;
    uint16_t offset;
    uint16_t length;
    uint8_t  frame_type;
    uint8_t  frame_flags;
} utp_frame_meta_info_t;

typedef struct utp_packet_out_slice {
    uint16_t    offset;
    uint16_t    length;
    const void *data;
    uint8_t     source;
} utp_packet_out_slice_t;

struct utp_packet_out;
TAILQ_HEAD(utp_packet_out_tailq, utp_packet_out);

typedef struct utp_packet_out {
    TAILQ_ENTRY(utp_packet_out) po_next;

    uint64_t                sent_time_us;
    uint64_t                packet_number;
    uint64_t                ack_number;
    struct utp_packet_out  *loss_chain;

    uint32_t frame_types;
    uint16_t po_flags;
    uint16_t local_flags;

    uint16_t data_size;
    uint16_t encrypt_data_size;
    uint16_t alloc_size;
    uint8_t  slice_count;
    uint8_t  frame_meta_count;
    uint32_t stream_data_size;
    uint16_t transient_ack_size;
    uint32_t stream_id;
    uint64_t stream_offset;

    utp_packet_out_slice_t   slices[UTP_PACKET_OUT_MAX_SLICES];
    utp_frame_meta_info_t    frame_meta[UTP_PACKET_OUT_MAX_FRAMES];
    utp_packet_out_attempt_t attempts[UTP_PACKET_OUT_MAX_ATTEMPTS];
    uint16_t                 attempt_count;

    void *bw_state;

    uint8_t *raw_data;
    uint8_t *encrypt_data;

    /* 池记账字段:acquire 时记录该对象缓冲区来自哪个桶,release 时用于 O(1) 归还,
       不属于协议/发送语义,不出现在 docs 的字段表里。 */
    size_t bucket_index;
} utp_packet_out_t;

typedef struct utp_packet_out_bucket_config {
    uint16_t size;
    size_t   count;
} utp_packet_out_bucket_config_t;

typedef struct utp_packet_out_buffer_node {
    TAILQ_ENTRY(utp_packet_out_buffer_node) link;
    uint8_t *data;
} utp_packet_out_buffer_node_t;
TAILQ_HEAD(utp_packet_out_buffer_node_tailq, utp_packet_out_buffer_node);

typedef struct utp_packet_out_bucket {
    uint16_t                              size;
    size_t                                count;
    uint8_t                              *storage;
    utp_packet_out_buffer_node_t         *nodes;
    struct utp_packet_out_buffer_node_tailq free_buffers;
} utp_packet_out_bucket_t;

typedef struct utp_packet_out_pool {
    const utp_allocator_t          *allocator;
    utp_packet_out_t                *structs;
    size_t                           struct_capacity;
    struct utp_packet_out_tailq      free_structs;
    utp_packet_out_bucket_t          buckets[UTP_PACKET_OUT_MAX_BUCKETS];
    size_t                           bucket_count;
} utp_packet_out_pool_t;

utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool, const utp_allocator_t *allocator,
                                              size_t struct_capacity, const utp_packet_out_bucket_config_t *buckets,
                                              size_t bucket_count);
void                 utp_packet_out_pool_cleanup(utp_packet_out_pool_t *pool);
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size,
                                                 utp_packet_out_t **out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t *pool, utp_packet_out_t *pkt);

bool utp_packet_out_add_send_attempt(utp_packet_out_t *pkt, uint64_t packet_number, uint64_t sent_time_us);
void utp_packet_out_clear_send_attempts(utp_packet_out_t *pkt);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_PACKET_OUT_H
```

- [ ] **Step 2: 写 `c/src/packet_out.c` 的 init/cleanup(先只实现这两个,其余函数体留到 Task 2/3 补,本步骤不写它们的定义)**

```c
#include "internal/packet_out.h"

#include <limits.h>
#include <string.h>

static void bucket_cleanup(utp_packet_out_bucket_t *bucket, const utp_allocator_t *allocator) {
    if (bucket->nodes != NULL) {
        utp_allocator_free(allocator, bucket->nodes);
    }
    if (bucket->storage != NULL) {
        utp_allocator_free(allocator, bucket->storage);
    }
    memset(bucket, 0, sizeof(*bucket));
}

static utp_internal_error_t bucket_init(utp_packet_out_bucket_t *bucket, const utp_allocator_t *allocator,
                                        uint16_t size, size_t count) {
    size_t i;

    memset(bucket, 0, sizeof(*bucket));
    TAILQ_INIT(&bucket->free_buffers);
    bucket->size  = size;
    bucket->count = count;

    if (count > SIZE_MAX / size || count > SIZE_MAX / sizeof(*bucket->nodes)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }

    bucket->storage = utp_allocator_alloc(allocator, (size_t)size * count);
    if (bucket->storage == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    bucket->nodes = utp_allocator_alloc(allocator, count * sizeof(*bucket->nodes));
    if (bucket->nodes == NULL) {
        bucket_cleanup(bucket, allocator);
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    for (i = 0u; i < count; ++i) {
        bucket->nodes[i].data = bucket->storage + i * (size_t)size;
        TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[i], link);
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_cleanup(utp_packet_out_pool_t *pool) {
    size_t i;

    if (pool == NULL) {
        return;
    }
    for (i = 0u; i < pool->bucket_count; ++i) {
        bucket_cleanup(&pool->buckets[i], pool->allocator);
    }
    if (pool->structs != NULL) {
        utp_allocator_free(pool->allocator, pool->structs);
    }
    memset(pool, 0, sizeof(*pool));
}

utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool, const utp_allocator_t *allocator,
                                              size_t struct_capacity, const utp_packet_out_bucket_config_t *buckets,
                                              size_t bucket_count) {
    utp_packet_out_bucket_config_t sorted[UTP_PACKET_OUT_MAX_BUCKETS];
    size_t                         i;
    size_t                         j;

    if (pool == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memset(pool, 0, sizeof(*pool));

    if (struct_capacity == 0u || struct_capacity > SIZE_MAX / sizeof(utp_packet_out_t) || buckets == NULL ||
        bucket_count == 0u || bucket_count > UTP_PACKET_OUT_MAX_BUCKETS) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < bucket_count; ++i) {
        if (buckets[i].size == 0u || buckets[i].count == 0u) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }

    memcpy(sorted, buckets, bucket_count * sizeof(*buckets));
    for (i = 1u; i < bucket_count; ++i) {
        for (j = i; j > 0u && sorted[j - 1u].size > sorted[j].size; --j) {
            utp_packet_out_bucket_config_t tmp = sorted[j];

            sorted[j]     = sorted[j - 1u];
            sorted[j - 1u] = tmp;
        }
    }

    pool->allocator = utp_allocator_resolve(allocator);
    pool->structs   = utp_allocator_alloc(pool->allocator, struct_capacity * sizeof(utp_packet_out_t));
    if (pool->structs == NULL) {
        return UTP_INTERNAL_ERROR_NOMEM;
    }
    memset(pool->structs, 0, struct_capacity * sizeof(utp_packet_out_t));
    pool->struct_capacity = struct_capacity;
    TAILQ_INIT(&pool->free_structs);
    for (i = 0u; i < struct_capacity; ++i) {
        pool->structs[i].loss_chain = &pool->structs[i];
        TAILQ_INSERT_TAIL(&pool->free_structs, &pool->structs[i], po_next);
    }

    for (i = 0u; i < bucket_count; ++i) {
        utp_internal_error_t error = bucket_init(&pool->buckets[i], pool->allocator, sorted[i].size, sorted[i].count);

        if (!utp_internal_error_is_ok(error)) {
            pool->bucket_count = i;
            utp_packet_out_pool_cleanup(pool);
            return error;
        }
    }
    pool->bucket_count = bucket_count;
    return UTP_INTERNAL_ERROR_OK;
}
```

- [ ] **Step 3: 更新 `c/CMakeLists.txt`**

在第 39-48 行的 `add_library(utp_c STATIC ...)` 源文件列表中,`src/receive_history.c` 后面加入 `src/packet_out.c`;头文件列表中 `src/internal/receive_history.h` 后面加入 `src/internal/packet_out.h`:

```cmake
add_library(utp_c STATIC
    src/allocator.c src/ack.c src/address.c src/buffer.c src/crypto.c src/error.c src/event_loop.c src/frame.c
    src/hash.c src/log.c src/range_set.c src/ring.c src/status.c src/time.c src/udp.c src/proto.c src/ack_scheduler.c src/send_history.c src/rtt.c
    src/receive_history.c src/packet_out.c src/utp.c
    include/utp/log.h include/utp/status.h include/utp/utp.h
    src/internal/allocator.h src/internal/ack.h src/internal/address.h src/internal/buffer.h src/internal/crypto.h
    src/internal/error.h src/internal/event_loop.h src/internal/frame.h src/internal/hash.h src/internal/log.h
    src/internal/range_set.h src/internal/ring.h src/internal/time.h src/internal/udp.h src/internal/proto.h
    src/internal/receive_history.h src/internal/ack_scheduler.h src/internal/send_history.h src/internal/rtt.h
    src/internal/packet_out.h src/internal/wire.h
)
```

第 53-54 行的 include 目录追加 `3rd`(供 `#include "queue.h"` 解析):

```cmake
target_include_directories(utp_c PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../3rd")
```

第 75-84 行的 `utp_c_configure_cpp_test` 函数同样追加 `3rd`(测试 `.cc` 直接 `#include "internal/packet_out.h"`,需要能解析到 `"queue.h"`):

```cmake
function(utp_c_configure_cpp_test target)
    target_link_libraries(${target} PRIVATE utp_c)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../cpp/test"
        "${CMAKE_CURRENT_SOURCE_DIR}/../3rd")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wformat=2 -UNDEBUG)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /UNDEBUG)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()
```

第 86-97 行的测试块里,`utp_c_receive_history_test` 之后新增:

```cmake
    add_executable(utp_c_packet_out_test test/test_packet_out.cc)
    utp_c_configure_cpp_test(utp_c_packet_out_test)
```

第 104-114 行的 `UTP_C_FORMAT_SOURCES` 追加 `src/packet_out.c`、`src/internal/packet_out.h`、`test/test_packet_out.cc`(位置对齐上面两处改动)。

- [ ] **Step 4: 写第一批测试 `c/test/test_packet_out.cc`**

```cpp
#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>
#include <cstdlib>

extern "C" {
#include "internal/packet_out.h"
}

namespace {

struct allocation_tracker {
    size_t allocations = 0u;
    size_t frees       = 0u;
};

void *tracked_alloc(void *user_data, size_t size) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);

    ++tracker->allocations;
    return std::malloc(size);
}

void *tracked_realloc(void *user_data, void *pointer, size_t size) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);

    ++tracker->allocations;
    return std::realloc(pointer, size);
}

void tracked_free(void *user_data, void *pointer) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);

    ++tracker->frees;
    std::free(pointer);
}

}  // namespace

TEST_CASE("packet_out pool init allocates once for structs plus two allocations per bucket", "[packet_out][pool]") {
    allocation_tracker             tracker   = {};
    const utp_allocator_t          allocator = {tracked_alloc, tracked_realloc, tracked_free, &tracker};
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 2u}, {512u, 1u}};

    REQUIRE(utp_packet_out_pool_init(&pool, &allocator, 3u, buckets, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(tracker.allocations == 5u);  // structs(1) + (storage+nodes) * 2 buckets

    utp_packet_out_pool_cleanup(&pool);
    REQUIRE(tracker.frees == 5u);
}

TEST_CASE("packet_out pool init rejects invalid bucket configuration", "[packet_out][pool]") {
    utp_packet_out_pool_t          pool                                       = {};
    utp_packet_out_bucket_config_t single[]                                   = {{128u, 1u}};
    utp_packet_out_bucket_config_t too_many[UTP_PACKET_OUT_MAX_BUCKETS + 1u]  = {};
    utp_packet_out_bucket_config_t zero_size[]                                = {{0u, 1u}};
    utp_packet_out_bucket_config_t zero_count[]                               = {{128u, 0u}};
    size_t                          i;

    for (i = 0u; i < UTP_PACKET_OUT_MAX_BUCKETS + 1u; ++i) {
        too_many[i].size  = static_cast<uint16_t>(128u + i);
        too_many[i].count = 1u;
    }

    REQUIRE(utp_packet_out_pool_init(nullptr, nullptr, 1u, single, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 0u, single, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, nullptr, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, single, 0u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, too_many, UTP_PACKET_OUT_MAX_BUCKETS + 1u) ==
            UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, zero_size, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, zero_count, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("packet_out pool accepts a bucket sized at the uint16_t ceiling", "[packet_out][pool]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{65535u, 1u}};

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    utp_packet_out_pool_cleanup(&pool);
}
```

- [ ] **Step 5: 运行测试确认 Step 4 中的用例失败(此时 `utp_packet_out_pool_init`/`_cleanup` 还没实现,应无法编译)**

Run: `cmake --build build --target utp_c_packet_out_test` (假设 `build/` 已由 `cmake -S c -B build` 生成)
Expected: 编译失败(`utp_packet_out_pool_init`/`utp_packet_out_pool_cleanup` 未定义,或头文件不存在),证明测试确实在检验尚未实现的功能。

- [ ] **Step 6: 完成 Step 2/Step 3 的实现与 CMake 改动后重新构建并运行**

Run: `cmake -S c -B build -DUTP_C_BUILD_TESTS=ON && cmake --build build --target utp_c_packet_out_test && ctest --test-dir build -R utp_c_packet_out_test --output-on-failure`
Expected: 全部 3 个 `TEST_CASE` PASS。

- [ ] **Step 7: Commit**

```bash
git add c/src/internal/packet_out.h c/src/packet_out.c c/CMakeLists.txt c/test/test_packet_out.cc
git commit -m "feat(c): packet_out 池骨架(结构体池 + 分桶缓冲池 init/cleanup)"
```

---

## Task 2: acquire / release

**Files:**
- Modify: `c/src/packet_out.c`(追加 `utp_packet_out_pool_acquire`/`utp_packet_out_pool_release` 实现)
- Modify: `c/test/test_packet_out.cc`(追加测试用例)

**Interfaces:**
- Consumes(来自 Task 1):`utp_packet_out_pool_t`、`utp_packet_out_t`(含 `bucket_index`/`alloc_size`/`raw_data`/`encrypt_data`/`loss_chain`/`po_next`)、`utp_packet_out_bucket_t`、`utp_packet_out_buffer_node_t`。
- Produces:
  - `utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size, utp_packet_out_t **out)` —— 成功时 `*out` 指向一个 `raw_data == encrypt_data` 指向所选桶缓冲、`alloc_size` 等于桶容量、`loss_chain == *out`、其余字段全 0 的对象。桶未耗尽但请求尺寸超过最大桶容量返回 `UTP_INTERNAL_ERROR_INVALID_ARGUMENT`;命中的桶或结构体池为空返回 `UTP_INTERNAL_ERROR_LIMIT`。
  - `void utp_packet_out_pool_release(utp_packet_out_pool_t *pool, utp_packet_out_t *pkt)` —— 清空计数/flags/attempts 等状态,保留 `raw_data`/`encrypt_data`/`alloc_size`/`bucket_index`,把对象和缓冲区分别归还 `free_structs`/对应桶的 `free_buffers`。

- [ ] **Step 1: 追加失败测试到 `c/test/test_packet_out.cc`**

```cpp
TEST_CASE("packet_out pool acquire selects the smallest bucket regardless of configuration order",
          "[packet_out][acquire]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{512u, 1u}, {128u, 2u}};  // 故意乱序
    utp_packet_out_t               *pkt_small = nullptr;
    utp_packet_out_t               *pkt_large = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 4u, buckets, 2u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt_small) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pkt_small->alloc_size == 128u);
    REQUIRE(pkt_small->raw_data == pkt_small->encrypt_data);
    REQUIRE(pkt_small->loss_chain == pkt_small);

    REQUIRE(utp_packet_out_pool_acquire(&pool, 200u, &pkt_large) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pkt_large->alloc_size == 512u);

    REQUIRE(utp_packet_out_pool_acquire(&pool, 9000u, &pkt_large) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out pool acquire reports LIMIT when a bucket is exhausted without touching other buckets",
          "[packet_out][acquire]") {
    utp_packet_out_pool_t          pool        = {};
    utp_packet_out_bucket_config_t buckets[]   = {{128u, 1u}, {512u, 1u}};
    utp_packet_out_t               *pkt_small  = nullptr;
    utp_packet_out_t               *pkt_small2 = nullptr;
    utp_packet_out_t               *pkt_large  = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 4u, buckets, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt_small) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt_small2) == UTP_INTERNAL_ERROR_LIMIT);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 500u, &pkt_large) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pkt_large->alloc_size == 512u);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out pool acquire reports LIMIT when the struct pool is exhausted", "[packet_out][acquire]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 4u}};
    utp_packet_out_t               *pkt1     = nullptr;
    utp_packet_out_t               *pkt2     = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt1) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt2) == UTP_INTERNAL_ERROR_LIMIT);

    // 结构体池耗尽时不消耗缓冲区名额:release 后应能再次成功 acquire。
    utp_packet_out_pool_release(&pool, pkt1);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt2) == UTP_INTERNAL_ERROR_OK);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out pool release resets state but preserves the buffer for reuse", "[packet_out][release]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 1u}};
    utp_packet_out_t               *pkt      = nullptr;
    utp_packet_out_t               *pkt2     = nullptr;
    uint8_t                        *original_raw_data;
    uint16_t                        original_alloc_size;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 2u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt) == UTP_INTERNAL_ERROR_OK);

    pkt->po_flags         = UTP_PO_ENCRYPTED;
    pkt->local_flags      = UTP_POL_LOSS;
    pkt->frame_types      = 0xffu;
    pkt->slice_count      = 3u;
    pkt->frame_meta_count = 2u;
    pkt->attempt_count    = 1u;
    original_raw_data     = pkt->raw_data;
    original_alloc_size   = pkt->alloc_size;

    utp_packet_out_pool_release(&pool, pkt);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt2) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(pkt2->raw_data == original_raw_data);
    REQUIRE(pkt2->alloc_size == original_alloc_size);
    REQUIRE(pkt2->po_flags == 0u);
    REQUIRE(pkt2->local_flags == 0u);
    REQUIRE(pkt2->frame_types == 0u);
    REQUIRE(pkt2->slice_count == 0u);
    REQUIRE(pkt2->frame_meta_count == 0u);
    REQUIRE(pkt2->attempt_count == 0u);
    REQUIRE(pkt2->loss_chain == pkt2);

    utp_packet_out_pool_cleanup(&pool);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build --target utp_c_packet_out_test`
Expected: 链接失败(`utp_packet_out_pool_acquire`/`utp_packet_out_pool_release` 未定义)。

- [ ] **Step 3: 在 `c/src/packet_out.c` 中实现 acquire/release(追加到文件末尾)**

```c
static size_t choose_bucket(const utp_packet_out_pool_t *pool, uint16_t requested_size) {
    size_t i;

    for (i = 0u; i < pool->bucket_count; ++i) {
        if (pool->buckets[i].size >= requested_size) {
            return i;
        }
    }
    return pool->bucket_count;
}

utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size,
                                                 utp_packet_out_t **out) {
    size_t                         bucket_index;
    utp_packet_out_bucket_t       *bucket;
    utp_packet_out_buffer_node_t  *node;
    utp_packet_out_t              *pkt;

    if (pool == NULL || out == NULL || requested_size == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    bucket_index = choose_bucket(pool, requested_size);
    if (bucket_index == pool->bucket_count) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    bucket = &pool->buckets[bucket_index];
    if (TAILQ_EMPTY(&bucket->free_buffers) || TAILQ_EMPTY(&pool->free_structs)) {
        return UTP_INTERNAL_ERROR_LIMIT;
    }

    node = TAILQ_FIRST(&bucket->free_buffers);
    TAILQ_REMOVE(&bucket->free_buffers, node, link);

    pkt = TAILQ_FIRST(&pool->free_structs);
    TAILQ_REMOVE(&pool->free_structs, pkt, po_next);

    memset(pkt, 0, sizeof(*pkt));
    pkt->loss_chain   = pkt;
    pkt->raw_data     = node->data;
    pkt->encrypt_data = node->data;
    pkt->alloc_size   = bucket->size;
    pkt->bucket_index = bucket_index;

    *out = pkt;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_packet_out_pool_release(utp_packet_out_pool_t *pool, utp_packet_out_t *pkt) {
    utp_packet_out_bucket_t *bucket;
    uint8_t                 *raw_data;
    uint8_t                 *encrypt_data;
    uint16_t                 alloc_size;
    size_t                   bucket_index;
    size_t                   node_index;

    if (pool == NULL || pkt == NULL) {
        return;
    }

    raw_data     = pkt->raw_data;
    encrypt_data = pkt->encrypt_data;
    alloc_size   = pkt->alloc_size;
    bucket_index = pkt->bucket_index;
    bucket       = &pool->buckets[bucket_index];

    memset(pkt, 0, sizeof(*pkt));
    pkt->loss_chain   = pkt;
    pkt->raw_data     = raw_data;
    pkt->encrypt_data = encrypt_data;
    pkt->alloc_size   = alloc_size;
    pkt->bucket_index = bucket_index;
    TAILQ_INSERT_TAIL(&pool->free_structs, pkt, po_next);

    node_index = (size_t)(raw_data - bucket->storage) / bucket->size;
    TAILQ_INSERT_TAIL(&bucket->free_buffers, &bucket->nodes[node_index], link);
}
```

- [ ] **Step 4: 重新构建并运行测试**

Run: `cmake --build build --target utp_c_packet_out_test && ctest --test-dir build -R utp_c_packet_out_test --output-on-failure`
Expected: 全部 `TEST_CASE`(含 Task 1 的)PASS。

- [ ] **Step 5: Commit**

```bash
git add c/src/packet_out.c c/test/test_packet_out.cc
git commit -m "feat(c): packet_out acquire/release(最小可容纳桶选取 + 状态复位)"
```

---

## Task 3: add_send_attempt / clear_send_attempts + frame_types 互操作测试

**Files:**
- Modify: `c/src/packet_out.c`(追加 `utp_packet_out_add_send_attempt`/`utp_packet_out_clear_send_attempts` 实现)
- Modify: `c/test/test_packet_out.cc`(追加测试用例)

**Interfaces:**
- Consumes(来自 Task 1/2):`utp_packet_out_t{attempts[UTP_PACKET_OUT_MAX_ATTEMPTS], attempt_count}`;`UTP_FRAME_BIT`/`utp_frame_type_t`(`c/src/internal/frame.h`,通过在测试里额外 `#include "internal/frame.h"` 引入)。
- Produces:
  - `bool utp_packet_out_add_send_attempt(utp_packet_out_t *pkt, uint64_t packet_number, uint64_t sent_time_us)` —— 已达 `UTP_PACKET_OUT_MAX_ATTEMPTS` 时返回 `false` 且不修改任何字段;否则追加一条记录并返回 `true`。
  - `void utp_packet_out_clear_send_attempts(utp_packet_out_t *pkt)` —— 将 `attempt_count` 置 0(不清空 `attempts[]` 内容,仅逻辑清空,与 `release()` 的物理清零职责区分)。

- [ ] **Step 1: 追加失败测试到 `c/test/test_packet_out.cc`**

```cpp
TEST_CASE("packet_out add_send_attempt records attempts and caps at the configured maximum",
          "[packet_out][attempts]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 1u}};
    utp_packet_out_t               *pkt      = nullptr;
    size_t                          i;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt) == UTP_INTERNAL_ERROR_OK);

    for (i = 0u; i < UTP_PACKET_OUT_MAX_ATTEMPTS; ++i) {
        REQUIRE(utp_packet_out_add_send_attempt(pkt, 10u + i, 1000u + i) == true);
    }
    REQUIRE(pkt->attempt_count == UTP_PACKET_OUT_MAX_ATTEMPTS);
    REQUIRE(pkt->attempts[0].packet_number == 10u);

    REQUIRE(utp_packet_out_add_send_attempt(pkt, 999u, 9999u) == false);
    REQUIRE(pkt->attempt_count == UTP_PACKET_OUT_MAX_ATTEMPTS);
    REQUIRE(pkt->attempts[0].packet_number == 10u);  // 未被溢出写入破坏

    utp_packet_out_clear_send_attempts(pkt);
    REQUIRE(pkt->attempt_count == 0u);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out frame_types composes with the existing UTP_FRAME_BIT macro", "[packet_out][frame_types]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 1u}};
    utp_packet_out_t               *pkt      = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt) == UTP_INTERNAL_ERROR_OK);

    pkt->frame_types |= UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM);
    pkt->frame_types |= UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK);

    REQUIRE((pkt->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_STREAM)) != 0u);
    REQUIRE((pkt->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_ACK)) != 0u);
    REQUIRE((pkt->frame_types & UTP_FRAME_BIT(UTP_FRAME_TYPE_PADDING)) == 0u);

    utp_packet_out_pool_cleanup(&pool);
}
```

同时在文件顶部的 `extern "C" { ... }` 块中追加 `#include "internal/frame.h"`。

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build build --target utp_c_packet_out_test`
Expected: 链接失败(`utp_packet_out_add_send_attempt`/`utp_packet_out_clear_send_attempts` 未定义)。

- [ ] **Step 3: 在 `c/src/packet_out.c` 中实现(追加到文件末尾)**

```c
bool utp_packet_out_add_send_attempt(utp_packet_out_t *pkt, uint64_t packet_number, uint64_t sent_time_us) {
    if (pkt == NULL || pkt->attempt_count >= UTP_PACKET_OUT_MAX_ATTEMPTS) {
        return false;
    }
    pkt->attempts[pkt->attempt_count].packet_number = packet_number;
    pkt->attempts[pkt->attempt_count].sent_time_us  = sent_time_us;
    ++pkt->attempt_count;
    return true;
}

void utp_packet_out_clear_send_attempts(utp_packet_out_t *pkt) {
    if (pkt != NULL) {
        pkt->attempt_count = 0u;
    }
}
```

- [ ] **Step 4: 重新构建并运行全部测试**

Run: `cmake --build build --target utp_c_packet_out_test && ctest --test-dir build -R utp_c_packet_out_test --output-on-failure`
Expected: 全部 `TEST_CASE` PASS(累计 Task 1+2+3 共 9 个用例)。

- [ ] **Step 5: 跑一次全量 CTest,确认没有破坏既有模块**

Run: `ctest --test-dir build --output-on-failure`
Expected: 全部既有测试(`utp_c_api_test`、`utp_c_public_api_test`、`utp_c_proto_test`、`utp_c_crypto_test`、`utp_c_receive_history_test`、`utp_c_packet_out_test`)PASS。

- [ ] **Step 6: Commit**

```bash
git add c/src/packet_out.c c/test/test_packet_out.cc
git commit -m "feat(c): packet_out add_send_attempt/clear_send_attempts + frame_types 互操作测试"
```
