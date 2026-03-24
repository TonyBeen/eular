# libutp 实现设计文档（实现对齐版）

> 更新时间：2026-03-22  
> 目的：对齐当前代码实现，明确“已实现能力 / 未接入能力 / 风险与注意事项 / 下一步优化方向”。

---

## 1. 文档范围

本文档基于当前仓库实现整理，重点覆盖：

- 连接与事件驱动框架
- 包结构、帧编解码与接收解析
- ACK、丢包检测、重传与拥塞控制
- 路径验证与抗放大限制
- 内存池与对象生命周期
- 安全模块接入状态
- 测试覆盖与已知限制

不覆盖完整协议规范推导（详见仓库根目录 `协议设计文档.md` 的协议视角内容）。

---

## 2. 当前实现状态总览

| 模块 | 状态 | 说明 |
|---|---|---|
| Context/连接调度 | ✅ 已实现 | 支持 `bind/connect`、读写事件驱动、连接表管理 |
| 主动建连流程 | ✅ 可用 | 发送 Initial，收到 Handshake 后置 Connected |
| 被动接入/accept | ✅ 已实现 | 两阶段流程：`OnNewConnection` 决策 -> `accept()` 发 Handshake -> 等待 `HandshakeDone` 后建连 |
| Stream 业务层 | ✅ 可用 | 已支持 `createStream/getStream`、收发缓冲与读写回调，stream 配额按活跃流统计 |
| PacketIn 解析 | ✅ 已实现 | 固定头解析、payload 长度校验、帧遍历 |
| PacketOut 生命周期 | ✅ 已实现 | `MemoryManager` 池化分配/归还，支持重传 attempt 链 |
| ACK 解析与发送侧处理 | ✅ 已实现 | `FrameAck::decode` + `SendControl::onAckReceived` |
| ACK 发送生成 | ✅ 已接入 | 已支持独立 ACK timer、包计数阈值/延迟二选一触发、乱序快速 ACK，以及流发送时的 ACK piggyback |
| 丢包检测/重传 | ✅ 已实现 | FACK 检测 + Lost 队列 + 新包号重传 |
| 拥塞控制 | ✅ BBRv1 已接入 | `SendControl` 默认使用 `BbrV1` |
| Cubic | ❌ 未实现 | `cubic.cpp` 仍是占位程序 |
| Path Challenge/Response | ✅ 已实现 | 地址变化检测、挑战应答、超时重试与失败 |
| Keepalive（空闲保活） | ✅ 已实现 | 空闲定时发 Ping，收到任意包刷新活动时间，连续 N 次无响应超时断开（N 可配置） |
| 会话票据 TokenAuth | ✅ 可用（模块级） | AEAD token 封装/解封装已实现 |
| Crypto 帧与握手接入 | ✅ 已实现 | `FrameCrypto` 编解码已实现，并接入 Initial/Handshake |
| AES-GCM 包级收发接入 | ✅ 已实现 | `AesGcmContext::encrypt(PacketOut*)/decrypt(PacketIn*)` 已接入 |
| 0-RTT 重连 | ✅ Phase-3 已接入 | 已支持票据携带/验证、`UTP_TYPE_0RTT` 发送与 pending 缓存回放，并具备短窗口抗重放去重 |

---

## 3. 分层架构

### 3.1 对外 API 层

- `Context`（`include/utp/utp.h`）：
  - 生命周期：`bind/connect/accept`
  - 回调：`OnConnected/OnConnectError/OnNewConnection/OnConnectionClosed`
  - `OnConnectError` 统一参数为 `ConnectAttemptInfo`，同时覆盖同步前置失败与异步握手失败
  - `OnNewConnection` 当前签名为 `bool(const NewConnectionInfo&)`，返回值用于放行/拒绝被动连接
- `Connection`（`include/utp/connection.h`）：
  - 连接统计、描述信息
  - 流创建与查询入口（`createStream/getStream`）
- `Stream`（`include/utp/stream.h`）：
  - 支持 `write/read` 与零拷贝 `acquire/commit` 接口

### 3.2 内部实现层

- `ContextImpl`
  - 维护 UDP socket 与读写事件
  - 按 DCID 分发到 `ConnectionImpl`
  - 被动建连采用 pending 状态机：先记录 pending，再由 `accept()` 发 Handshake，收到 `HandshakeDone` 后晋升为正式连接
