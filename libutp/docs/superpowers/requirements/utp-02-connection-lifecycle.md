# uTP 需求文档 02 — 连接生命周期 / 状态机 / CID

> 反推来源：`cpp/src/context/connection_impl.{h,cpp}`、`cpp/src/context/context_impl.{h,cpp}`、`cpp/src/context.cpp`（代码为唯一 ground truth）。
> doc/ 仅交叉参考。所有数值均来自代码；不确定处标注“待确认”。引用格式 `文件:行号`。

---

## 1. 职责与边界

本模块负责单条 uTP 连接从建立、握手收敛、稳态到关闭/异常的完整生命周期，以及 Context 层对多连接的解复用与生命周期收敛。

- `ConnectionImpl`（`connection_impl.h:44`）：单连接对象。持有状态机 `m_state`、本/对端 CID、包号空间、密钥上下文、发送控制、各类定时器（连接超时、HandshakeDone、ACK、keepalive、路径校验、关闭排空）。负责主动建连 (`connect`)、被动初始化 (`initPassive`)、收包驱动状态迁移 (`onUdpPacket`)、发包驱动 (`onWrite`)、错误/关闭收敛 (`abortConnection`/`beginCloseSent`)。
- `ContextImpl`（`context_impl.h:50`）：单 UDP socket 上的连接管理器。负责按 CID/地址解复用收包 (`onReadEvent`)、CID 分配 (`allocLocalCid`)、被动握手协商与 pending 队列 (`m_pendingIncoming`)、连接态收敛回调分发 (`handleConnectionState`)、主动连接重试 (`m_pendingConnections`)、0-RTT 票据校验与重放防护。
- `Context`（`context.cpp`）：薄公开封装，仅把 `Status`/int 结果归一化 (`NormalizePublicStatus`) 后转发给 `ContextImpl`。

边界：拥塞/重传（`SendControl`）、流（`StreamImpl`）、MTU、路径校验细节、加密密钥调度均为独立模块，本模块只在生命周期节点上调用它们。

---

## 2. 数据结构 / CID 模型

### 2.1 连接标识与包头 CID 语义

线路包头前两字段为 `scid`（发送方本地 CID）、`dcid`（发送方所记对端 CID = 接收方本地 CID）。

- 发送侧序列化顺序：先 `m_localConnectionID`(=scid)，后 `m_peerConnectionID`(=dcid)。`connection_impl.cpp:2728,2733`；pending 路径同序 `context_impl.cpp:844,846`。
- 接收侧解析顺序：先 `scid`，后 `dcid`。`context_impl.cpp:1480-1487`。
- 解复用主键：`dcid`。`m_connections.find(dcid)`（`context_impl.cpp:1513`）。

### 2.2 CID 分配

- `allocLocalCid`（`context_impl.cpp:1256`）：随机 `Random<uint32_t>(1, UINT32_MAX)`，`0` 保留为“未分配”，最多重试 `kMaxTry=8`，需同时不与 `m_connections`、`m_pendingIncoming` 冲突。
- 主动方：`startPendingConnectAttempt` 先 `allocLocalCid` 再构造 `ConnectionImpl(this,&m_udpSocket,cid)`（`context_impl.cpp:497-510`）。
- 被动方：`createAndInsertPassiveConnection` 用服务端自分配的 `localCid` 构造并 `initPassive`（`context_impl.cpp:1041-1048`）。
- 两端各自独立随机选择本地 CID；不存在“server 为 client 分配 CID”的语义。构造函数 `m_localConnectionID(cid)`（`connection_impl.cpp:339`）。

### 2.3 `dcid == 0` 语义

`dcid==0` 表示“发送方尚不知道对端 CID”。客户端首个 Initial 包 `m_peerConnectionID` 仍为 0，故 Initial 的 dcid=0。服务端据此识别被动 Initial（见 §3.4）。

### 2.4 Context 侧核心容器（`context_impl.h:238-252`）

