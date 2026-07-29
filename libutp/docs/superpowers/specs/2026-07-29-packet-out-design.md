# C 端 packet_out(发送包对象 + 池)设计

- 日期:2026-07-29
- 状态:设计定稿,待写实现计划
- 范围:`c/` 下新增 `utp_packet_out_t` 发送包对象与 `utp_packet_out_pool_t` 池(仅对象生命周期 + 缓冲区管理),不含队列编排、拥塞控制、限速逻辑

---

## 1. 背景与上位任务拆分

用户要求"继续可靠性层"最终定位到:C 端已有 `rtt.c`/`send_history.c`/`receive_history.c`/`ack_scheduler.c` 构成完整的接收侧 ACK 生成 + RTT 估计管线,但发送侧"已发送包跟踪 + 丢包检测 + 重传调度"(对应 cpp 的 `SendControl`)整体缺失。

cpp `SendControl`(`cpp/src/context/send_ctl.{h,cpp}`,1760+244 行)自身耦合了 `PacketOut` 发送包队列、`Congestion` 拥塞算法接口、`Pacer` 限速器 —— 三者在 C 端均不存在。用户明确要求"完整实现,不能因为耦合多就不实现",但受限于依赖顺序(send_ctl 依赖 packet_out 与 congestion/pacer 才能真正跑起来),拆分为三个子项目、各自走 spec → plan → implementation:

1. **packet_out**(本文档) —— 其余两者都要用到,必须先做。
2. **pacer / congestion** —— 体量较小、依赖少,参考 cpp `congestion/*` 与 lsquic 对应实现。
3. **send_ctl 核心** —— 用真实的 congestion/pacer 接口把三者接起来,不做占位桩。

本文档仅覆盖第 1 步。

## 2. 参考实现

- **cpp 自身**:`cpp/src/proto/packet_out.{h,cpp}`(150+79 行)、`cpp/src/proto/packet_common.h` —— 字段语义、flags 含义、`loss_chain`/`frame_meta` 概念的直接来源。
- **lsquic**(`/Users/eular/Github/lsquic`,`src/liblsquic/lsquic_packet_out.{h,c}`、`lsquic_mm.{h,c}`)—— 生产级 C QUIC 实现,提供了"定长结构体池 + 按大小分桶的数据缓冲池"这一关键设计,解决了"运行期零分配"与"支持可变/巨帧 MTU"之间的张力。C 端本设计的池结构直接借鉴其 `packet_out_bufs[N_BUCKETS]` 分桶思路,但改为**桶内容量在池初始化时一次性预分配到位**(lsquic 是运行时惰性 grow/shrink),以满足本项目 `c/STYLE.md` "established-connection 的收发路径不得分配内存,内存边界在 Context/Connection 创建时确定"的更严格要求。

### 2.1 lsquic 设计细节(具体依据)

`lsquic_mm.{h,c}` 对 `lsquic_packet_out_t` 采用**两套独立池**,与本设计的"结构体池 + 缓冲池"划分一一对应:

- **结构体池**:用 `malo`(内部 slab 分配器)分配定长的 `lsquic_packet_out_t` 本体,`lsquic_mm_get_packet_out()` 内部调用 `lsquic_malo_get(mm->malo.packet_out)`。
- **缓冲池**:`mm->packet_out_bufs` 是 `SLIST_HEAD(, packet_out_buf) packet_out_bufs[MM_N_OUT_BUCKETS]`,`MM_N_OUT_BUCKETS = 5`,按大小分为 5 档(`lsquic_mm.c`):
  - `PACKET_OUT_PAYLOAD_0`:`1280 - overhead`(IPv6 最小 MTU 档)
  - `PACKET_OUT_PAYLOAD_1`:`GQUIC_MAX_IPv6_PACKET_SZ - overhead`
  - `PACKET_OUT_PAYLOAD_2`:`GQUIC_MAX_IPv4_PACKET_SZ - overhead`
  - `PACKET_OUT_PAYLOAD_3`:`4096`
  - `PACKET_OUT_PAYLOAD_4`:`0xffff`(65535,巨帧/回环上限)

  `packet_out_index()` 用线性阈值比较从请求大小映射到桶下标;`lsquic_mm_get_packet_out()` 按此下标从对应 `SLIST` 弹出一块空闲缓冲,`lsquic_mm_put_packet_out()` 释放时按原桶下标压回对应 `SLIST`。桶为空时 lsquic 走**惰性 grow**(现场 `malloc` 补充),并有周期性采样的 `maybe_shrink_packet_out_bufs()` 做收缩。