- `ConnectionImpl`
  - 连接状态机、收发主流程、路径验证
  - 发送成功后把包交给 `SendControl` 管理生命周期
- `SendControl`
  - inflight/unacked/lost 队列
  - ACK 驱动的拥塞控制、丢包检测、重传计时器

### 3.3 协议对象层

- `PacketIn`：接收包解析与帧遍历
- `PacketOut`：发送包对象、原始缓冲、状态位
- `Frame*`：各帧编解码（`ack/path/version/stream/padding/session_token/connection_close`）

### 3.4 算法与工具层

- 拥塞控制：`BbrV1`（已接入），`Cubic`（占位）
- 速率控制：`Pacer`
- 历史信息：`ReceiveHistory`、`SendHistory`
- 内存管理：`MemoryManager + MaloCacheLine + SLIST`
- 安全模块：`X25519Wrapper`、`AesGcmContext`、`TokenAuth`

---

## 4. 包与头部模型

### 4.1 固定头（20 字节）

`UTPHeaderProto`（`src/proto/proto.h`）字段顺序：

1. `scid`（4B）
2. `dcid`（4B）
3. `pn`（8B）
4. `payload_length`（2B）
5. `types`（1B）
6. `reserve`（1B）

### 4.2 包类型（Types）

当前定义：`Initial / Handshake / 0RTT / ConnectionClose / Ctrl`。

现状注意：当前实际主流程主要在 `Initial` 和 `Ctrl`（Path/Ack 等控制帧）上运转。

---

## 5. 收发主流程

## 5.1 发送流程（当前）

1. `ConnectionImpl::sendPacket` 分配 `PacketOut`（来自 `MemoryManager`）
2. 序列化固定头和 payload
3. 使用 `UdpSocket::send` 发送
4. 成功后交给 `SendControl::packetSent`，进入 unacked 队列

关键点：

- 发送路径对 `payloadLen`、`packetLen` 做了 `uint16_t` 上限保护
- 路径验证阶段应用抗放大限制（见 §8）
- 当前发送接口是单 buffer（`data + len`）

## 5.2 接收流程（当前）

1. `ContextImpl::onReadEvent` 按 DCID 找连接
2. `ConnectionImpl::onUdpPacket` 用 `PacketIn` 解析
3. 遍历帧：
   - `PathChallenge` -> 回 `PathResponse`
   - `PathResponse` -> 验证路径
   - `Ack` -> `SendControl::onAckReceived`
   - `ConnectionClose` -> 更新连接状态
4. 被动建连：
  - unknown DCID 且包类型为 `Initial` 时创建 pending 条目并触发 `OnNewConnection`
  - 回调返回 `true` 后等待应用调用 `accept()`
  - `accept()` 成功后发送 Handshake 并启动等待 `HandshakeDone` 超时计时
  - 收到 `HandshakeDone` 后创建 `ConnectionImpl`，并触发 `OnConnected`

---

## 6. 帧支持矩阵（实现 vs 接入）

| 帧类型 | 编解码实现 | 接入连接主流程 | 说明 |
|---|---|---|---|
| `Stream` | ✅ | ✅（收发已接入） | 已接入连接收包分发、发送队列与可读回调 |
| `Ack` | ✅ | ✅（收发已接入） | ACK 解析驱动发送侧回收/拥塞控制，且可在接收路径生成发送 |
| `Padding` | ✅ | ⚠️ 未见主动生成路径 | 编解码完整 |
| `ConnectionClose` | ✅ | ✅（接收） | 收到后状态置 `CloseReceived` |
| `Ping` | ✅ | ✅ | 已接入 keepalive 探测发送，MTU 探测也会构造 Ping |
| `PathChallenge` | ✅ | ✅ | 地址变化验证 |
| `PathResponse` | ✅ | ✅ | 回显校验数据 |
| `SessionToken` | ✅ | ⚠️ 未见连接流程集成 | 编解码和 token 模块都有 |
| `AckFrequency` | ✅ | ✅ | 已接入握手协商 + 事件驱动自适应更新（RTT/丢包持续变化触发），并联动发送侧重排序阈值 |
| `Version` | ✅ | ✅ | 当前 Initial 发送该帧 |
| `Crypto` | ✅ | ✅ | 用于加密握手参数与公钥交换 |
| `HandshakeDone` | ✅（长度解析） | ✅（被动建连完成信号） | 用于 pending -> Connected 晋升 |