- `m_connections`：`cid(dcid) -> ConnectionImpl::SP`，已建立/建立中的连接。
- `m_pendingConnections`：`ConnectionImpl* -> PendingConnectAttempt`，主动握手中的连接（含剩余重试次数 `retriesRemaining`）。
- `m_pendingIncoming`：`localCid -> PendingIncomingConnection`，被动握手中的对端（**尚未创建 ConnectionImpl**）。
- `m_pendingIncomingPeerIndex`：`PeerIndexKey{peerAddress, peerCid(scid)} -> localCid`，被动 Initial 去重索引（`context_impl.h:145-159`）。
- `m_pendingIncomingQueue`：已上报 `OnNewConnection`、待应用 `accept()` 的队列。
- `m_waitHandshakeDone`：已 `accept()`、等待对端 HandshakeDone 回声的 localCid 集合。

### 2.5 连接内关键成员（`connection_impl.h`）

`m_state`(295)、`m_localConnectionID`/`m_peerConnectionID`(317-318)、`m_packetNumber{1}`(319)、`m_isClientInitiator{true}`(386)、HandshakeDone 收敛字段 `m_handshakeRetryCount`/`m_handshakeDonePending`/`m_handshakeDoneSent`/`m_handshakeDoneLastPacketNo`/`m_peerHandshakePacketNo`/`m_handshakeReceivedAtUs`(354-359)、关闭收敛字段 `m_closeFramePending`/`m_closeErrorCode`/`m_closeReason`/`m_closePtoUs`/`m_closeDeadlineUs`/`m_closePeerResendCount`(360-369)、错误字段 `m_lastErrorCode`/`m_lastErrorReason`/`m_closeByPeer`/`m_closedNotified`(387-390)。

---

## 3. 状态机

### 3.1 状态枚举（`connection_impl.h:67-77`）

| 枚举 | 值 | 含义 |
|---|---|---|
| `kStateWaitSendInitial` | 0 | 等待发送 Initial（仅主动方） |
| `kStateInitialSent` | 1 | 已发送 Initial，等待对端 Handshake |
| `kStateHandshakeSent` | 2 | 已定义，**未被赋值使用**（见 §7/§8） |
| `kStateHandshakeReceived` | 3 | 已定义，**未被赋值使用** |
| `kStateConnected` | 4 | 已连接 |
| `kStateCloseSent` | 5 | 已发/待发本端 CONNECTION_CLOSE |
| `kStateCloseReceived` | 6 | 收到对端 CONNECTION_CLOSE |
| `kStatePtoTimedWait` | 7 | 关闭后 PTO 排空等待 |
| `kStateDisconnected` | 8 | 断连（初始态与终态） |

初始值 `m_state{kStateDisconnected}`（`connection_impl.h:295`）。枚举顺序被 `abortConnection` 的 `m_state >= kStateCloseSent` 守卫复用（`connection_impl.cpp:387`）——顺序具语义意义。

### 3.2 主动方（client）迁移

| 起点 | 触发 | 终点 | 代码 |
|---|---|---|---|
| `kStateDisconnected` | `connect()`（须处于 Disconnected，否则 `UTP_ERR_INVALID_STATE`） | `kStateWaitSendInitial` | `connection_impl.cpp:478-482` |
| `kStateWaitSendInitial` | `onWrite()` 成功 `sendInitialPacket()`（0-RTT 时首包由早期数据代替）+ `armConnectTimerForRound` 成功 | `kStateInitialSent` | `connection_impl.cpp:1137-1175` |
| `kStateWaitSendInitial` | `sendInitialPacket()` 失败 / 起定时器失败 | `kStateDisconnected`（并 `notifyConnectionClosed`） | `connection_impl.cpp:1149,1165` |
| `kStateInitialSent` | 收到 `UTP_TYPE_HANDSHAKE` 包（加密态须已具备 tx/rx 密钥，否则 abort） | `kStateConnected`；置 `m_handshakeDonePending=true`、`armHandshakeDoneTimer`、停 `m_connTimer` | `connection_impl.cpp:1040-1058` |
| `kStateInitialSent` | `onConnTimeout()` 且重试<max 且 socket 无可读数据 | 停留 `kStateInitialSent`，重发 Initial，`++m_handshakeRetryCount` | `connection_impl.cpp:3135-3147` |
| `kStateInitialSent` | `onConnTimeout()` 且重试耗尽 | `kStateDisconnected`（`UTP_ERR_TIMEOUT`） | `connection_impl.cpp:3160-3167` |

