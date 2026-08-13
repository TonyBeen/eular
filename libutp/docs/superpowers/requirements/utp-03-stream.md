# utp-03 流（Stream）需求文档（代码反推）

> Ground truth：`cpp/` 源码。`doc/` 仅交叉参考，不一致以代码为准并在 §8 标注。
> 主要文件：
> - `cpp/src/context/stream_impl.{h,cpp}`（流实现、FIN 语义、发送预算、聚合、接收重组红黑树）
> - `cpp/src/util/ring_buffer.{h,cpp}`（发送侧环形缓冲）
> - `cpp/src/util/receive_history.{h,cpp}`（接收包号历史，用于 ACK，非流数据重组）
> - `cpp/include/utp/stream.h`（对外 `Stream` 抽象）
> - `cpp/src/proto/frame/stream.h`（STREAM 帧结构）
> - `cpp/src/context/connection_impl.cpp`（StreamID 分配、多路复用调度）
> - `cpp/include/utp/config.h`、`cpp/src/util/transport_param.h`（参数默认值）

---

## 1. 职责与边界

`StreamImpl`（`cpp/src/context/stream_impl.h:49`，继承 `Stream`）代表一个连接内的逻辑流，负责：

- 应用侧读写接口：拷贝式 `write/read`，零拷贝式 `acquireWriteBuffer/commitWrite`、`acquireReadViews/commitReadViews`（`stream_impl.cpp:88-338`）。
- 发送缓冲管理：`RingBuffer m_sendBuffer`（`stream_impl.h:135`），维护未发送/在途/已确认偏移。
- 接收重组：以流偏移为 key 的红黑树 `m_recvFragmentsTree`（`stream_impl.h:136`），零拷贝持有底层 `PacketIn`（引用计数）。
- FIN / RESET 结束语义与状态机（`stream_impl.cpp:340-352`、`371-398`、`634-654`）。
- 小包聚合（coalescing）延迟决策（`stream_impl.cpp:873-937`）。
- 回调：可读/可写/关闭/重置（`stream_impl.cpp:436-456`）。

**边界（不属于本模块，由 `ConnectionImpl` 承担）**：
- StreamID 的分配与合法性校验（`connection_impl.cpp:1500-1545`、`3565-3626`）。
- 多流之间的调度/多路复用（STRICT / DRR / DISABLED，`connection_impl.cpp:3637-3833`）。
- 流量控制窗口（MAX_STREAM_DATA / MAX_DATA / *_BLOCKED）与实际帧封装发送（`connection_impl.cpp:1771` 等）。
- 底层 `PacketIn` 内存池、RESET_STREAM 帧编解码。

`StreamImpl` 通过持有的 `m_conn`（`ConnectionImpl*`）回调连接层：`scheduleWrite()`、`nextScheduleTime()`、`sendStreamFrame()`、`sendResetStreamFrame()`、`onStreamBytesConsumed()`、`onStreamDataSent()`、`streamPayloadBudgetHint()`。

---

## 2. 数据结构 / StreamID 规则

### 2.1 STREAM 帧（`cpp/src/proto/frame/stream.h:18-33`）

字段：`stream_flag(u8)`、`stream_data_length(u16)`、`stream_id(u32)`、`stream_offset(u64)`、`stream_data(ptr)`。
帧头大小 `FRAME_STREAM_HDR_SIZE = 1+1+2+4+8 = 16` 字节（`stream.h:14`）。
- `stream_id` 为 **32 位**（注意：对外接口/帧均为 u32，见 §8 与 doc 差异）。
- `stream_data_length` u16 → 单帧 payload ≤ `UINT16_MAX`（发送侧 `payloadLen = min(view.len, UINT16_MAX)`，`stream_impl.cpp:723`）。
- FIN 位由 `STREAM_IS_FIN(stream_flag)` 判定；早期数据构造用 `stream_flag = fin ? 0x01 : 0x00`（`connection_impl.cpp:1621`）。

### 2.2 StreamID 编码（`cpp/src/context/stream_impl.h:20-27`）

