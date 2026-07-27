# uTP 需求文档 05：流量控制（连接级 + 流级）

> 本文档由现有 C++ 实现（`cpp/`）反推。**代码是唯一 ground truth**，`doc/` 仅交叉参考；不一致处以代码为准并在 §8 标注差异。
> 引用格式：`文件:行号`。数值均来自代码，未能确证处标「待确认」。

---

## 1. 职责与边界

流量控制模块负责限制**在途/已发送数据量**与**已接收数据量**，防止发送端压垮接收端缓冲区，提供背压。分两个层级：

- **连接级（connection-level）**：对一条连接上所有流的累计字节数设窗口。
- **流级（stream-level）**：对单条 stream 的字节偏移设窗口。

职责细分：

- **发送方向（本端作为 sender）**：发送前校验累计发送量是否超过对端授予的窗口（`m_peerMaxData` / `m_peerMaxStreamData`）；超限时发 `DataBlocked` / `StreamDataBlocked` 并返回 `UTP_ERR_WOULD_BLOCK`。见 `cpp/src/context/connection_impl.cpp:1788-1821`。
- **接收方向（本端作为 receiver）**：校验对端发送偏移是否超过本端已通告窗口（`m_localMaxDataAdvertised` / `m_localMaxStreamDataAdvertised`）；随应用消费数据推进窗口并发 `MaxData` / `MaxStreamData`。见 `cpp/src/context/connection_impl.cpp:1561-1603`、`2254-2318`。

**边界（本模块不负责）**：

- 拥塞控制（cwnd / BBR / CUBIC）由 `send_ctl.cpp` 负责，与流量控制窗口是**两套独立限制**，本模块不涉及。
- 流 ID 数量限制（`MaxStreams` / `StreamsBlocked`，帧类型 `kFrameMaxStreams` / `kFrameStreamsBlocked`）在本实现中**存在帧枚举但不属于本模块的 initial_max_streams 字节流控**，本文档只覆盖字节级流控。
- 传输参数的握手协商流程（何时编码进 Initial/Handshake 包）由握手模块驱动，本模块只消费其中流控字段。

---

## 2. 数据结构 / 帧

### 2.1 帧类型枚举（`cpp/src/proto/frame.h:47-50`）

`FrameType` 为顺序枚举（`kFrameInvalid=0` 起），序列化为 1 字节。流控相关四帧：

| 枚举 | 序号(=opcode) | 用途 |
|---|---|---|
| `kFrameMaxData` | 18 (0x12) | 连接级窗口更新 |
| `kFrameMaxStreamData` | 19 (0x13) | 流级窗口更新 |
| `kFrameDataBlocked` | 20 (0x14) | 连接级受限通知 |
| `kFrameStreamDataBlocked` | 21 (0x15) | 流级受限通知 |

（opcode 由枚举位置推算：`kFrameInvalid`=0 … `kFrameMaxData`=18；与 `doc/协议设计文档.md:184-187` 声明的 0x12–0x15 一致。）

### 2.2 帧结构与线格式

- **`FrameMaxData`**（`cpp/src/proto/frame/max_data.h:18-29`）：`uint64_t maximum_data`。线长 `FRAME_MAX_DATA_SIZE = 1 + 8 = 9` 字节（`max_data.h:13`）。编解码见 `max_data.cpp:18-66`。
- **`FrameMaxStreamData`**（`cpp/src/proto/frame/max_stream_data.h:18-30`）：`uint32_t stream_id`、`uint64_t maximum_stream_data`。线长 `FRAME_MAX_STREAM_DATA_SIZE = 1 + 4 + 8 = 13` 字节（`max_stream_data.h:13`）。见 `max_stream_data.cpp:18-73`。
- **`FrameDataBlocked`**（`cpp/src/proto/frame/data_blocked.h:18-29`）：`uint64_t data_limit`。线长 `FRAME_DATA_BLOCKED_SIZE = 1 + 8 = 9` 字节（`data_blocked.h:13`）。见 `data_blocked.cpp:18-71`。
- **`FrameStreamDataBlocked`**（`cpp/src/proto/frame/stream_data_blocked.h:18-30`）：`uint32_t stream_id`、`uint64_t stream_data_limit`。线长 `FRAME_STREAM_DATA_BLOCKED_SIZE = 1 + 4 + 8 = 13` 字节（`stream_data_blocked.h:13`）。见 `stream_data_blocked.cpp:18-73`。