---

## 7. ACK、丢包检测与重传

## 7.1 ACK 处理

- 入口：`ConnectionImpl::onUdpPacket` 解析 `FrameAck`
- 数据结构：`AckInfo`（包含 `largest_ack_packno`、ranges）
- 控制逻辑：`SendControl::onAckReceived`
  - 从 unacked 队列移除被确认包
  - 回调拥塞控制 `onBeginAck/onAck/onEndAck`
  - 触发后续丢包检测与重传定时器更新
- ACK 发送策略：
  - 收到 ack-eliciting 包后，按 `ack_eliciting_threshold` 或 `max_ack_delay_ms` 触发 ACK
  - 增加独立 ACK timer，避免“只收 1 个包后无后续流量”时 ACK 长时间滞留
  - 收到超过 `reordering_threshold` 的包号缺口时立即快速 ACK
  - 本端若恰好要发 Stream 数据，会优先把待发送 ACK piggyback 到同一包里
- AckFrequency：
  - 握手包会携带 `AckFrequency`
  - 收包侧会更新连接级 `ack_eliciting_threshold / reordering_threshold / max_ack_delay_ms`
  - `reordering_threshold` 会同步写入 `SendControl::m_reorderThresh`，用于发送侧 FACK 丢包判定
  - 本端不会定时广播 `AckFrequency`，仅在事件驱动评估时发送更新
  - 评估维度：`RTT` 持续上升（LatencySensitive）与丢包频繁（Lossy）
  - 采用三档策略：`Stable / LatencySensitive / Lossy`
  - 档位切换有持续时间门限（升档更快、回退更慢）与最小发送间隔，避免参数抖动

## 7.2 丢包检测

- 策略：基于 FACK + 重排序阈值（默认 3，且可被 AckFrequency 动态联动调整）
- 函数：`SendControl::detectLosses`
- 命中后：包从 unacked 移入 lost 队列，并打 `kPoLost/kPoLossRecorded` 等标志

## 7.3 重传（当前策略）

- 调度：`SendControl::onCanWrite` 消费 lost 队列
- 重传函数：`retransmitLostPacket`
- 关键行为：
  - 使用新包号重传（通过重写包头 `pn`）
  - 保持 payload 复用，不额外复制整包数据

## 7.4 轻量 attempt 链（本次已接入）

`PacketOut` 新增发送尝试链：

- 节点内容：`packet_no + sent_time + next`
- 记录时机：
  - 首次发送 `SendControl::packetSent`
  - 重传发送 `SendControl::retransmitLostPacket`
- 清理时机：
  - `PacketOut::Destroy`
  - `MemoryManager::putPacketOut`

设计目的：

- 保留发送历史（为后续 RTT/诊断/更细粒度恢复做准备）
- 不复制 payload，控制额外内存成本

---

## 8. 路径验证与抗放大

`NetworkPath` 维护 `Unknown/Validated/Validating/Failed` 四态。

### 8.1 触发

- 收包时检测到对端地址变化：进入 `Validating`

### 8.2 验证机制

- 发送 `PathChallenge`（8 字节随机数据）
- 收到匹配 `PathResponse` 后切到 `Validated`
- 超时后重试，达到上限进入 `Failed`

### 8.3 抗放大限制

`ConnectionImpl::canSendOnCurrentPath` 在 validating 状态下对普通业务流量限制：

- 允许发送上限：`m_bytesIn * 3 + 256`
- `PathChallenge/PathResponse/Ping/Ack/ConnectionClose` 豁免

注意：这能降低路径伪造时的放大量风险。

---

## 9. 拥塞控制与速率控制

## 9.1 BBRv1（已接入）

`SendControl::init` 默认实例化 `BbrV1`。

`BbrV1` 具备：

- 模式：`StartUp/Drain/ProbeBW/ProbeRTT`
- ACK 驱动带宽采样与最小 RTT 更新
- 恢复窗口、增益周期、ACK 聚合处理