- `STREAM_TYPES = 4`，`STREAM_ID_MASK = 0b11`。
- bit0：`==0` 客户端发起，`==1` 服务端发起。
- bit1：`==0` 双向，`==2`(即 bit1=1) 单向。

| bit1 | bit0 | 发起方 | 方向 | 首个 ID（slot base） |
|---|---|---|---|---|
| 0 | 0 | 客户端 | 双向 | 0 |
| 0 | 1 | 服务端 | 双向 | 1 |
| 1 | 0 | 客户端 | 单向 | 2 |
| 1 | 1 | 服务端 | 单向 | 3 |

### 2.3 本地 StreamID 分配（`connection_impl.cpp:3565-3615`、`1500-1511`）

- `streamIdSlot()`：`roleBase = m_isClientInitiator ? 0 : 1`；单向 `roleBase+2`，双向 `roleBase`。
- 每 slot 维护游标 `m_streamId[slot]`，首用时初始化为 `roleBase + dirBit`，之后 **每分配一个 +STREAM_TYPES(=4)**，即同类型 ID 单调递增、连接内唯一、不复用。
- `streamOrdinal = streamId / STREAM_TYPES + 1`，与流数量上限比较（`connection_impl.cpp:1539`、`3581`）。
- 创建条件：状态须为 `kStateConnected`，或（`kStateInitialSent` 且启用 0-RTT）（`connection_impl.cpp:3573-3577`）。

### 2.4 对端 StreamID 校验（`connection_impl.cpp:1530-1545`）

- 必须是对端发起方向（bit0 与本端角色相反），否则 `UTP_ERR_STREAM_STATE_ERROR`。
- `streamOrdinal > streamLimit(type,false)` → `UTP_ERR_STREAM_LIMIT_ERROR`。
- 未知 StreamID 首帧到达时惰性创建 `StreamImpl` 并触发 `m_onIncomingStream`（`connection_impl.cpp:1579-1590`）。

### 2.5 RecvFragment（`stream_impl.h:35-47`）

红黑树节点：`treeNode`、`packet(PacketIn*)`、`data`、`len`、`consumed`、`offset`、`fin`、`accounted`、`memoryCost`。`remaining() = len - consumed`。

---

## 3. 状态机 / 流程

### 3.1 状态（`stream.h:59-64`，`stream_impl.cpp:340-352`）

| State | 条件 |
|---|---|
| `kStateOpen` | 默认 |
| `kStateHalfClosedLocal` | `m_localFinQueued` 为真（未同时满足 Closed 条件） |
| `kStateHalfClosedRemote` | `m_peerFin` 为真（未同时满足 Closed 条件） |
| `kStateClosed` | `m_localFinQueued && m_localFinSent && m_peerFin` |

### 3.2 发送路径

1. 应用 `write()`/`commitWrite()`：数据写入 `m_sendBuffer.produce()`，累加 `m_sendQueuedBytes`，记录入队时间戳 `m_lastSendQueuedAtUs`（`stream_impl.cpp:228-232`）。**仅入队，不直接发送**。
2. 决定立即发送或聚合延迟：`shouldDeferSend()` 为真则 `nextScheduleTime(延迟ms)`；否则 `scheduleWrite()` + `nextScheduleTime(1)`（`stream_impl.cpp:238-251`）。
3. 连接调度器回调 `onConnectionWritable(nowUs)`（`stream_impl.cpp:773-791`）→ `flushPendingSends(budget)`。
4. `flushPendingSends`（`stream_impl.cpp:656-771`）：按 `m_nextSendOffset` 从环形缓冲取可读视图，单帧上限 `min(view.len, UINT16_MAX, streamPayloadBudgetHint())`；遇 `UTP_ERR_WOULD_BLOCK` 折半重试（`tryLen/=2`）保证前进；成功则推进 `m_nextSendOffset`、`m_sendInFlightBytes`，减少 `m_sendQueuedBytes`。
5. 纯 FIN（无数据）在队列排空且 `m_localFinQueued && !m_localFinSent` 时以空 payload+fin 发出（`stream_impl.cpp:681-703`）。
6. ACK 到达 `onPacketAcked(offset,len)`（`stream_impl.cpp:793-853`）：维护 `m_sendAckedRanges`（`std::map<u64,u64>`）合并区间，连续段推进 `m_sendAckedOffset` 并 `m_sendBuffer.consume()` 释放空间，触发 `maybeNotifyWritable`。