所有字段为定长序列化（非 varint），大小端由 `Serialize` 决定（待确认具体字节序，本模块不涉及）。

### 2.3 传输参数流控字段（`cpp/src/util/transport_param.h`）

`TransportParams` 结构含三个流控字段（`transport_param.h:85-87`）：

| 字段 | 类型 | 结构体默认值 | 含义 |
|---|---|---|---|
| `initial_max_data` | `uint64_t` | `64*1024*1024`（64 MiB） | 协商给对端的连接级初始接收窗口 |
| `initial_max_stream_data_bidi_local` | `uint64_t` | `16*1024*1024`（16 MiB） | 双向流「本地发起」初始接收窗口 |
| `initial_max_stream_data_bidi_remote` | `uint64_t` | `16*1024*1024`（16 MiB） | 双向流「远端发起」初始接收窗口 |

启用标志位（`transport_param.h:20-39`）：`kInitialMaxData=1<<5`、`kInitialMaxStreamDataBidiLocal=1<<6`、`kInitialMaxStreamDataBidiRemote=1<<7`，均属 `kDefaultFlags`。

传输参数帧 `FrameTransportParams` 定长 `FRAME_TRANSPORT_PARAMS_SIZE = 1+2+4+2+2+2+1+8+8+8 = 30` 字节（`transport_params.h:14`），编解码顺序见 `transport_params.cpp:38-91`（末三个 `uint64_t` 即三个流控字段）。

**注意**：运行时的实际默认值来自 `Config`（`cpp/include/utp/config.h:122-124`），与 `TransportParams` 结构体内联默认值**不同**（见 §5、§8）：

| Config 字段 | 默认值 |
|---|---|
| `initial_max_data` | `8*1024*1024`（8 MiB） |
| `initial_max_stream_data_bidi_local` | `256*1024`（256 KiB） |
| `initial_max_stream_data_bidi_remote` | `256*1024`（256 KiB） |

初始化时 `m_loaclTP` 三字段由 `cfg` 覆盖（`connection_impl.cpp:454-456`），故实际生效的是 Config 值。

### 2.4 连接内状态成员（`cpp/src/context/connection_impl.h:296-308, 382-385`）

**发送侧（对端授予的窗口）：**
- `uint64_t m_peerMaxData{0}`：对端连接级窗口（0=未知）。
- `std::unordered_map<uint32_t,uint64_t> m_peerMaxStreamData`：对端各流窗口。
- `uint64_t m_streamDataSentTotal{0}`：本端已发送数据累计（连接级）。
- `std::unordered_map<uint32_t,uint64_t> m_streamMaxSentOffset`：各流已发送最大偏移。

**接收侧（本端通告的窗口）：**
- `uint64_t m_localMaxDataAdvertised{0}`：本端已通告的连接级窗口。
- `std::unordered_map<uint32_t,uint64_t> m_localMaxStreamDataAdvertised`：本端各流已通告窗口。
- `uint64_t m_localBytesConsumedTotal{0}` + `m_localStreamBytesConsumed`：应用已消费字节（连接级/流级），驱动窗口推进。
- `uint64_t m_localBytesReceivedTotal{0}` + `m_localStreamMaxReceivedOffset`：已接收字节/最大偏移，用于校验对端是否越界。
- `bool m_initialFlowControlAdvertised{false}`：是否已发出首个连接级窗口更新。

**限速时间戳（防泛滥）：**
- `m_lastMaxDataSentUs` / `m_lastMaxStreamDataSentUs`（map）：上次发 MaxData(/StreamData) 时刻。
- `m_lastDataBlockedSentUs` / `m_lastStreamDataBlockedSentUs`（map）：上次发 Blocked 时刻。

---

## 3. 更新时机 / 流程

### 3.1 初始化（`connection_impl.cpp:447-473`）

握手/初始化时用 `Config` 填充 `m_loaclTP`，并置 `m_localMaxDataAdvertised = m_loaclTP.initial_max_data`；`m_peerMaxData=0`（未知）；各 map 清空。