- **本设计与之的取舍**:采用同样的"结构体池 + 按大小分桶的缓冲池"两级结构、桶下标选择逻辑(取能容纳请求大小的最小桶),但:
  1. 桶边界**不写死**,改为调用方在 `utp_packet_out_pool_init()` 时以 `(size,count)` 数组传入(最多 8 档),因为 STYLE.md 要求 established 连接路径不得分配内存,预置多少档、多大规格必须由上层按场景决定,不应在 packet_out 模块内固化 lsquic 面向的 QUIC/IPv4/IPv6 MTU 假设。
  2. 不做惰性 grow/shrink:所有桶在 `pool_init` 时一次性分配到位,耗尽即返回 `UTP_INTERNAL_ERROR_LIMIT`,不现场 `malloc`,不做采样收缩 —— 这是本项目比 lsquic 更严格的"零运行期分配"要求所致。
  3. 结构体池不引入类似 `malo` 的通用 slab 分配器,直接用定长数组 + `TAILQ` 空闲链,足够覆盖当前唯一的定长对象类型,避免额外抽象层。

## 3. 与 cpp 的关键差异

| 点 | cpp | C 版本 | 原因 |
|---|---|---|---|
| `encrypt_data` 生命周期 | 运行时单独申请,需 `AesGcmContext::ReleaseEncryptBuffer` 释放 | 池化的按桶缓冲,acquire/release 无独立释放路径 | C 端 `crypto.h` 的 AEAD API 本就要求调用方提供输出缓冲区(见 `utp_crypto_aead_seal`),不需要加密上下文自管理输出内存 |
| `frame_meta`/`frames` 联合体压缩(`FrameMetaInfo one` vs `FrameMetaVec vec`,64 字节技巧) | 有 | 无,直接用定长数组 `frame_meta[8]` | 内存微优化,无实测数据前不做;未来有性能需求再引入 |
| `PacketOutAttempt` 重传尝试记录 | 链表节点(疑似另经 malo 池分配) | 定长数组 `attempts[4]` | 避免额外的节点池,`4` 覆盖 `MAX_RESUBMITTED_ON_RTO=2` 并留余量(见 `utp-04-reliability-ack.md` §5) |
| 数据缓冲区尺寸档位 | 无分档(单个连接一份按需大小的 buffer) | 按桶预分配,桶位由调用方配置(参考 lsquic 5 档,上限 65535) | 本机回环(127.0.0.1)等场景 MTU 可达 65536,且 C 端尺寸字段本身是 `uint16_t`(上限 65535);单一固定尺寸桶要么浪费内存要么无法覆盖巨帧场景 |
| 队列基础设施 | `<queue.h>`(BSD `sys/queue.h`)TAILQ,裸指针 | `3rd/queue.h`(已在仓库内,cpp 同款,纯标准 C 宏、无 GNU 扩展)TAILQ,裸指针,由固定容量结构体池支撑 | 与 cpp 结构直接对齐,便于未来做差分/一致性测试;`3rd/queue.h` 是仓库既有 vendored 依赖而非编译器扩展,不违反 STYLE.md "no compiler extensions" |
| `po_flags`/`po_lflags` 位域打包(lsquic 用移位宏如 `POBIT_SHIFT`/`POIPv6_SHIFT`/`POPNS_SHIFT` 把多个子字段压进同一整数) | (cpp 是独立 bool/enum 字段,非位域打包) | `po_flags`/`local_flags` 用简单位图枚举(`UTP_PO_*`/`UTP_POL_*`,每个语义一个独立 bit),不做 lsquic 式的多值子字段位域打包 | lsquic 的位域打包是为了在大量并发连接下压缩单包内存占用;本项目当前无此内存压力实测数据,位图枚举可读性更好、`-Wconversion -Wpedantic` 下也更不容易出移位错误,后续如有实测需要再收紧 |
| `frame_rec`/`frame_rec_arr`(lsquic 用 union 区分"包内只有一个 stream 帧"与"多个帧"两种情况,对齐 cpp 的 `FrameMetaInfo`/`FrameMetaVec` 压缩技巧) | `FrameMetaInfo one` / `FrameMetaVec vec` union | 不做 union 压缩,直接用定长数组 `frame_meta[8]` | 同上,属于内存微优化,两个参考实现(cpp 与 lsquic)都做了但本设计明确推迟,已在表格首行说明 |