### 3.3 接收路径 / 重组（`onFrame`，`stream_impl.cpp:458-632`）

1. 前置校验：被对端 RESET → `UTP_ERR_STREAM_CLOSED`；StreamID 不匹配/空数据 → `UTP_ERR_INVALID_PARAM`；offset 溢出 → `UTP_ERR_STREAM_FLOW_CONTROL`。
2. 前向间隙限制：`frameOffset - m_recvOffset > recv_stream_max_gap` → `UTP_ERR_WOULD_BLOCK`（`stream_impl.cpp:479-484`）。
3. 左侧裁剪：`frameOffset < m_recvOffset` 的已消费部分裁掉；若整帧已消费且带 FIN 且 `frameEnd==m_recvOffset` 置 `m_peerFin`。
4. 重叠去重：对已有片段做 prev/lowerBound 裁剪，仅把不重叠的空洞区间构造成新 `RecvFragment` 插入红黑树；插入时对底层 `PacketIn` `retainPacketIn()`（引用计数 +1）（`stream_impl.cpp:507-593`）。
5. FIN 归属：把 FIN 挂到覆盖 `originalEnd` 的尾片段；若无数据 FIN 且末端等于已读点则直接 `m_peerFin`，否则插入零长 FIN 哨兵片段（`stream_impl.cpp:595-626`）。
6. `maybeAdvancePeerFin` 清理已到达读点的空/已消费片段并传播 FIN（`stream_impl.cpp:1171-1186`）。

### 3.4 读取

- 拷贝式 `read()`（`stream_impl.cpp:126-180`）：仅拷贝从 `m_recvOffset` 起 **连续** 的字节，推进 `m_recvOffset`、递减 `m_recvBufferedBytes`，`eraseRecvFragment` 释放并 `releasePacketIn`；无数据且 `m_peerFin` 返回 0（EOF），否则 `WOULD_BLOCK`。
- 零拷贝 `acquireReadViews()`（const，`stream_impl.cpp:257-294`）返回最多 2 段连续视图；`commitReadViews(bytes)` 确认消费并释放片段（`stream_impl.cpp:296-338`）。
- 消费后回调 `m_conn->onStreamBytesConsumed(id, n)`（用于流量控制窗口更新）。

### 3.5 关闭 / 重置

C 版以 `utp_stream_shutdown(stream, how)` 统一半关闭接口：`WRITE` 在已排队数据之后发送 FIN；`READ` 立即丢弃本地接收缓存并可靠发送 `STOP_SENDING(stream_id, UTP_PROTOCOL_STOP_SENDING_CANCELLED)`；`BOTH` 组合两者。`UTP_PROTOCOL_STOP_SENDING_CANCELLED` 是内部帧协议错误码，不属于公开 API。读关闭后，已经接收以及后续在途 STREAM 字节仍须通过流级、连接级流控校验，并从连接接收窗口退休，允许其他流继续使用 `MAX_DATA`；这些数据不再缓存或交付应用，也不再为该流发送 `MAX_STREAM_DATA`。

`utp_stream_reset(error_code)` 只异常中止**本地写方向**：按该流已经成功写入 UDP 的最大偏移发送可靠 `RESET_STREAM`，尚在 scheduled 队列且未实际发送的 STREAM 字节必须取消并从连接发送流控账本回退；已发送但未确认或已判丢的数据不回退，只停止重传。后续写接口返回 `CANCELLED`，读方向保持可用。收到 `RESET_STREAM` 则只关闭本地读方向并释放接收重组缓存，后续读接口返回 `CANCELLED`；收到 `STOP_SENDING` 时必须停止本地写并回送 `RESET_STREAM`。迟到 ACK 按幂等方式回收。

