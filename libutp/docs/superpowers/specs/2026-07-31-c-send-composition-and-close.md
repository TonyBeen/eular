# C 发送合包、瞬时帧与关闭语义决策

- 日期: 2026-07-31
- 状态: 已确认，C 实现必须遵守
- 范围: `c/` 的 PacketOut、connection 发送调度、重传、ACK piggyback 与连接关闭

本文记录 C 实现阶段已确认的设计决定。`docs/superpowers/requirements/` 仍是功能需求基线；本文对 C
发送路径的实现取舍优先，尤其在旧 `doc/` 或 cpp 当前行为不一致时。

## 1. CONNECTION_CLOSE：严格 immediate close

`CONNECTION_CLOSE` 是终止连接的状态机屏障，不承担可靠交付业务数据的职责；业务完整性由应用层在发送
connection close 前自行保证。

- `CONNECTION_CLOSE` **MUST** 单独成包，不与 ACK、STREAM 或任何其他 frame 合包。
- 在 close 被 queue 后，本端立即进入 closing，**MUST NOT** 再发送 ACK-only、STREAM、流控、握手、
  PING 或其他普通 control 包。
- 已 unacked 的业务包可保留到连接销毁时统一回收，或用于现有统计收敛；**MUST NOT** 进入丢包重传。
- closing 状态收到包时，不处理其中业务 frame；允许受限地重发单独的 `CONNECTION_CLOSE`。
- 收到对端 `CONNECTION_CLOSE` 后进入 draining；draining 状态 **MUST NOT** 再发送任何包。
- closing/draining 的状态与 CID 解复用信息至少保留 `3 * PTO`，期限到达后才销毁连接。

发送端的单独成包约束是本端行为，接收端 **MUST** 对对端错误共包的 `CONNECTION_CLOSE` 保持兼容：不得因
close 与其他合法 frame 同包而直接作为格式错误拒绝。该包内其他 frame 仍按正常接收流程处理，close 的状态
转换在本包处理完成后生效；之后的包遵循 closing/draining 规则。本端绝不产生这种共包格式。

## 2. Transient frame 与重传

首发包可以 piggyback transient frame 与可靠 frame。丢包后只重传可靠内容，不重传首发时刻的 ACK。

```text
首次发送: [ACK][PING][PADDING][reliable control][STREAM]
丢失重传:                   [reliable control][STREAM]
```

- transient 集合固定为 `ACK`、`PING`、`PADDING`。
- 纯 transient 包不进入可靠重传队列；丢失后直接释放。
- 含至少一个可重传 frame 的包进入 unacked/loss 记账。
- 重传时 **MUST NOT** 生成新的 ACK；新的 ACK 仅由正常 ACK scheduler 根据当时 receive history 生成。
- `CONNECTION_CLOSE` 有独立关闭状态机，不属于普通合包重传。
- 其他 frame 的可重传性以 cpp 的 `UTP_FRAME_RETX_MASK` 为参考，并在 C 中由单一 frame policy 定义。

### 2.1 无复制 transient strip

cpp 现有 `PacketEditor::StripTransientAckPayload()` 使用 payload 内存搬移。C 为保持 PacketOut scatter/gather
与 STREAM 外部 buffer 的零拷贝，采用逻辑剥离：

- 本端 builder **MUST** 将所有 transient frame 放在 payload 前缀；这是发送端布局约束，不是接收端线协议
  限制。
- 重传时重写 header payload length，删除 transient metadata/frame bits，并将 raw slice 起点越过该前缀。
- external STREAM slice 的指针和长度保持不变，**MUST NOT** 因 transient strip 移动或复制。
- 若未来启用加密，剥离后废弃旧密文，使用新的 packet number 与新的 payload 重新加密。

ACK scheduler 仅在携带 ACK 的 UDP 包实际发送成功时才能清除 pending；若包在发送前失败、被取消或被 close
屏障丢弃，ACK 必须继续 pending 或重新置 pending。

## 3. ACK + single STREAM 合包

第一阶段就支持 ACK 与单个 STREAM frame 合包，不仅限于 ACK 与小 control frame：

```text
[transient prefix][reliable control][single STREAM header][single STREAM external data]
```

- 每个 PacketOut 暂时最多一个 STREAM frame，复用现有 `stream_id`、`stream_offset`、`stream_data_size`
  字段，不提前扩展为多 stream packet。
- STREAM data 保持 external slice；raw buffer 仅保存包头、transient/control frame 和 STREAM header。
- ACK 或 control 已待发时，先扣除它们的空间再计算 STREAM fragment 长度，尽量填满当前路径 MTU。
- 无法同时容纳 STREAM header 时，才发送 ACK-only 或 control-only packet。
- STREAM metadata 只描述 raw header；external data 的长度由 `stream_data_size` 表示，不能按 raw buffer 区间
  校验。