## 9.2 Cubic（未完成）

- `cubic.h` 仅声明类
- `cubic.cpp` 为占位程序，未实现 `Congestion` 接口

## 9.3 Pacer

- `Pacer` 接口已具备
- `SendControl::canSend` 已按 `cwnd + pacer` 约束发送节奏

---

## 10. 内存管理与生命周期

## 10.1 MemoryManager

- `PacketOut` 对象池：`MaloCacheLine<PacketOut>`
- raw buffer 池：按桶（5档）复用，降低 malloc/free 开销
- `PacketIn` 对象池：`MaloCacheLine<PacketIn>` + 3 桶接收缓冲池
- 构造时初始化池头；析构时释放缓存链表

## 10.2 PacketOut 生命周期

- 分配：`MemoryManager::getPacketOut`
- 发送后持有者：`SendControl`
- ACK 回收/销毁：`SendControl::destroyPacket` -> `MemoryManager::putPacketOut`
- 复用前清理：attempt 链会被清空

注意：`PacketIn` 池化已接入 `ConnectionImpl::onUdpPacket` 主收包路径；`ContextImpl` 的 pending 解码路径仍为栈对象，可继续统一。

## 10.3 PacketIn 生命周期（当前）

- 分配：`MemoryManager::getPacketIn`
- 使用：`ConnectionImpl::onUdpPacket`（含解密分支）
- 回收：`MemoryManager::putPacketIn`
- 缓冲复用：按接收包大小映射到 3 桶，支持统计与收缩

---

## 11. 安全模块现状

## 11.1 已实现（模块级）

- `X25519Wrapper`：密钥对生成与共享密钥推导
- `AesGcmContext`：通用明文/AAD 加解密接口
- `TokenAuth`：会话 token（AES-GCM）封装与校验

## 11.2 未完成（协议接入级）

- `AckFrequency` 帧已实现并接入主流程；当前已支持事件驱动自适应更新，后续可继续增强统计可观测性与参数学习能力
- Cubic 拥塞控制尚未实现
- 0-RTT 重连已接入 Phase-3（见 §16 设计草案与实现状态）

---

## 11.3 连接关闭与收尾（最新行为）

1. 进入 `CloseSent/CloseReceived` 后：
  - 停止正常 stream 数据面发送。
  - 接收侧仅保留 `ConnectionClose` 最小处理，其它业务帧忽略。
2. 收到对端 `ConnectionClose` 后：
  - 回发 close 并进入 PTO timed-wait。
  - timed-wait 下仅在再次收到对端 close 时被动重发，最多 3 次。
3. 3*PTO 到期后：
  - 状态转 `Disconnected`，交由 `ContextImpl::handleConnectionState` 收敛回调。

---

## 12. 测试覆盖现状

当前已有 Catch2 单测：

- `test_packet_in.cc`：包头解析、帧迭代、截断保护
- `test_packet_out.cc`：创建/重置/尺寸边界
- `test_frame_ack_decode.cc`：ACK 解码范围与 `largest_ack_packno`
- `test_frame_ack_frequency.cc`：AckFrequency 默认回退、超限钳制与 encode/decode 归一化
- `test_ack_behavior.cc`：独立 ACK timer、乱序快速 ACK、ACK piggyback、发送侧阈值联动与 RTT 持续升高触发的自适应 AckFrequency 更新
- `test_frame_path.cc`：Challenge/Response 编解码
- `test_frame_session_token.cc`：Token 帧编码/截断
- `test_network_path.cc`：路径状态机与超时失败
- `test_receive_history.cc`：区间合并/裁剪/cutoff 语义

尚缺典型集成测试：

- 连接级端到端（握手->发送->ACK->重传）
- Path validating 与抗放大联动
- BBR 状态迁移回归

---

## 13. 已知限制与注意事项

## 13.1 功能限制

1. 被动连接采用异步交付：`accept()` 仅发起握手并返回错误码，连接对象通过 `OnConnected` 回调提供。
2. 主动/被动连接共享同一连接表（key=本端 CID），CID 分配已同时规避已建立连接与被动 pending 冲突。
3. `OnNewConnection` 返回 `false` 时会立即回 `ConnectionClose(UTP_ERR_CANCELLED)`。
4. `HandshakeDone` 超时会回 `ConnectionClose(UTP_ERR_TIMEOUT)` 并触发 `OnConnectError`。