FIN 与 RESET_STREAM 共用接收侧最终偏移。首次 FIN/RESET 保存 `peer_final_size`；后续 STREAM 不得超过该偏移，重复 FIN/RESET 声明的最终偏移必须一致。RESET_STREAM 的 `final_size` 不得小于该流已见最大偏移，也不得超过流级或连接级接收额度。

`on_closed` 仅在本地可读、可写方向均已结束且接收缓存排空时触发一次；不再提供单独的 reset 回调。

四种 Stream ID 分别以 4 为步长单调递增，Connection 生命周期内不得复用。UDP 允许乱序，不能仅凭 `stream_id < max_seen` 判定迟到帧；C 版为已回收流维护有界终态哈希表和 LRU，默认每连接 4096 条，可通过 `stream_terminal_capacity` 配置，容量满时淘汰最旧记录并原地复用槽位。终态记录用于幂等处理迟到的 STREAM、RESET_STREAM 和 STOP_SENDING，不能再次触发 `on_incoming_stream`。

首次收到会创建流的 STREAM、RESET_STREAM 或 STOP_SENDING 时，协议状态必须先提交，再调用 `on_incoming_stream`；用户可在该回调中立即注册状态回调或执行读写。因该首帧产生的 readable、writable、closed 通知在 `on_incoming_stream` 返回后触发，避免用户漏掉边沿。

---

## 4. 不变量与规则（MUST / MUST NOT）

- MUST：应用写入前若 `m_localFinQueued` 已置，`write/commitWrite` 返回 `UTP_ERR_STREAM_CLOSED`（`stream_impl.cpp:98-99,204`）。FIN 之后 MUST NOT 再写。
- MUST：`write` 的 `len > appWriteCredit()` 返回 `UTP_ERR_WOULD_BLOCK`（`stream_impl.cpp:102-103`）。
- MUST：`commitWrite` 中 `bytes > freeSize()` → `UTP_ERR_OVERFLOW`；`bytes > UINT32_MAX` → `UTP_ERR_OVERFLOW`；`queued+inFlight+bytes > kMaxSendQueueBytes(16MB)` → `UTP_ERR_STREAM_DATA_LIMITED`（`stream_impl.cpp:212-226`）。
- MUST：单帧 payload MUST NOT 超过 `UINT16_MAX`（u16 长度字段）。
- MUST：接收重组按流偏移有序，重叠数据 MUST 被裁剪去重（不产生重复/空洞拷贝）。
- MUST：`m_recvBufferedBytes + fragment.len > kMaxRecvFragmentBytes(2MB)` 时拒绝插入，返回 `UTP_ERR_WOULD_BLOCK`（`stream_impl.cpp:949-951`）。
- MUST：重组内存/片段数受双层限额约束（单流 + 单连接），超限 `accountRecvFragment` 返回 false → `UTP_ERR_WOULD_BLOCK`（`stream_impl.cpp:1088-1146`）。
- MUST：持有 `PacketIn` 的片段释放时必须 `releasePacketIn`（引用计数 -1），仅当 `packet!=nullptr && len>0` 才 retain/release（`stream_impl.cpp:532-533,1079-1081`）。
- MUST：同 offset 新片段若不比旧片段长则丢弃新片段；更长则原地替换 `rb_replace_node`（`stream_impl.cpp:963-978`）。
- MUST NOT：回调重入——`m_notifyingReadable/m_notifyingWritable` 守卫防止回调内递归通知（`stream_impl.cpp:1234-1258`）。
- MUST：`setPriority` 仅接受 [0,7]，否则 `UTP_ERR_INVALID_PARAM`（`stream_impl.cpp:405-411`）；优先级变更清零 `m_schedWaitRounds` 并触发 `scheduleWrite`。
- MUST：`reset` 在流已完全关闭（三态全真）时返回 `UTP_ERR_STREAM_CLOSED`（`stream_impl.cpp:378-380`）。