### 3.3 被动方（server）迁移

- `initPassive()`（`connection_impl.cpp:558`）要求当前为 `kStateDisconnected` 且 `peerAddress` 有效、`peerConnectionID != 0`（`:565-571`），**直接置 `kStateConnected`**（`:617`），随即 `markPeerActivity` + `scheduleWrite`。即被动连接对象一经创建即为已连接态，无独立握手中间态。
- 时序上：普通被动连接的 `ConnectionImpl` 只在服务端 **收到客户端 HandshakeDone 回声后** 才由 `context` 层惰性创建（§4.2），故创建时握手已实质完成；0-RTT 被动路径则在 accept 决策后立即创建（`context_impl.cpp:1772`）。

### 3.4 被动 Initial / 0-RTT 判定（`connection_impl.cpp:642-647`）

```
isPassiveInitial  = (m_state==kStateDisconnected) && type==UTP_TYPE_INITIAL && header.dcid==0
isPassiveZeroRtt  = (m_state==kStateConnected)   && type==UTP_TYPE_0RTT    && header.dcid==0
                    && (m_peerConnectionID==0 || header.scid==m_peerConnectionID)
```
非上述两类且 `dcid != m_localConnectionID` 的包被丢弃（`:647-649`）。收到有效包后 `m_peerConnectionID = header.scid`（`:660`）。

被动 Initial 在 Context 层的解复用键（`context_impl.cpp:1805-1835`）：
- 先按 `(scid==desc.dcid && peerInfo.ip==peerIp && peerInfo.port==peerPort)` 判定是否已属某已建连接（去重，`:1819-1821`）。
- 再按 `PeerIndexKey{peerAddress, peerCid=scid}` 判定是否已在 pending 集合（去重，`:1830-1835`）。
- 未命中则 `allocLocalCid` 新建 `PendingIncomingConnection`。

### 3.5 关闭 / 异常迁移

| 起点 | 触发 | 终点 | 代码 |
|---|---|---|---|
| 任意<`kStateCloseSent` | 应用 `close()` → `beginCloseSent(UTP_ERR_OK)` | `kStateCloseSent`，置 `m_closeFramePending` | `connection_impl.cpp:3948-3954,3208-3237` |
| <`kStateCloseSent` | `abortConnection()`（致命错误） | `kStateCloseSent`，发 close，**立即 `notifyConnectionClosed`** | `connection_impl.cpp:385-414` |
| 任意 | 收到 `kFrameConnectionClose` | `kStateCloseReceived`，`m_closeByPeer=true` | `connection_impl.cpp:951-952` |
| `kStateCloseSent`/`kStateCloseReceived` | `onWrite()` 把 close 发完 | `enterPtoTimedWait()` → `kStatePtoTimedWait`（除非当前是 CloseReceived，则保持） | `connection_impl.cpp:1097,1116,3310-3320` |
| `kStateCloseSent`/`kStatePtoTimedWait`/`kStateCloseReceived` | `onCloseDrainTimeout()` | `kStateDisconnected`，`notifyConnectionClosed` | `connection_impl.cpp:3335-3346` |
| `kStatePtoTimedWait` | 收包 | 静默（仅探测 close 帧，不回包） | `connection_impl.cpp:687-708` |
| `kStateConnected` | keepalive 探测超限 | `abortConnection(UTP_ERR_TIMEOUT)` | `connection_impl.cpp:3264-3268` |