## 13.2 实现细节注意点

1. 重传现为“就地换 PN + 轻量 attempt 链”，不复制 payload；但 attempt 节点按次分配，后续可考虑 slab/池化。
2. `UdpSocket::send` 仍是单 buffer 模型，重传优化空间在 scatter-gather 发送。
3. `FrameTypeToString` 在头文件声明形式与实现文件存在编译告警（链接/内联风格需统一）。
4. 工程内有宏重定义告警（`INVALID_SOCKET`、`UTP_THREAD_LOCAL`），建议统一平台宏来源。
5. `AesGcmContext::init` 成功路径应补全显式返回值（当前代码需关注）。
6. `ContextImpl` pending 握手解码路径尚未复用 `PacketIn` 池（与主连接路径存在实现差异）。
7. `keepalive_interval=0` 时当前按 `max_idle_timeout - 3*srtt` 近似推导，属于工程策略而非严格规范条款。

---

## 14. 下一步优化 TODO（建议优先级）

### P0（建议优先）

1. 补齐被动建连相关测试（放行/拒绝、accept 后等待 HandshakeDone、超时回收）。
2. 增加连接级端到端回归（主动/被动混合并发、CID 冲突回避场景）。
3. 扩展 Stream 生命周期回收策略（已关闭流的对象回收与统计）。
4. 0-RTT 可观测性增强：补充 accepted/rejected 事件与统计。

### P1

1. `UdpSocket` 增加 scatter-gather（header + payload iovec）发送能力，减少重传拷贝成本。
2. 将 attempt 记录接入 ACK 统计（更细粒度 RTT/重传分析）。
3. 将 `PacketIn` 池化扩展到 pending 握手路径并补齐统计对齐。

### P2

1. 完成 Cubic 实现并通过配置切换。
2. 增加连接级集成测试与压力测试。
3. 统一告警项（宏、声明风格）以提高 CI 信噪比。

---

## 15. 结论

当前 `libutp` 已完成“连接骨架 + 主被动握手 + ACK 收发闭环 + 重传/拥塞控制 + 路径验证 + 基础内存池 + 包级加密接入”的主干能力。  
尚未完成的重点转为“Stream 数据面端到端能力、Cubic 实现和更完整的连接级回归测试”。

---

## 16. 0-RTT 实现设计草案（第一版）

目标：在保持现有协议风格前提下，实现可控、可回退的 0-RTT 重连能力，优先落地最小可用闭环。

### 16.1 设计目标

1. 客户端可在重连时携带票据并发送早数据。
2. 服务端可验证票据后选择“接受或拒绝”早数据。
3. 拒绝 0-RTT 时不影响正常 1-RTT 握手与建连。
4. 保证基础抗重放能力（窗口 + 去重缓存）。

### 16.2 非目标（首版不做）

1. 不做跨节点全局防重放（仅单进程/单节点窗口防护）。
2. 不做复杂应用层语义回滚（先约束早数据只用于幂等请求）。
3. 不引入多版本兼容协商扩展。

### 16.3 协议与结构扩展建议

1. `ConnectInfo` 保持普通建连语义，不再承载 0-RTT 私有字段。
2. 新增独立 `Connect0RttInfo`，仅用于非加密 0-RTT 直连场景，包含 `session_ticket` 与可选早数据。
3. 在 Initial/Handshake 阶段携带 `SessionToken` 帧（或扩展 Crypto 附带字段），用于服务端恢复验证。
4. 保留现有 `UTP_TYPE_0RTT`，用于客户端在握手未完成时发送早数据包。
5. 配置新增 `zero_rtt_token_max_lifetime_s`（建议默认 600 秒）。