---

## 5. 参数与默认值（确切值 + 变量名）

### 5.1 StreamImpl 编译期常量（`stream_impl.h:54-56`，`stream_impl.cpp:38-40`）

| 变量 | 值 | 用途 |
|---|---|---|
| `kDefaultBufferCapacity` | `64*1024` (64KB) | 发送环形缓冲缺省容量（config 缺失/为0时用） |
| `kMaxSendQueueBytes` | `16*1024*1024` (16MB) | 单流 queued+inFlight 硬上限 |
| `kMaxRecvFragmentBytes` | `2*1024*1024` (2MB) | 单流未消费接收字节上限 |
| `kMinSendBudgetBytesPerWritable` | `4*1024` (4KB) | 单次可写轮次发送预算下限 |
| `kMaxSendBudgetBytesPerWritable` | `64*1024` (64KB) | 单次可写轮次发送预算上限 |
| `kDefaultMtuPacketsPerWritable` | `4` | 预算按 MTU payload 的量子倍数 |

发送预算：`StreamWritableSendBudget = clamp(streamPayloadBudgetHint()*4, 4KB, 64KB)`（`stream_impl.cpp:50-58`）。

### 5.2 Config 流相关默认值（`cpp/include/utp/config.h:127-144`）

| 字段 | 默认值 | 说明 |
|---|---|---|
| `stream_default_priority` | `4` | 默认优先级（0 最高，7 最低） |
| `stream_scheduler_mode` | `kStreamSchedulerStrict(1)` | 调度模式 |
| `stream_aging_threshold` | `8` | STRICT 老化阈值（等待轮次） |
| `stream_aging_step` | `1` | 每达阈值提升的优先级级数 |
| `stream_drr_quantum` | `1200` | DRR 基准量子 |
| `stream_drr_deficit_cap` | `128*1024` | DRR 赤字上限 |
| `stream_send_buffer_limit` | `256*1024` (256KB) | 单流写缓冲上限（RingBuffer 初始容量与写信用） |
| `stream_enable_coalescing` | `true` | 是否启用小包聚合 |
| `stream_min_payload_before_immediate_send` | `1200` | 达此排队字节则不再延迟、立即发 |
| `stream_coalesce_delay_us` | `1000` (1ms) | 聚合等待时延 |
| `recv_reassembly_memory_limit` | `16*1024*1024` (16MB) | 单连接重组内存上限 |
| `recv_reassembly_fragment_limit` | `4096` | 单连接重组片段数上限 |
| `recv_stream_reassembly_memory_limit` | `4*1024*1024` (4MB) | 单流重组内存上限 |
| `recv_stream_reassembly_fragment_limit` | `1024` | 单流重组片段数上限 |
| `recv_stream_max_gap` | `2*1024*1024` (2MB) | 单帧相对读点最大前向间隙 |

> 注意存在多重上限并存：写侧受 `stream_send_buffer_limit(256KB)`、`kMaxSendQueueBytes(16MB)`、RingBuffer `freeSize` 共同约束；`appWriteCredit = min(cap-used, sendBuffer.freeSize())`（`stream_impl.cpp:855-861`）。读侧未消费字节受 `kMaxRecvFragmentBytes(2MB)` 约束，而 pinned 内存受 4MB/流、16MB/连接约束——两套口径不同（见 §7 风险）。

### 5.3 连接层调度常量（`connection_impl.cpp:88-89`）

| 变量 | 值 |
|---|---|
| `kMaxStreamPriorityLevels` | `8` |
| `kMaxStreamSendBurstsPerFlush` | `64`（单次 flush 最多选择 64 个突发） |

`streamPayloadBudgetHint()`（`connection_impl.cpp:1699-1718`）= `currentMaxPacketSize - UTP_HEADER_SIZE - [GCM_TAG_SIZE 若加密] - FRAME_STREAM_HDR_SIZE`（下限 1）。

### 5.4 TransportParams 流相关（`transport_param.h:82-87`）