关闭时 `onWrite()` 若 `sendConnectionCloseFrame` 返回 `WOULD_BLOCK` 则 `scheduleWrite` 重试；其他失败直接转 `kStateDisconnected`（`connection_impl.cpp:1120-1134`）。CloseReceived 侧对端 close 只回一次，由 `m_closePeerResendCount` 与 `kMaxCloseResendCount=3` 限制（`:980,1094,1113`）。

---

## 4. HandshakeDone 收敛机制

设计上 HandshakeDone 由 **主动方(client)** 发往被动方(server)，用于确认服务端 Handshake 包号并驱动服务端把 pending 提升为已连接。

### 4.1 客户端发送与重传

- 进入 `kStateConnected`（收到 Handshake）时置 `m_handshakeDonePending=true`、`m_handshakeDoneSent=false`、`armHandshakeDoneTimer()`（`connection_impl.cpp:1052-1055`）。
- `sendHandshakeDonePacket()`（`connection_impl.cpp:2320`）：以 `UTP_TYPE_CTRL` 发送 `HANDSHAKE_DONE + HANDSHAKE_DELAY` 帧，携带 `m_peerHandshakePacketNo` 与 `nowUs - baseUs` 延迟；成功后置 `m_handshakeDoneSent=true`、记 `m_handshakeDoneLastPacketNo`、重新 `armHandshakeDoneTimer`。
- 重传定时器 `onHandshakeDoneTimeout()`（`connection_impl.cpp:3046`）：仅在 `pending && kStateConnected` 时重发；失败则 `scheduleWrite` 且 10ms 后再试。
- 定时器间隔 `handshakeDoneDelayMs() = effectiveHandshakeTimeoutMs()/3`（下限 1ms，`connection_impl.cpp:3083-3087`）。
- 停止条件 `onHandshakeDoneFrameAcked()`（`connection_impl.cpp:3059`）：清 `m_handshakeDonePending` 并停表。
- Piggyback：`shouldPiggybackHandshakeDone(len,fin)` = `connected && pending && (len>0 || fin)`（`connection_impl.cpp:3123`），允许在业务 STREAM 发送时搭载 HandshakeDone。
- 若连接后又收到 `UTP_TYPE_HANDSHAKE`（服务端重发），重新置 pending 并重启定时器（`connection_impl.cpp:1060-1065`）。

### 4.2 服务端收敛（Context 层）

- `accept()`（`context_impl.cpp:727`）：从 `m_pendingIncomingQueue` 取队首，`sendPendingHandshake()` 发送 server hello（`UTP_TYPE_HANDSHAKE`，含 version/TP/crypto/ack_frequency/handshake_delay，`context_impl.cpp:888`），置 `handshakeSent=true`、记 `lastHandshakePacketNo`，加入 `m_waitHandshakeDone`。此刻**尚未创建 ConnectionImpl**。
- `onReadEvent()` 收到 `dcid` 命中 pending 且 `handshakeSent` 的包（`context_impl.cpp:1544`）：解析帧，若含 `kFrameHandshakeDone` 且 `done.ack_handshake_pn == pending.lastHandshakePacketNo`（`:1577-1578`）→ `handshakeDone=true`，则 `createAndInsertPassiveConnection` 创建被动连接、`removePendingIncoming`、回调 `m_onConnected`、`replayBufferedPendingPackets` 重放握手期缓存的业务包，再投递当前包（`:1608-1642`）。
- HandshakeDone 到达前的业务包会被缓存（数量/字节受 `PendingPreHandshakeBufferMaxPackets/Bytes` 限制，`context_impl.cpp:1591-1604`）。
- 服务端 pending 握手重传由 `processPendingHandshakeTimeouts()` 驱动（`context_impl.cpp:1327`，上限 `handshake_max_retries`）。

### 4.3 会话票据（server→client，稳态）

`maybeSendSessionTokenPacket()`（`connection_impl.cpp:2346`）：仅被动方(`!m_isClientInitiator`)、`kStateConnected`、未发过时，构造 0-RTT 恢复票据并以 `UTP_TYPE_CTRL` 下发，置 `m_sessionTokenIssued`。