### 3.2 接收对端传输参数（`connection_impl.cpp:812-833`）

仅在 `UTP_TYPE_INITIAL` / `UTP_TYPE_HANDSHAKE` 包内合法，否则报 `UTP_ERR_FRAME_UNEXPECTED` 并 abort。解码成功后保存 `m_peerTP`，取 `peerInitialMaxData = peerTp.initial_max_data>0 ? … : kDefaultInitialMaxData`，调用 `handleMaxDataFrame()` 初始化对端连接级窗口。
（对端的 `initial_max_stream_data_*` 不在此处主动灌入 `m_peerMaxStreamData`，而是在 `peerStreamDataLimit()` 惰性回退，见 §3.5。）

### 3.3 收到 MaxData / MaxStreamData（`connection_impl.cpp:834-849, 2110-2140`）

- `handleMaxDataFrame(v)`：仅当 `v > m_peerMaxData` 时更新，并 `scheduleWrite()` 唤醒被阻塞的发送。窗口**单调不减**。
- `handleMaxStreamDataFrame(sid,v)`：流首次出现则 emplace；已存在则仅当 `v > 旧值` 时更新。均触发 `scheduleWrite()`。

### 3.4 收到 DataBlocked / StreamDataBlocked（`connection_impl.cpp:850-869`）

作为接收端的响应：
- 收 `DataBlocked` → 立即回发 `sendMaxDataFrame(m_localMaxDataAdvertised)`（重发当前连接级窗口）。
- 收 `StreamDataBlocked` → `ensureFlowControlAdvertised(sid)` 后回发该流当前已通告窗口（缺省回退到 `initial_max_stream_data_bidi_remote` 或 `kDefaultInitialMaxStreamData`）。

（注意：这里对 blocked 帧的响应**未做限速**，每收一个 blocked 就回一个 max 帧。）

### 3.5 发送数据前的流控校验（`connection_impl.cpp:1788-1821`）

`sendStreamFrame()` 当 `len>0`：
1. `ensureFlowControlAdvertised(streamId)`。
2. 溢出检查：`m_streamDataSentTotal + len` 不得溢出 `uint64_t`。
3. **连接级**：若 `m_peerMaxData>0 && m_streamDataSentTotal+len > m_peerMaxData` → 限速地发 `DataBlocked(m_peerMaxData)`，返回 `UTP_ERR_WOULD_BLOCK`。
4. **流级**：`streamDataLimit = peerStreamDataLimit(streamId)`（`connection_impl.cpp:2142-2151`：map 命中取之，否则回退 `m_peerTP.initial_max_stream_data_bidi_local` 或 `kDefaultInitialMaxStreamData`）。若 `streamOffset > limit` 或 `len > limit-streamOffset` → 限速地发 `StreamDataBlocked`，返回 `UTP_ERR_WOULD_BLOCK`。

发送成功后 `onStreamDataSent()`（`connection_impl.cpp:2036-2049`）按流最大偏移增量累加 `m_streamDataSentTotal`（保证同一区间重传不重复计数）。

### 3.6 接收数据的流控校验（`connection_impl.cpp:1557-1603`）

`ingestStreamFrame()`：
1. `stream_offset + stream_data_length` 溢出检查 → `UTP_ERR_STREAM_FLOW_CONTROL`。
2. `ensureFlowControlAdvertised(sid)`；`frameEnd = offset+len`。
3. **流级**：`streamLimit`（已通告窗口或 `initialStreamReceiveWindow`），若 `frameEnd > streamLimit` → `UTP_ERR_STREAM_FLOW_CONTROL`（"peer exceeded stream receive window"）。
4. **连接级**：`streamDelta = max(0, frameEnd - 旧的该流最大接收偏移)`；若 `m_localBytesReceivedTotal + streamDelta > m_localMaxDataAdvertised`（或溢出）→ `UTP_ERR_STREAM_FLOW_CONTROL`（"peer exceeded connection receive window"）。
5. 交付 `stream->onFrame()` 成功且 `streamDelta>0` 后，推进 `m_localStreamMaxReceivedOffset[sid]=frameEnd`、`m_localBytesReceivedTotal += streamDelta`。