## 4. 按 packet class 的 frame priority

优先级不能突破 frame 的包类型/握手阶段合法性。先按 packet class 隔离，再在 class 内合包：

```text
A. CONNECTION_CLOSE: 独占包，状态机屏障
B. INITIAL / HANDSHAKE: 只容纳握手阶段合法的 frame
C. established CTRL: ACK、路径、流控、STREAM 等
```

established `CTRL` 的优先级从高到低为：

```text
P0 ACK                 (仅作为 transient prefix)
P1 PATH_RESPONSE
P2 ACK_FREQUENCY
P3 RESET_STREAM
P4 MAX_DATA / MAX_STREAM_DATA
P5 DATA_BLOCKED / STREAM_DATA_BLOCKED
P6 STREAM
P7 PING / PADDING       (transient 或填充)
```

- builder 按优先级贪心合包，受当前路径 MTU 与 `UTP_PACKET_OUT_MAX_FRAMES` 双重上限限制；同优先级 FIFO。
- `MAX_DATA` 只保留最大待发送值；`MAX_STREAM_DATA` 按 stream id 保留最大待发送值。
- 同一 stream 的 `RESET_STREAM` 只保留一个待发送项；blocked 通知按 `(type, stream id)` 合并，避免发送
  过时重复通知。
- 丢失的小 control 回到保存语义字段的 pending queue，和新的 control 再次合包；不复制旧 frame bytes。
- 丢失的 STREAM 保持 PacketOut/external slice 的零拷贝重传，优先于新 STREAM，但低于 P0-P5 control。

对单调状态 frame，PacketOut 的已编码 bytes 只用于首发，**不是**重传来源：

```text
MAX_DATA(200) 丢失，期间 desired 已更新为 300
=> 丢弃旧 packet 的 MAX_DATA bytes
=> 重新读取 semantic slot
=> 下一次构造 MAX_DATA(300)
```

PacketOut 为每个这类 frame 保存稳定 slot 引用与 generation 快照，供 ACK/丢失处理判断。若已有携带更大值的
queued 或 unacked packet，旧值丢失无需再次入队；否则标记 slot pending。旧值 ACK **MUST NOT** 回退当前
advertised/pending 状态。

此规则适用于 `MAX_DATA`、`MAX_STREAM_DATA` 和当前值可覆盖旧值的 blocked 通知；`RESET_STREAM` 不是单调
状态，重传时必须保留首次确定的 `{stream_id, error_code, final_size}`。STREAM 也必须保留原始 offset/data
range 零拷贝重传。

## 5. Control pending 的语义槽位与提交点

control pending 不使用通用 frame FIFO，而是使用连接内固定容量的语义槽位；不增加 public config，容量由
`UTP_CONNECTION_MAX_STREAMS` 和当前支持的 frame 类型推导。

```text
1 个 MAX_DATA
每 stream 1 个 MAX_STREAM_DATA
1 个 DATA_BLOCKED
每 stream 1 个 STREAM_DATA_BLOCKED
每 stream 1 个 RESET_STREAM
ACK 由 ack_scheduler 持有，不占 control slot
```

- slot 保存语义字段（例如 `stream_id`、`maximum_data`、`error_code`、`final_size`），builder 在选包时才
  编码 wire bytes。
- `MAX_DATA`/`MAX_STREAM_DATA` 保存最大待发送值；blocked 保存最新限制值；同 stream 的 reset 只保留一个
  终止状态。
- 小 control 丢失时，恢复对应语义 pending 项并与新 control 合包；不复制旧 packet 的 frame bytes。
- 每项状态分为 `desired`、`queued`、`sent`：仅 UDP 实际发送成功才更新 advertised 值、发送时间戳、限速
  时间戳或清除 ACK pending。
- 已编码但未写出的 PacketOut 因发送失败、关闭屏障或队列取消而释放时，相关语义项必须恢复为 pending。

该模型消除过时窗口更新的重复发送，保证 close/UDP 失败时不会提前认为状态已通告，并保持 established
发送路径无运行期分配。

## 6. STREAM 选择与公开优先级 API

P0-P5 control 始终先于 P6 STREAM；只有要生成 STREAM 时才调用 stream scheduler。C 不再按
`streams[]` 的数组下标直接选择，否则低下标持续可写时会饿死其他流。

公开 API 只提供两种 scheduler mode：

```c
typedef enum utp_stream_scheduler_mode {
    UTP_STREAM_SCHEDULER_STRICT = 0,
    UTP_STREAM_SCHEDULER_DRR = 1
} utp_stream_scheduler_mode_t;
```

- `utp_context_options_t` 增加 `stream_scheduler_mode`；零初始化和缺省值均为 `STRICT`。该值在 Context
  创建时固定，所有连接继承同一值；不支持 connection 级覆盖，与 cpp 的 `ContextImpl::m_config` 模型一致。