---

## 5. 不变量与规则（MUST / MUST NOT）

- MUST：`connect()`/`initPassive()` 仅在 `kStateDisconnected` 下允许，否则返回 `UTP_ERR_INVALID_STATE`（`connection_impl.cpp:478,565`）。
- MUST：`initPassive` 的 `peerConnectionID != 0` 且 `peerAddress` 有效（`connection_impl.cpp:569`）。
- MUST：本地 CID 非 0，且在 `m_connections`+`m_pendingIncoming` 内唯一（`context_impl.cpp:1256-1272`）。
- MUST：`pn==0` 的包一律丢弃；重复 `pn`（`m_receiveHistory.contains`）丢弃（`connection_impl.cpp:652`）。
- MUST：非握手包若 `dcid != m_localConnectionID` 且非被动 Initial/0-RTT 则丢弃（`connection_impl.cpp:647`）。
- MUST：TransportParams / Crypto 帧只允许出现在 `UTP_TYPE_INITIAL`/`UTP_TYPE_HANDSHAKE` 包中，否则 `UTP_ERR_FRAME_UNEXPECTED` → abort（`connection_impl.cpp:817-819,898-899`）。
- MUST：仅记录首个致命错误——`recordConnectionError` 在 `m_lastErrorCode != UTP_ERR_OK` 时抑制后续错误（`connection_impl.cpp:422-426`）。
- MUST：`notifyConnectionClosed` 幂等，`m_closedNotified` 保证只回调一次（`connection_impl.cpp:3382-3389`）。
- MUST NOT：`abortConnection` 在 `m_state >= kStateCloseSent` 时为 no-op（不重复关闭，`connection_impl.cpp:387`）。
- MUST NOT：`kStatePtoTimedWait` 下不再回发任何包（含对端回声 close，`connection_impl.cpp:704-707`）。
- MUST NOT：进入 closing 态后除 `kFrameConnectionClose` 外所有帧被忽略（`connection_impl.cpp:732-734`）。
- MUST：加密连接从 `kStateInitialSent` 转 `kStateConnected` 时须已具备 tx/rx AES-GCM 上下文，否则 abort（`connection_impl.cpp:1040-1047`）。
- MUST：candidate 路径验证通过前，仅处理 PATH_CHALLENGE/PATH_RESPONSE/CONNECTION_CLOSE，业务帧忽略（`connection_impl.cpp:737-740`）。

---

## 6. 参数与默认值（变量名 + 确切值）

连接内常量（`connection_impl.cpp:77-98`）：

| 变量 | 值 | 用途 |
|---|---|---|
| `kMinPtoUs` | 10000 (10ms) | close PTO 下限 |
| `kDefaultPtoUs` | 333333 (~333ms) | srtt 未知时的 PTO |
| `kMaxPtoUs` | 60000000 (60s) | close PTO 上限 |
| `kMaxCloseResendCount` | 3 | 被动 close 最大回发次数 |
| `kPathValidationSendCredit` | 256 | 路径验证发送额度 |
| `kMinAckFrequencyApplyIntervalMs` | 1000 | 应用对端 AckFrequency 最小间隔 |

Config 默认值（`cpp/include/utp/config.h`）：

| 字段 | 默认 | 生命周期用途 |
|---|---|---|
| `handshake_timeout` | 800 (ms) | 握手首轮超时基准（`localHandshakeTimeoutMs` 回退 1000ms，`connection_impl.cpp:3097`） |
| `handshake_max_retries` | 2 | Initial/被动握手重传上限 |
| `keepalive_interval` | 0（0 时按 `max_idle_timeout` 推算） | keepalive 探测间隔 |
| `keepalive_timeout` | 1500 (ms) | 单次探测等待；也用作 `NetworkPath` 初值（`connection_impl.cpp:340`） |
| `keepalive_probes` | 3 | 最大丢失探测数（超限即超时 abort） |
| `max_idle_timeout` | 30000 (ms) | 空闲阈值 |
| `ack_every_n_packets` | 4 | 初始 ack-eliciting 阈值 |
| `ack_delay` | 25 (ms) | 最大 ACK 延迟 |
| `pending_incoming_limit` | 1024 | 全局未完成被动握手上限 |
| `pending_incoming_per_ip_limit` | 32 | 单 IP 未完成被动握手上限 |
| `pending_unaccepted_timeout_ms` | 3000 (ms) | 等待应用 accept 的最长时间 |