### 3.7 应用消费数据后推进窗口（`connection_impl.cpp:2254-2318`）

`onStreamBytesConsumed(sid,bytes)`（由 `stream_impl.cpp:176,334` 在应用读走数据时调用）：
1. 累加 `m_localBytesConsumedTotal`、`m_localStreamBytesConsumed[sid]`。
2. 若非 `kStateConnected` 或 `m_peerConnectionID==0` 直接返回（仅记账，不发帧）。
3. **连接级**：`targetMaxData = initialDataWindow + m_localBytesConsumedTotal`（滑动窗口，窗口大小恒等于初值）。`delta = target - m_localMaxDataAdvertised`，`threshold = initialDataWindow/10`。满足 `delta>=threshold`（按量）**或** 距上次发送 `>= kFlowControlUpdateMinIntervalUs`（按时）则发 `MaxData(target)` 并更新 `m_localMaxDataAdvertised`、`m_initialFlowControlAdvertised=true`。
4. **流级**：`targetMaxStreamData = initialStreamReceiveWindow(sid) + consumedByStream`，同样按 `baseMaxStreamData/10` 阈值 或 时间间隔触发 `MaxStreamData`。

---

## 4. 不变量与规则（MUST / MUST NOT）

- **[MUST]** 对端窗口单调不减：`handleMaxDataFrame` / `handleMaxStreamDataFrame` 仅接受严格大于当前值的更新（`connection_impl.cpp:2112,2134`）。
- **[MUST NOT]** 发送端在连接级发送总量 MUST NOT 超过 `m_peerMaxData`（当其 >0），流级 MUST NOT 超过 `peerStreamDataLimit(sid)`（`connection_impl.cpp:1795,1808`）。
- **[MUST]** 接收端收到超出本端已通告连接/流窗口的数据 → 以 `UTP_ERR_STREAM_FLOW_CONTROL` 拒绝（`connection_impl.cpp:1567,1575`）。
- **[MUST]** 传输参数解码时流控字段 MUST NOT 超过 `kMaxFlowControlValue = (1<<60)-1`，否则 `UTP_ERR_INVALID_PARAM`（`transport_params.cpp:105-110`；`transport_param.h:18`）。
- **[MUST]** 传输参数帧仅在 Initial/Handshake 包合法，其他包内出现 MUST abort（`connection_impl.cpp:817-819`）。
- **[MUST]** 计数防溢出：发送总量、接收 delta 累加前均做 `uint64_t` 溢出检查（`connection_impl.cpp:1791,1557,1574`）。
- **[MUST]** `m_streamDataSentTotal` 按「流最大偏移增量」累加，重传同一区间 MUST NOT 重复计数（`connection_impl.cpp:2042-2048`）。
- **[MUST]** MaxData/MaxStreamData/Blocked 帧的自发送 MUST 受最小间隔限速（见 §5），blocked 触发同理；例外：§3.4 对收到 blocked 的**回应**不限速。
- **[MUST NOT]** `ensureFlowControlAdvertised` 在非 `kStateConnected`、`m_peerConnectionID==0` 或 `streamId==UINT32_MAX` 时 MUST NOT 建立通告条目（`connection_impl.cpp:2231-2237`）。

---

## 5. 参数与默认值

