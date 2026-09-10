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
- **lsquic**(`/Users/eular/Github/lsquic`,`src/liblsquic/lsquic_packet_out.{h,c}`、`lsquic_mm.{h,c}`)—— 生产级 C QUIC 实现,提供了"Connection 私有定长对象 slab + Engine 共享分桶数据缓冲池"这一关键设计。C 端采用相同职责切分:发送包描述符归 Connection,可变大小数据缓冲归 Context;两者均按固定批次惰性增长。

### 2.1 lsquic 设计细节(具体依据)

`lsquic_mm.{h,c}` 对 `lsquic_packet_out_t` 采用**两套独立池**,与本设计的"结构体池 + 缓冲池"划分一一对应:

- **结构体池**:Engine 有默认 `malo.packet_out`,但完整 Connection 会建立自己的 `packet_out_malo`;`lsquic_mm_get_packet_out()` 可接收该 Connection 私有 `malo`。Connection 销毁时可整体释放其描述符 slab。
- **缓冲池**:`mm->packet_out_bufs` 是 `SLIST_HEAD(, packet_out_buf) packet_out_bufs[MM_N_OUT_BUCKETS]`,`MM_N_OUT_BUCKETS = 5`,按大小分为 5 档(`lsquic_mm.c`):
  - `PACKET_OUT_PAYLOAD_0`:`1280 - overhead`(IPv6 最小 MTU 档)
  - `PACKET_OUT_PAYLOAD_1`:`GQUIC_MAX_IPv6_PACKET_SZ - overhead`
  - `PACKET_OUT_PAYLOAD_2`:`GQUIC_MAX_IPv4_PACKET_SZ - overhead`
  - `PACKET_OUT_PAYLOAD_3`:`4096`
  - `PACKET_OUT_PAYLOAD_4`:`0xffff`(65535,巨帧/回环上限)

  `packet_out_index()` 用线性阈值比较从请求大小映射到桶下标;`lsquic_mm_get_packet_out()` 按此下标从对应 `SLIST` 弹出一块空闲缓冲,`lsquic_mm_put_packet_out()` 释放时按原桶下标压回对应 `SLIST`。桶为空时 lsquic 走**惰性 grow**(现场 `malloc` 补充),并有周期性采样的 `maybe_shrink_packet_out_bufs()` 做收缩。

- **本设计与之的取舍**:桶下标同样选择能容纳请求大小的最小桶,但不引入通用 `malo`:
  1. 桶边界直接固定为 `{1280, 1500, 4096, 9000, 65535}`。这是协议代码的编译期策略,新增或删除档位直接修改模块代码,不提供调用方配置 API。
  2. Connection 描述符块和 Context 缓冲块都以 32 个为一批惰性申请。空闲对象立即复用;没有最大连接数或固定 PacketOut 上限,分配器失败才返回 `UTP_INTERNAL_ERROR_NOMEM`。
  3. Context 缓冲桶每 1024 次借还采样峰值。当长期峰值低于已分配对象数的四分之一时,释放一半完全空闲的 32 块批次;Connection 描述符块仅在 Connection 销毁时整体释放。

## 3. 与 cpp 的关键差异