派生时序：
- `handshakeDoneDelayMs = effectiveHandshakeTimeoutMs()/3`（下限 1，`:3083`）。
- `effectiveHandshakeTimeoutMs = min(local, peerTP.handshake_timeout)`（若对端携带，`:3100`）。
- `handshakeTimeoutForRoundMs = ExponentialBackoffTimeoutMs(base, round)`，即 `base << round`，round≥31 饱和为 UINT32_MAX（`:170-180,3109`）。
- `keepaliveIntervalMs`：`min(localInterval, peerSafeInterval)` 下限 1；`peerSafeInterval = peerIdleTimeout - max(3*srtt_ms, 50)`（`:3170-3186`）。
- close PTO：`srtt + max(varianceWeight*rttVar, granularity)`，被动关闭 `varianceWeight=1` 否则 2，CLAMP 到 [10ms,60s]；排空 deadline = `now + closePtoUs*3`（`:3282-3308,3326`）。
- 客户端 Initial 内嵌 AckFrequency 默认：`ack_eliciting_threshold=min(ack_every_n_packets,255)`、`reordering_threshold=3`、`max_ack_delay_ms=ack_delay`（`:2447-2453`）。

---

## 7. 对外接口 / 回调

### 7.1 Context 层（`context.cpp` → `context_impl.cpp`）

- `bind(ip,port,ifname)`、`connect(ConnectInfo)`、`connect0Rtt`、`connect0RttWithState`、`accept()`。
- 回调注册：`setOnConnected(OnConnected)`、`setOnConnectError(OnConnectError)`、`setOnNewConnection(OnNewConnection)`、`setOnConnectionClosed(OnConnectionClosed)`、`setOnZeroRttDecision(OnZeroRttDecision)`。
- 触发点（`handleConnectionState`，`context_impl.cpp:398`）：
  - `kStateConnected` 且在 pending 集合 → `m_onConnected(current)`，出 pending（`:413-419`）。
  - pending 连接转 `kStateCloseSent/CloseReceived/PtoTimedWait/Disconnected` 且重试耗尽 → `reportConnectError(...)`（`:423-444,453-481`）。
  - 已建立连接转 `kStateDisconnected` → `m_onConnectionClosed(current)` 并从 `m_connections` 移除（`:482-491`）。
  - 被动 promote 成功时另有一处 `m_onConnected(conn)`（`:1634`、0-RTT 路径 `:1798`）。
  - 被动 Initial 上报 `m_onNewConnection(info)`（`:1976`），返回 false 且未 accept 则回 close 并清理（`:1983-1990`）。

### 7.2 Connection 层（`connection_impl.h` / `connection.h` 覆写）

- `setOnIncomingStream`、`setOnSessionTokenReady`、`setOnError`、`setOnClosed`、`close()`、`createStream`、`getStream`、`exportSessionToken`/`exportSessionResumptionState` 等。
- `notifyConnectionError(code,reason)`（`:3363`）经 `m_onError(ConnectionErrorInfo)` 上报（`UTP_ERR_OK` 时跳过）。
- `notifyConnectionClosed(code,reason,byPeer)`（`:3380`）经 `m_onClosed(ConnectionCloseInfo{code,reason,by_peer})` 上报，幂等。

---

## 8. 当前实现边界（已实现 / 预留 / TODO + 位置）