| 名称（变量/宏） | 值 | 出处 | 说明 |
|---|---|---|---|
| `kDefaultInitialMaxStreamData` | `= StreamImpl::kMaxRecvFragmentBytes = 2*1024*1024`（2 MiB） | `connection_impl.cpp:93`；`stream_impl.h:56` | 流级窗口兜底默认 |
| `kDefaultInitialMaxData` | `kDefaultInitialMaxStreamData*4 = 8 MiB` | `connection_impl.cpp:94` | 连接级窗口兜底默认 |
| `kMaxDataUpdateStep` | `256*1024`（256 KiB） | `connection_impl.cpp:95` | **已定义但当前未被引用**（待确认是否死代码） |
| `kMaxStreamDataUpdateStep` | `128*1024`（128 KiB） | `connection_impl.cpp:96` | **已定义但当前未被引用** |
| `kFlowControlUpdateMinIntervalUs` | `20000`（20 ms） | `connection_impl.cpp:97` | MaxData/MaxStreamData 最小发送间隔 |
| `kFlowControlBlockedMinIntervalUs` | `50000`（50 ms） | `connection_impl.cpp:98` | DataBlocked/StreamDataBlocked 最小间隔 |
| 连接级更新阈值 | `initialDataWindow / 10` | `connection_impl.cpp:2277` | 按量触发的 delta 门限 |
| 流级更新阈值 | `baseMaxStreamData / 10` | `connection_impl.cpp:2304` | 同上（流级） |
| `Config::initial_max_data` | `8 MiB` | `config.h:122` | **运行时实际**连接级初值 |
| `Config::initial_max_stream_data_bidi_local` | `256 KiB` | `config.h:123` | 运行时流级（本地发起）初值 |
| `Config::initial_max_stream_data_bidi_remote` | `256 KiB` | `config.h:124` | 运行时流级（远端发起）初值 |
| `TransportParams::initial_max_data`（结构体内联） | `64 MiB` | `transport_param.h:85` | 未被 Config 覆盖时的默认（实际被覆盖） |
| `TransportParams::initial_max_stream_data_bidi_local`（内联） | `16 MiB` | `transport_param.h:86` | 同上 |
| `TransportParams::initial_max_stream_data_bidi_remote`（内联） | `16 MiB` | `transport_param.h:87` | 同上 |
| `kMaxFlowControlValue` | `(1<<60)-1` | `transport_param.h:18` | 传输参数流控字段上限 |
| 帧线长 `FRAME_MAX_DATA_SIZE` | `9` | `max_data.h:13` | |
| 帧线长 `FRAME_MAX_STREAM_DATA_SIZE` | `13` | `max_stream_data.h:13` | |
| 帧线长 `FRAME_DATA_BLOCKED_SIZE` | `9` | `data_blocked.h:13` | |
| 帧线长 `FRAME_STREAM_DATA_BLOCKED_SIZE` | `13` | `stream_data_blocked.h:13` | |
| 帧线长 `FRAME_TRANSPORT_PARAMS_SIZE` | `30` | `transport_params.h:14` | |

---

## 6. 接口

声明于 `cpp/src/context/connection_impl.h:184-192`：

- `void handleMaxDataFrame(uint64_t maximumData)` — 处理收到的连接级窗口更新。
- `void handleMaxStreamDataFrame(uint32_t streamId, uint64_t maximumStreamData)` — 流级窗口更新。
- `Status sendMaxDataFrame(uint64_t maximumData)` — 发送连接级窗口（`connection_impl.cpp:2153-2171`，`UTP_TYPE_CTRL`）。
- `Status sendMaxStreamDataFrame(uint32_t streamId, uint64_t maximumStreamData)` — 发送流级窗口（`:2173-2194`）。
- `Status sendDataBlockedFrame(uint64_t dataLimit)` — 连接级受限（`:2196-2210`）。
- `Status sendStreamDataBlockedFrame(uint32_t streamId, uint64_t streamDataLimit)` — 流级受限（`:2212-2227`）。
- `void ensureFlowControlAdvertised(uint32_t streamId)` — 惰性建立流通告条目（`:2229-2244`）。

辅助（内部）：`uint64_t peerStreamDataLimit(uint32_t) const`（`:2142-2151`）、`uint64_t initialStreamReceiveWindow(uint32_t) const`（`:2246-2252`）、`void onStreamDataSent(...)`（`:2036-2049`）、`void onStreamBytesConsumed(uint32_t,size_t)`（`:2254-2318`，供 `StreamImpl` 回调）。

帧编解码接口：各 `Frame*::encode/decode/frameSize(void*,size_t,Status&)`（各帧 `.h`/`.cpp`）。

---

## 7. 当前实现边界（已知局限 / 观察）