## 4. 数据结构

```c
#define UTP_PACKET_OUT_MAX_FRAMES    8u
#define UTP_PACKET_OUT_MAX_SLICES    8u
#define UTP_PACKET_OUT_MAX_ATTEMPTS  4u
#define UTP_PACKET_OUT_MAX_BUCKETS   8u   // 池可配置的最大桶位数

typedef struct utp_packet_out_attempt {
    uint64_t packet_number;
    uint64_t sent_time_us;
} utp_packet_out_attempt_t;

typedef struct utp_frame_meta_info {
    void    *owner;          // 不透明指针;stream 层落地前恒为 NULL,本模块不解引用
    uint16_t offset;
    uint16_t length;
    uint8_t  frame_type;      // 复用 c/src/internal/frame.h 的 utp_frame_type_t
    uint8_t  frame_flags;     // UTP_FRAME_META_* 位图(新增,语义对齐 cpp FrameMetaFlags)
} utp_frame_meta_info_t;

typedef struct utp_packet_out_slice {
    uint16_t    offset;
    uint16_t    length;
    const void *data;
    uint8_t     source;       // UTP_PACKET_OUT_SLICE_RAW_OFFSET / _EXTERNAL
} utp_packet_out_slice_t;

typedef struct utp_packet_out {
    TAILQ_ENTRY(utp_packet_out) po_next;   // unacked/scheduled/lost/buffered 四队列复用同一挂载点(互斥归属,对齐 cpp)

    uint64_t sent_time_us;
    uint64_t packet_number;
    uint64_t ack_number;                   // 若含 ACK 帧,记录其中最大已确认包号
    struct utp_packet_out *loss_chain;      // 丢包链(自环初始化),send_ctl 子项目使用

    uint32_t frame_types;                  // 复用 UTP_FRAME_BIT(),无需新枚举
    uint16_t po_flags;                     // UTP_PO_* 位图(对齐 cpp PacketOutFlags)
    uint16_t local_flags;                  // UTP_POL_* 位图(对齐 cpp PacketOutLocalFlags)

    uint16_t data_size;
    uint16_t encrypt_data_size;
    uint16_t alloc_size;                   // 本槽位实际占用的桶容量
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

    void *bw_state;   // 不透明指针,congestion 子项目(#2)填充,本模块不解引用

    uint8_t *raw_data;       // 指向池中按桶分配的缓冲(明文 + 包头)
    uint8_t *encrypt_data;   // 指向池中按桶分配的缓冲(密文输出)
} utp_packet_out_t;

TAILQ_HEAD(utp_packet_out_tailq, utp_packet_out);
```

## 5. 池设计

### 5.1 两级池

- **结构体池**:`utp_packet_out_t` 定长,`utp_packet_out_pool_init()` 按 `struct_capacity` 一次性分配数组 + 空闲链(`TAILQ_ENTRY`)。
- **数据缓冲池**:`raw_data`/`encrypt_data` 从独立的、按大小分桶的缓冲池中获取。桶位**完全由调用方配置**(不在项目里硬编码档位),形如:

```c
typedef struct utp_packet_out_bucket_config {
    uint16_t size;    // 该桶的缓冲区大小(降序或升序均可,init 时内部排序)
    size_t   count;   // 该桶预分配的缓冲区数量
} utp_packet_out_bucket_config_t;

utp_internal_error_t utp_packet_out_pool_init(
    utp_packet_out_pool_t *pool, const utp_allocator_t *allocator,
    size_t struct_capacity,
    const utp_packet_out_bucket_config_t *buckets, size_t bucket_count /* <= UTP_PACKET_OUT_MAX_BUCKETS */);
```