已实现：
- 主/被动建连、握手重传、HandshakeDone 收敛与 piggyback、keepalive、优雅关闭与异常 abort、PTO 排空、0-RTT 票据/状态建连、路径变更 candidate 校验。

预留 / 未使用：
- `kStateHandshakeSent`(值2)、`kStateHandshakeReceived`(值3) 已定义但**全代码无赋值**（`connection_impl.h:69-71`，`m_state =` 赋值处见 §3 列表，无此二者）。当前握手在 client 侧是 `kStateInitialSent → kStateConnected` 一跳，server 侧直接 `kStateConnected`，中间态被跳过。

TODO / 可疑代码痕迹：
- `abortConnection` 内注释坦言缺少专用 `armCloseDrainTimer()`，改用 `handshakeDoneDelayMs()` 作为排空定时器时长，并把 `m_closeDeadlineUs` 设为 `now + closePtoUs()`（1×PTO）（`connection_impl.cpp:407-411`）——与优雅关闭路径经 `enterPtoTimedWait()` 的 `3×PTO`（`:3326`）不一致。见 §9 风险。
- `sendPendingHandshake` 中存在空 `else {}` 分支（`context_impl.cpp:962-963`），发送失败仅靠返回值，无额外处理。

---

## 9. 与 doc/ 的差异

参考 `doc/连接与异常处理流程.md`、`doc/协议设计文档.md`、`doc/设计实现文档.md`。总体一致，以下为差异/需注意点（以代码为准）：

1. **HandshakeDone 方向**：doc 未明确强调，代码中 HandshakeDone 由 **client → server**（client 收到 server Handshake 后发，确认 server 的握手包号），服务端据此把 pending 提升为连接。这是 uTP 与标准 QIC（server 发 HANDSHAKE_DONE）相反的设计。
2. **服务端连接对象惰性创建**：`accept()` 只发 server hello、不建 `ConnectionImpl`；普通连接对象在收到 client HandshakeDone 后才创建（`context_impl.cpp:1608-1642`）。doc 未突出这一惰性时序。
3. **中间握手态未用**：doc 的状态叙述聚焦 WaitSendInitial→InitialSent→Connected，与代码一致；但枚举里保留的 `kStateHandshakeSent/Received` 在实现中恒未使用（§8）。
4. **关闭排空窗口不一致**：`doc/连接与异常处理流程.md:182` 描述“3×PTO 观察窗口”，`enterPtoTimedWait` 符合（`3×PTO`）；但 `abortConnection` 使用 `handshakeDoneDelayMs()` 起表 + `1×PTO` deadline，且**立即** `notifyConnectionClosed`，与优雅关闭“排空后再通知”的语义不同（§8）。
5. **包号起点**：`doc/连接与异常处理流程.md:119` 称“主动方从 0 起步，被动侧从 1 起步”；代码中 `m_packetNumber{1}`（`connection_impl.h:319`），首个 `packetNumber()` 返回 1，且 `pn==0` 被视为非法丢弃（`connection_impl.cpp:652`）。主动方“从 0”与实现不符 —— **待确认**（可能指内部计数器口径差异）。

---

## 9. 依赖

- `SendControl`（`send_ctl.h`）：发包调度、ACK 处理、PTO/重传；`onWrite` 与关闭排空依赖 `scheduledCount()`/`onCanWrite`。
- `StreamImpl`（`stream_impl.h`）：流数据发送、piggyback HandshakeDone。
- 加密：`X25519Wrapper`、`AesGcmContext`、`TrafficKeySchedule`、`TokenAuth`、`ResumptionStateCodec`（握手密钥、0-RTT 票据校验/生成）。
- `NetworkPath`、`MtuDiscovery`、`ReceiveHistory`、`RttStats`、`TransportParams`、`FrameAckFrequency`。
- `ev::EventTimer` / `event_base`：全部生命周期定时器；`ev::EventPoll`：读写事件。
- 帧编解码 `proto/frame/*`、`PacketIn`/`PacketOut`、`Serialize`。