- **`kMaxDataUpdateStep` / `kMaxStreamDataUpdateStep` 未使用**（`connection_impl.cpp:95-96`）。实际窗口推进阈值改用 `initialWindow/10`（`:2277,2304`），两个 step 常量疑为遗留/死代码，需确认。
- **窗口大小恒定**：更新后窗口大小始终等于初始窗口（`target = initialWindow + consumed`），实现为**固定大小滑动窗口**，不做自适应放大（不像 QUIC auto-tuning）。
- **对端流级初值未预填**：收到对端 TP 时只灌 `m_peerMaxData`，流级依赖 `peerStreamDataLimit()` 惰性回退到 `m_peerTP.initial_max_stream_data_bidi_local`（`:2146`）。此处**统一用 bidi_local**，未按流是本端发起/远端发起区分 local/remote（可能与语义预期不符，待确认）。
- **对 blocked 帧的响应不限速**（§3.4），存在被对端 blocked 泛洪放大回应的理论风险。
- 流控字段线格式为**定长 8/4 字节**（非 varint），与典型 QUIC varint 编码不同。
- `initialStreamReceiveWindow` 依据 `m_isClientInitiator` + `STREAM_ID_IS_CLIENT/SERVER` 选择 local/remote 窗口（`:2246-2252`）。
- 传输参数帧仅携带三个流控字段（无独立的 uni 流窗口 / 无 `initial_max_stream_data_uni`）。

---

## 8. 与 doc/ 差异

参考 `doc/协议设计文档.md` §8「流量控制设计」（360-395 行）与帧表（184-187 行）。

1. **初值数值 doc 未给具体默认**：设计文档只说「由 TransportParams 中 `initial_max_data` 等定义」，未列默认值。代码存在**两套不一致的默认**：`TransportParams` 内联（64/16/16 MiB，`transport_param.h:85-87`）vs `Config`（8 MiB / 256 KiB / 256 KiB，`config.h:122-124`）。**运行时以 Config 为准**（`connection_impl.cpp:454-456`）。这是实现内部的潜在混淆点。
2. **「未确认数据量不得超过窗口」表述与实现口径不同**：`doc/协议设计文档.md:371,382` 称「未确认（unacked）数据量不得超过窗口」；代码实际用的是**已发送总量**（`m_streamDataSentTotal`，`connection_impl.cpp:1795`）而非 unacked，即窗口消费不随 ACK 回收（发送量单调递增对齐接收偏移）。属于 QUIC 式（发送偏移语义）而非「在途量」语义，与文档字面描述不一致。
3. **更新时机**：doc（384-395 行）只定性描述「应用消费数据后 / 想调整策略」；代码给出了确切的双触发（量阈 `initialWindow/10` 或时间 20 ms）与限速机制，文档未量化。
4. **opcode**：doc 帧表标 0x12–0x15，与枚举顺序推算一致（§2.1），无冲突。
5. doc §8 未提及 `kMaxFlowControlValue` 上限校验、blocked 回应逻辑、以及 uni 流窗口的缺失——这些均为实现细节/局限，文档层面留白。

---

## 9. 依赖

- **握手 / 传输参数模块**：提供 `m_peerTP`、触发 TP 帧解码（`connection_impl.cpp:812-833`），初值来源。
- **`Config`**（`cpp/include/utp/config.h`）：运行时流控初值来源。
- **`StreamImpl`**（`cpp/src/context/stream_impl.{h,cpp}`）：`kMaxRecvFragmentBytes` 定义兜底默认（`stream_impl.h:56`）；在应用读取数据时回调 `onStreamBytesConsumed`（`stream_impl.cpp:176,334`）驱动窗口推进。
- **发送路径 / `SendControl`（`cpp/src/context/send_ctl.cpp`）**：`sendPacket()` 承载流控帧（`UTP_TYPE_CTRL`）；`scheduleWrite()` 在窗口放开后唤醒发送。拥塞控制是并行的独立限制。
- **`Serialize`（`utils/serialize.hpp`）**：帧字段定长编解码。
- **`Status` / `UTP_ERR_*`**：错误码 `UTP_ERR_STREAM_FLOW_CONTROL`、`UTP_ERR_WOULD_BLOCK`、`UTP_ERR_INVALID_PARAM`、`UTP_ERR_OVERFLOW`、`UTP_ERR_FRAME_UNEXPECTED`。
- **`time::MonotonicUs()`**：限速与更新间隔判定的时间源。