| 点 | cpp | C 版本 | 原因 |
|---|---|---|---|
| `encrypt_data` 生命周期 | 运行时单独申请,需 `AesGcmContext::ReleaseEncryptBuffer` 释放 | 池化的按桶缓冲,acquire/release 无独立释放路径 | C 端 `crypto.h` 的 AEAD API 本就要求调用方提供输出缓冲区(见 `utp_crypto_aead_seal`),不需要加密上下文自管理输出内存 |
| `frame_meta`/`frames` 联合体压缩(`FrameMetaInfo one` vs `FrameMetaVec vec`,64 字节技巧) | 有 | 无,直接用定长数组 `frame_meta[8]` | 内存微优化,无实测数据前不做;未来有性能需求再引入 |
| `PacketOutAttempt` 重传尝试记录 | 链表节点(疑似另经 malo 池分配) | `send_control` 按 32 项块维护包号索引 | 重传次数不设 PacketOut 固定上限；索引归可靠发送层管理，PacketOut 仅保存所属尝试链。 |
| 数据缓冲区尺寸档位 | 无分档(单个连接一份按需大小的 buffer) | Context 共享固定五档 `{1280,1500,4096,9000,65535}`,每档按 32 块惰性增长 | 常规 MTU 路径只会触发一个桶;共享池避免把未使用的大包桶复制到每条 Connection,顶档仍覆盖回环等特殊路径 |
| 队列基础设施 | `<queue.h>`(BSD `sys/queue.h`)TAILQ,裸指针 | `3rd/queue.h`(已在仓库内,cpp 同款,纯标准 C 宏、无 GNU 扩展)TAILQ,裸指针,由固定容量结构体池支撑 | 与 cpp 结构直接对齐,便于未来做差分/一致性测试;`3rd/queue.h` 是仓库既有 vendored 依赖而非编译器扩展,不违反 STYLE.md "no compiler extensions" |
| `po_flags`/`po_lflags` 位域打包(lsquic 用移位宏如 `POBIT_SHIFT`/`POIPv6_SHIFT`/`POPNS_SHIFT` 把多个子字段压进同一整数) | (cpp 是独立 bool/enum 字段,非位域打包) | `po_flags`/`local_flags` 用简单位图枚举(`UTP_PO_*`/`UTP_POL_*`,每个语义一个独立 bit),不做 lsquic 式的多值子字段位域打包 | lsquic 的位域打包是为了在大量并发连接下压缩单包内存占用;本项目当前无此内存压力实测数据,位图枚举可读性更好、`-Wconversion -Wpedantic` 下也更不容易出移位错误,后续如有实测需要再收紧 |
| `frame_rec`/`frame_rec_arr`(lsquic 用 union 区分"包内只有一个 stream 帧"与"多个帧"两种情况,对齐 cpp 的 `FrameMetaInfo`/`FrameMetaVec` 压缩技巧) | `FrameMetaInfo one` / `FrameMetaVec vec` union | 不做 union 压缩,直接用定长数组 `frame_meta[8]` | 同上,属于内存微优化,两个参考实现(cpp 与 lsquic)都做了但本设计明确推迟,已在表格首行说明 |

## 4. 数据结构

```c
#define UTP_PACKET_OUT_MAX_FRAMES    8u
#define UTP_PACKET_OUT_MAX_SLICES    8u
#define UTP_PACKET_OUT_MAX_BUCKETS   8u   // 内部固定桶数组容量，当前启用五档

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
    struct utp_send_attempt_node *attempts;
    uint32_t                      attempt_count;

    void *bw_state;   // 不透明指针,congestion 子项目(#2)填充,本模块不解引用

    uint8_t *raw_data;       // 指向池中按桶分配的缓冲(明文 + 包头)
    uint8_t *encrypt_data;   // 指向池中按桶分配的缓冲(密文输出)
} utp_packet_out_t;

TAILQ_HEAD(utp_packet_out_tailq, utp_packet_out);
```

## 5. 池设计

### 5.1 Context/Connection 两级所有权

- **Connection 描述符池**:`utp_packet_out_pool_t` 只管理 `utp_packet_out_t`。空闲链为空时申请一个含 32 个描述符的连续 slab。对象只能由所属 Connection 借出和归还，Connection 销毁时释放全部 slab。
- **Context 缓冲池**:`utp_packet_out_buffer_pool_t` 管理 `raw_data`/`encrypt_data`。所有 Connection 共用五个固定桶:`1280`、`1500`、`4096`、`9000`、`65535`。桶空时申请一个包含 32 个节点和 32 块同尺寸缓冲的连续块。

这种切分与 lsquic 一致:描述符包含发送控制、重传和流元数据，具有强 Connection 生命周期；数据缓冲只有容量属性，放在 Context 可被任一空闲 Connection 立即复用。Connection 初始化时由 Context 显式传入缓冲池，不保留私有缓冲池 fallback。Context 当前按事件线程串行访问，无需为共享桶加锁。

初始化只建立空闲链和固定桶元数据，不申请发送描述符或数据缓冲。Context 不设置最大连接数或 PacketOut 固定容量；发送控制队列采用 `SIZE_MAX` 的实际无上限策略，历史发送索引独立按 32 项块增长。分配器无法扩容时才返回 `UTP_INTERNAL_ERROR_NOMEM`。

### 5.2 acquire / release 与收缩