当前实现决策补充：
1. `UTP_TYPE_0RTT` 首包不拆为双包模型，先采用单包布局。
2. `SessionToken` 固定作为 payload 首帧，保持明文可解析。
3. `SessionToken` 后的应用数据帧进入早期 AEAD 保护区，不再按明文 frame 链直接解析。
4. 接收端处理顺序固定为“先解析 SessionToken -> 验票并恢复早期密钥 -> 再解密后续应用数据帧”。
5. SessionResumptionState 对外暴露为 opaque 字符串，使用标准 Base64（非 URL-safe）。
6. SDK 支持 `Context` 级别设置会话恢复密钥；若用户未设置则使用 SDK 默认密钥。
7. SDK 提供 `exportSessionToken(...)`，供非加密 `connect0Rtt(...)` 直接复用票据。
8. SDK 不提供对外解析 API，应用不感知封装内部字段。
9. SDK 不做持久化；收到“SessionTokenReady”通知后由用户主动调用导出接口并自行存储。

### 16.4 客户端状态机（建议）

1. `connect0Rtt(ticket)`：
  - 发送 `UTP_TYPE_0RTT` 首包，payload 首帧为 `SessionToken`
  - 该入口仅用于非加密 0-RTT；加密恢复统一走 `connect0RttWithState(...)`
  - 若存在早数据，则放在 `SessionToken` 之后发送
2. 收到服务端“接受 0-RTT”信号：
  - 早数据继续按正常 ACK/重传轨道推进
3. 收到“拒绝 0-RTT”或握手参数不匹配：
  - 丢弃未确认早数据并上报可重试提示
  - 回退到 1-RTT 正常发送路径

### 16.5 服务端状态机（建议）

1. 收到 `UTP_TYPE_0RTT` 首包：
  - 首先只解析 payload 第一帧 `SessionToken`
  - 调用 `TokenAuth::open` 验证有效期、来源、上下文绑定
2. 验证通过：
  - 恢复票据中的加密模式与早期密钥材料
  - 初始化早期 AEAD 上下文后，再解密 `SessionToken` 之后的应用数据帧
3. 验证失败：
  - 标记拒绝 0-RTT，仅保留后续正常握手或直接拒绝连接
  - 不允许将 `SessionToken` 之后的帧按明文继续解析

### 16.6 抗重放最小方案

1. 票据中加入 `issue_time + nonce + client_ip_prefix(optional)`。
2. 服务端维护短时窗口去重表：`(ticket_id, client_nonce)`。
3. 命中重复即拒绝 0-RTT（但不必拒绝连接本身）。

### 16.7 关键代码落点（建议）

1. API：`include/utp/utp.h` 中普通 `ConnectInfo` 与 0-RTT 专用结构拆分。
2. 连接发送：`ConnectionImpl::connect/sendInitialPacket/sendPacket` 增加 0-RTT 分支。
3. 收包解析：`ConnectionImpl::onUdpPacket` 增加 `UTP_TYPE_0RTT` 验证与分发。
4. Token：复用 `crypto/token.*`，新增恢复上下文校验接口。
5. 回调：为应用补充“0-RTT accepted/rejected”可观测事件（可选）。
6. API：新增会话恢复能力接口：
  - `setResumptionSecret(...) / clearResumptionSecret()`
  - `setOnSessionTokenReady(...)`
  - `exportSessionToken(...)`
  - `exportSessionResumptionState(...)`
  - `connect0Rtt(...)` 仅用于非加密票据直连
  - `connect0RttWithState(...)`
7. 错误语义：`connect0RttWithState` 返回前置校验错误码（如 Base64 解码失败、AEAD 解密失败、本地过期等）；服务端拒绝通过异步失败回调上报。

### 16.8 分阶段实施计划

1. Phase-1：打通票据携带与验证，先不发早数据。
2. Phase-2：接入 `UTP_TYPE_0RTT` 早数据发送/接收，支持拒绝回退。
3. Phase-3：补齐抗重放统计、回归测试与压测。

### 16.9 测试建议

1. 正常重连：票据有效，0-RTT 被接受，数据可达。
2. 票据过期：0-RTT 被拒绝但连接可建立。
3. 重放请求：命中去重窗口，0-RTT 被拒绝。
4. 参数不一致：拒绝 0-RTT 并回退 1-RTT。
5. 丢包场景：0-RTT 包重传与握手并发不互相破坏。
6. 状态串前置校验：标准 Base64 非法、AEAD 解密失败、expires_at 本地过期应返回不同错误码。
7. 服务端拒绝路径：前置校验通过但服务端拒绝时，通过异步回调返回拒绝原因。