| 字段 | 默认值 |
|---|---|
| `init_max_streams_bidi` | `64` |
| `init_max_streams_uni` | `32` |
| `initial_max_data` | `64MB` |
| `initial_max_stream_data_bidi_local` | `16MB` |
| `initial_max_stream_data_bidi_remote` | `16MB` |

`streamLimit()` 取协商值且下限 1（`connection_impl.cpp:1489-1498`）。默认接收窗口回退值 `kDefaultInitialMaxStreamData = kMaxRecvFragmentBytes = 2MB`（`connection_impl.cpp:93`）。

---

## 6. 对外接口 / 回调（`cpp/include/utp/stream.h`）

接口（纯虚，`StreamImpl` 实现）：
- `id()`、`write(data,len,fin)`、`read(buffer,cap)`。
- 零拷贝写：`acquireWriteBuffer(views[2], maxBytes)` → `commitWrite(bytes, fin)`。
- 零拷贝读：`acquireReadViews(views[2], maxBytes) const` → `commitReadViews(bytes)`。
- `state()`、`readable()`、`writable()`、`close()`、`reset(errorCode)`、`resetReceived()`。
- `setPriority(uint8)` / `priority()`（范围 0–7，常量 `kPriorityHighest=0`、`kPriorityLowest=7`、`kPriorityDefault=4`）。

回调：
- `OnReadable void()` — 有连续可读数据或已收 FIN（`maybeNotifyReadable`，`stream_impl.cpp:1234`）。
- `OnWritable void()` — 可写信用恢复（`maybeNotifyWritable`，仅在 `force` 且 `writable()` 时）。
- `OnClosed void()` — 双向完全关闭且接收缓冲排空，仅回调一次。
- `OnReset void(uint16_t errorCode)` — 收到/发生 RESET，仅回调一次。

> C 版实现状态：已提供 `utp_stream_set_on_readable()`、`utp_stream_set_on_writable()`、
> `utp_stream_set_on_closed()`。可读、可写回调具有重入保护；注册可读/可写回调时若状态已满足，
> 会同步通知一次。仅关闭状态变化触发 `on_closed`，且最多一次；流清理期间会先解除回调，避免析构路径回调应用。

内部（`friend ConnectionImpl`）：`onFrame`、`onReset`、`onConnectionWritable`、`onPacketAcked`、`hasPendingSendWork`、`shouldDeferSend`、`coalesceDelayRemainingUs`（`stream_impl.h:82-112`）。

---

## 7. 当前实现边界 / 风险

1. **StreamID 分配无回绕/枯竭保护**：`nextStreamId += STREAM_TYPES` 直接自增 u32（`connection_impl.cpp:3603`），仅靠 `m_streams.find` 命中判 `UTP_ERR_STREAM_ID_EXHAUSTED`；接近 u32 上限时 `+4` 可能溢出，未见显式回绕检查。待确认是否可达。
2. **接收上限口径不一致**：未消费字节上限 `kMaxRecvFragmentBytes=2MB`（`stream_impl.cpp:949`）与 pinned 内存上限 `recv_stream_reassembly_memory_limit=4MB`、连接级 16MB 并存，语义不同（一个是逻辑数据量、一个是持有 PacketIn 物理内存），配置调参时需注意二者关系。
3. **接收窗口回退默认 2MB**：当协商窗口为 0 时回退 `kDefaultInitialMaxStreamData=2MB`（`connection_impl.cpp:93`），恰等于 `kMaxRecvFragmentBytes`，与 config 中 `initial_max_stream_data_*=16MB` 不同源，存在两处默认值。
4. **发送预算多阈值叠加**：`appWriteCredit` 同时受 256KB(config)、16MB(kMaxSendQueueBytes)、RingBuffer freeSize 约束，实际有效写信用取最小；RingBuffer 通过 `ensureFree` 可倍增扩容，但初始容量即 `stream_send_buffer_limit`。
5. **聚合延迟的多前置豁免**：`shouldDeferSend` 在路径校验中、HandshakeDone 未完成、`m_bytesIn<2048`、含 FIN、或排队量≥`stream_min_payload_before_immediate_send` 等情形一律不延迟（`stream_impl.cpp:873-918`），即聚合只在“稳态且小包”下生效。
6. **零长 FIN 哨兵片段**：无数据 FIN 在有空洞时会插入 `len=0` 的哨兵节点占一个片段名额（`stream_impl.cpp:608-623`），受片段数上限计。
7. **`receive_history.{h,cpp}` 与流数据重组无直接关系**：它记录已收包号区间用于 ACK/去重（`m_maxRanges` 默认 256，`dropOldestRange` + `stopWait(cutoff)` 控内存），属 ACK 模块，本流模块不使用其做数据重组。