推荐(非强制)默认配置,参考 lsquic 的 5 档并把顶档抬到 `uint16_t` 上限:`{1280, 1500, 4096, 9000, 65535}`。回环等特殊路径可由调用方传入自定义档位。

所有桶在 `pool_init` 时**一次性预分配到位**,不支持运行时扩容/收缩,满足 STYLE.md 的分配时机约束。

### 5.2 acquire / release

```c
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size,
                                                 utp_packet_out_t **out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t *pool, utp_packet_out_t *pkt);
```

- `acquire` 选择能容纳 `requested_size` 的最小桶;若该档位空闲缓冲耗尽,直接返回 `UTP_INTERNAL_ERROR_LIMIT`,不跨桶借用、不回退到 `utp_allocator_t` 之外的分配。
- `release` 等价于 cpp 的 `reset()`:清空计数/flags/attempts,缓冲区按其桶归位复用,`loss_chain` 复位为自环。

### 5.3 队列职责边界

`packet_out` 模块只提供对象 + 池 + `po_next` 挂载点。unacked/scheduled/lost/buffered 四个 `TAILQ_HEAD` 由 send_ctl 子项目(#3)声明和编排 —— 保持 STYLE.md "小接口单一职责"。

## 6. API 汇总

```c
utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool, const utp_allocator_t *allocator,
                                              size_t struct_capacity,
                                              const utp_packet_out_bucket_config_t *buckets, size_t bucket_count);
void                 utp_packet_out_pool_cleanup(utp_packet_out_pool_t *pool);
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool, uint16_t requested_size,
                                                 utp_packet_out_t **out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t *pool, utp_packet_out_t *pkt);

bool utp_packet_out_add_send_attempt(utp_packet_out_t *pkt, uint64_t packet_number, uint64_t sent_time_us);
void utp_packet_out_clear_send_attempts(utp_packet_out_t *pkt);
```

## 7. 错误处理

- 所有越界/耗尽路径返回 `utp_internal_error_t`,不 `abort`,不记录 payload 内容(遵循 `c/STYLE.md` / `c/ERRORS.md`)。
- `acquire` 在结构体池或对应桶耗尽时均返回 `UTP_INTERNAL_ERROR_LIMIT`。
- `add_send_attempt` 在 `attempt_count == UTP_PACKET_OUT_MAX_ATTEMPTS` 时返回 `false`(不计数、不报错,对齐 cpp 语义:达到上限后静默忽略新尝试记录)。

## 8. 测试计划

新增 `c/test/test_packet_out.cc`,按 `test_receive_history.cc` 的风格接入 `utp_c_configure_cpp_test`:

- 池初始化/清理;多桶配置的边界值(0 个桶、`bucket_count > UTP_PACKET_OUT_MAX_BUCKETS`、单桶、桶大小超过 `uint16_t` 上限)。
- `acquire` 按最小可容纳桶选取;某桶耗尽返回 `LIMIT`,不跨桶借用。
- `release` 后重新 `acquire` 复用同一块缓冲区(验证池确实复用,而非静默泄漏/新分配)。
- `release` 清空 flags/frame_types/attempts/frame_meta_count 等状态,但保留 `raw_data`/`encrypt_data` 指针与 `alloc_size`。
- `add_send_attempt` 计数封顶行为。
- `frame_types` 位图与既有 `UTP_FRAME_BIT()` 的往返一致性。

## 9. 范围外

- unacked/scheduled/lost/buffered 队列编排、丢包检测、RTO/TLP 定时器 —— 属于 send_ctl 子项目(#3)。
- `bw_state` 指向的带宽采样状态、`Congestion`/`Pacer` 接口本身 —— 属于 pacer/congestion 子项目(#2)。
- stream 层(`frame_meta[].owner` 的实际使用)—— 未来 stream 状态机落地时再接入,本设计只保证字段存在且类型正确。