```c
utp_internal_error_t utp_packet_out_buffer_pool_init(utp_packet_out_buffer_pool_t *pool,
                                                     const utp_allocator_t *allocator);
utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool,
                                              const utp_allocator_t *allocator);
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool,
                                                 utp_packet_out_buffer_pool_t *buffer_pool,
                                                 uint16_t requested_size, utp_packet_out_t **out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t *pool,
                                                 utp_packet_out_buffer_pool_t *buffer_pool,
                                                 utp_packet_out_t *pkt);
```

- `acquire` 先按最小容纳原则选择固定桶；描述符 slab 或对应缓冲桶为空时，各自一次扩容 32 个对象。两者任何一步失败都回滚已借资源并返回 `UTP_INTERNAL_ERROR_NOMEM`。
- `release` 清空发送状态并把描述符放回所属 Connection、缓冲放回 Context 对应桶。缓冲的桶号和节点在 `utp_packet_out_t` 中记录，因此归还不需要地址反查或线性扫描。
- 每桶每 1024 次借还更新一次峰值移动平均。当平均峰值低于当前分配量的四分之一时，最多释放一半**完全空闲**的 32 块批次；正在使用的块绝不移动或释放。这样没有连接数上限时，历史突发也不会永久占住 Context 内存。

### 5.3 队列职责边界

`packet_out` 模块只提供对象 + 池 + `po_next` 挂载点。发送尝试索引以及 unacked/scheduled/lost/buffered 四个 `TAILQ_HEAD` 均由 `send_control` 管理。

## 6. API 汇总

```c
utp_internal_error_t utp_packet_out_buffer_pool_init(utp_packet_out_buffer_pool_t *pool,
                                                     const utp_allocator_t *allocator);
void                 utp_packet_out_buffer_pool_cleanup(utp_packet_out_buffer_pool_t *pool);
utp_internal_error_t utp_packet_out_pool_init(utp_packet_out_pool_t *pool,
                                              const utp_allocator_t *allocator);
void                 utp_packet_out_pool_cleanup(utp_packet_out_pool_t *pool);
utp_internal_error_t utp_packet_out_pool_acquire(utp_packet_out_pool_t *pool,
                                                 utp_packet_out_buffer_pool_t *buffer_pool,
                                                 uint16_t requested_size, utp_packet_out_t **out);
void                 utp_packet_out_pool_release(utp_packet_out_pool_t *pool,
                                                 utp_packet_out_buffer_pool_t *buffer_pool, utp_packet_out_t *pkt);
```

## 7. 错误处理

- 公开输入和资源耗尽路径返回 `utp_internal_error_t`,不记录 payload 内容(遵循 `c/STYLE.md` / `c/ERRORS.md`)；内部对象归还依赖 acquire/release 所建立的不变量。
- `acquire` 不存在固定容量耗尽；32 个一批的扩容失败时返回 `UTP_INTERNAL_ERROR_NOMEM`，请求大小为零时返回 `UTP_INTERNAL_ERROR_INVALID_ARGUMENT`。

## 8. 测试计划

新增 `c/test/test_packet_out.cc`,按 `test_receive_history.cc` 的风格接入 `utp_c_configure_cpp_test`:

- 初始化不分配；32 个一批的描述符和缓冲惰性增长、分配失败回滚。
- `acquire` 按五档固定最小可容纳桶选取，覆盖 `uint16_t` 上限。
- 两个 Connection 描述符池共用一个 Context 缓冲池；释放后第二个 Connection 不触发缓冲扩容。
- 长期空闲的完整缓冲块在采样后收缩，仍被借出的块不得收缩。
- `release` 清空 flags/frame_types/发送尝试关联/frame_meta_count 等状态,并在下次 acquire 时重新绑定 `raw_data`/`encrypt_data` 与 `alloc_size`。
- `frame_types` 位图与既有 `UTP_FRAME_BIT()` 的往返一致性。

## 9. 范围外

- unacked/scheduled/lost/buffered 队列编排、丢包检测、RTO/TLP 定时器 —— 属于 send_ctl 子项目(#3)。
- `bw_state` 指向的带宽采样状态、`Congestion`/`Pacer` 接口本身 —— 属于 pacer/congestion 子项目(#2)。
- stream 层(`frame_meta[].owner` 的实际使用)—— 未来 stream 状态机落地时再接入,本设计只保证字段存在且类型正确。