- 增加 `utp_stream_set_priority(utp_stream_t *, uint8_t)` 与 `utp_stream_priority(const utp_stream_t *)`。
  priority 有效范围为 `0..7`，`0` 最高；默认 priority 为 `4`。
- 不提供 `Disabled`，也不提供运行期切换 scheduler mode 的额外 API；mode 在 Context 创建时固定。

`STRICT` 选择有效 priority 最小的可发 stream；同优先级 round-robin。为避免持续高优先级负载饿死低优先级
stream，等待每满 8 轮将有效 priority 提升一级，最低提升到 0；被选中或不再有发送工作时清零等待轮次。

`DRR` 以 stream id round-robin，权重为 `8 - priority`，基准 quantum 为 1200 bytes，单流 deficit 上限
128 KiB。仅在 deficit 足以发送当前 fragment 时选择该 stream，并在发送后扣减实际 STREAM data bytes。

## 7. HandshakeDone 的 C 特例

主动端发送 `HandshakeDone` 不以 ACK 为连接成功判据，也不单独标记为 pending 重传。若被动端未收到，
被动端会重发 `Handshake`，主动端据此再次发送 `HandshakeDone`；主动端后续业务数据仍走正常可靠 pending。
此条覆盖旧文档中“客户端 HandshakeDone 必须 pending 至 ACK”的描述。

## 8. 合包时机

本阶段只做即时合包：每次进入发送路径时，将当前已 pending 的 frame 按本文件优先级合入 packet，并以一个
STREAM fragment 填充剩余 MTU。**MUST NOT** 为等待未来应用数据或 control frame 而主动延迟当前 packet。

延迟 coalescing 依赖调用者可见的 `next_send_deadline`、配置项和 timer 仲裁，待 Context timeout API 与配置模型
完整后再引入。届时 ACK/PTO/close deadline 必须优先于 coalesce deadline，close barrier 必须取消 coalesce
timer 与未封包业务数据。

## 9. 优先级与拥塞控制

frame priority 仅决定包构造时的选帧和合包顺序，**不**赋予普通可靠 frame 绕过 cwnd 或 pacer 的权限。

- `ACK-only` 不计入 in-flight，不等待普通 cwnd 准入，必须能满足 ACK deadline。
- `CONNECTION_CLOSE` 使用独立 close/PTO 状态机，不受普通业务队列阻塞。
- `PATH_RESPONSE` 受路径验证与反放大额度约束，不走普通 STREAM 的拥塞准入。
- `RESET_STREAM`、`MAX_*`、`BLOCKED`、`ACK_FREQUENCY` 和 `STREAM` 均受 cwnd/pacer 约束；窗口不足时保持
  pending，等待后续发送预算。
- 丢失的 STREAM 优先于新 STREAM，但仍不绕过拥塞控制。

### 9.1 本地 UDP 发送错误

`ENOBUFS`（Windows 的 `WSAENOBUFS`）是本地永久发送错误，**MUST NOT** 按 `EAGAIN`/`EWOULDBLOCK`
重排队或等待可写：该 PacketOut 从未发送，必须释放，连接不发送 `CONNECTION_CLOSE`，并立即终止。主动
握手尚未完成时通过 `on_connect_error(UTP_STATUS_LIMIT)` 报告；其他连接通过
`on_connection_error(status=UTP_STATUS_LIMIT, peer_initiated=false)` 报告。原因字符串为 `udp send ENOBUFS`。

`EAGAIN`/`EWOULDBLOCK` 的语义不同：PacketOut 保持未发送并回到发送队首，等待后续可写调度。

## 10. close PTO 与回收

关闭遵循 QUIC 的响应式 close 重发，而不是依赖双方完成两次挥手：

- 本地 queue close 后立即停止普通发送；首次 `CONNECTION_CLOSE` 成功写入 UDP 后，进入 closing，并设置
  `close_deadline = sent_time + 3 * close_pto`。
- close 尚未成功写入 UDP 时 **MUST NOT** 开始上述 deadline；socket 暂时不可写时保留单独的 close packet
  并等待可写。
- closing 期间收到任意对端包时，不处理业务 frame；仅当距上次 close 发送已过一个 `close_pto` 才限速重发
  一个单独的 `CONNECTION_CLOSE`。
- closing 期间无入包时 **MUST NOT** 盲目按 PTO 周期重发 close。
- 收到有效对端 `CONNECTION_CLOSE` 后立即进入 draining，不回 ACK、不回 close、不再发送任何包；
  `drain_deadline = receive_time + 3 * close_pto`。
- deadline 到达后释放连接状态与 CID 解复用项。

`close_pto` 由 RTT 派生；未知 RTT 使用 `333333us`，并 clamp 到 `[10ms, 60s]`。