---

## 8. 与 doc/ 差异

参考 `doc/协议设计文档.md` §7、§8 与 `doc/零拷贝与红黑树接收模型.md`：

1. **StreamID 位宽**：doc §7.2 帧结构写 `uint64_t stream_id`（`协议设计文档.md:305` 附近字段列表隐含），但代码帧与接口均为 **u32**（`proto/frame/stream.h:30`、`stream.h:78`）。以代码为准：32 位。
2. **StreamID 低位表**：doc §7.3 表与代码宏一致（bit0 角色、bit1 方向），无冲突。
3. **调度模式**：doc §7.5 列出 DISABLED/STRICT/DRR，与代码 `StreamSchedulerMode`（`config.h:34-37`）一致；默认 STRICT，DRR 为“赤字轮询”（WDRR 的按优先级加权变体，权重 = `kPriorityLowest - priority + 1`，`connection_impl.cpp:3772-3774`）。doc 未给出量子/赤字上限具体值，代码为 `stream_drr_quantum=1200`、`stream_drr_deficit_cap=128KB`。
4. **零拷贝接收模型**：`doc/零拷贝与红黑树接收模型.md` 描述红黑树 + PacketIn 引用计数，与代码 `m_recvFragmentsTree` + `retainPacketIn/releasePacketIn` 完全吻合。文档称“接收不再用环形队列”，代码接收确实用树；**但发送侧仍用 `RingBuffer`**（文档未覆盖发送侧，非矛盾）。
5. **流量控制默认值**：doc §8 只给出参数名（`initial_max_data` 等）未给数值；代码默认 `initial_max_data=64MB`、`initial_max_stream_data_*=16MB`（`transport_param.h:85-87`）。
6. **FIN 语义**：doc §7.4 概念性描述（FIN=发送完毕，RESET=中止）；代码额外实现了半关闭状态机四态与 `maybeNotifyClosed` 的“双向关闭且缓冲排空才回调”细节，doc 未细化。

---

## 9. 依赖

- `ConnectionImpl`（`cpp/src/context/connection_impl.{h,cpp}`）：StreamID 分配/校验、多路复用调度、帧发送、流量控制窗口、`onStreamBytesConsumed`/`onStreamDataSent`、`streamPayloadBudgetHint`、`m_recvReassembly*` 全局计账。
- `MemoryManager`（`cpp/src/util/mm.{h,cpp}`）：`getRecvFragment/putRecvFragment`、`retainPacketIn/releasePacketIn`。
- `PacketIn`（`cpp/src/proto/packet_in.h`）：零拷贝底层内存 + 引用计数，`alloc_size` 用于内存计账。
- `RingBuffer`（`cpp/src/util/ring_buffer.{h,cpp}`）：发送侧双视图环形缓冲（`writableViews/readableViewsFrom/produce/consume/ensureFree`）。
- `FrameStream`（`cpp/src/proto/frame/stream.h`）：STREAM 帧结构。
- `rbtree.h`：内核风格红黑树（`rb_insert_color`/`rb_erase`/`rb_replace_node`/`rb_first`/`rb_next`）。
- `TransportParams` / `Config`（`transport_param.h` / `config.h`）：流数量、窗口、调度、聚合、重组限额参数。
- `time::MonotonicUs()`（`cpp/src/util/time.h`）：聚合窗口时间戳。
