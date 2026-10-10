# ACK 与重传实现细节

## 1. 发送记账

`utp_send_control_t` 为每个 Connection 保存未确认 PacketOut、飞行字节、发送时间、ACK 包号和丢失状态。一个逻辑包可以关联多个发送尝试，但每次重传都产生新的包号。

### 发送尝试与瞬态帧

首次组包可按此前缀顺序组合瞬态和可靠内容：

```text
[ACK][PING][PADDING][可靠控制帧][单个 STREAM 帧]
```

`ACK`、`PING`、`PADDING` 是瞬态帧。仅包含瞬态帧的 PacketOut 不进入可靠重传队列；含有可重传控制帧或 STREAM 的 PacketOut 才进入 unacked 和记账。丢失后，发送控制器剥离旧的瞬态前缀、保留可靠帧的语义或 STREAM range，并用新包号重新组包。新的 ACK 只由当时的接收历史生成，不能从旧 PacketOut 复制。

ACK generation 在携带它的 PacketOut 成功进入发送队列时消费，而不是等待 UDP 实际写入。若随后收到新包，ACK scheduler 会创建新的 generation；`WOULD_BLOCK` 重排原 PacketOut 时不能把新 generation 一并清除。

### 可靠控制的语义重传

`MAX_DATA`、`MAX_STREAM_DATA`、`MAX_STREAMS` 和各类 `*_BLOCKED` 不以旧的 wire bytes 作为重传来源。发送控制保存它们的语义值、generation 与 pending 状态：单调窗口只保留最大值，blocked 只保留最新限制值，双向/单向 `MAX_STREAMS` 分开合并。旧包丢失时，若不存在携带更新值的 queued 或 unacked 包，才把对应语义槽重新标记 pending。

`RESET_STREAM` 保存首次确定的 `{stream_id, error_code, final_size}`，不能被后续状态覆盖；STREAM 保存原始 offset/data range。一个已编码但尚未写出的 PacketOut 被取消、关闭屏障丢弃或遭遇致命本地发送错误时，相关控制语义必须恢复为 pending。

## 2. ACK

接收历史记录 ack-eliciting 包号并按 AckFrequency 触发 ACK。触发条件包括累计阈值、乱序和延迟 ACK 到期；ACK 可与其他控制或 STREAM 帧合包。

握手包最多携带一个 ACK 帧；多个 ACK range 应放在同一个 ACK 帧内。收到多个 ACK 帧的异常握手不会回协议错误包，Connection 直接进入 draining，避免对异常输入继续响应。

发送侧收到 ACK 后：

1. 回收已确认 PacketOut；
2. 更新 RTT、带宽采样和拥塞控制；
3. 根据包号重排序和发送时间识别丢失；
4. 推进流和控制帧的确认状态。

## 3. 定时器和重传

Context 定时器驱动 ACK、PTO、握手和控制帧重试。`WOULD_BLOCK` 由统一 writable 事件继续发送，不能为每种帧单独注册写事件。丢失 PacketOut 进入重传队列，发送时重新构造包头和加密 payload。

### 发送优先级与发送错误

在已建立连接包中，发送优先级从高到低为 ACK、PATH_RESPONSE、ACK_FREQUENCY、RESET_STREAM、窗口更新、blocked 通知、STREAM、PING/PADDING。优先级只决定选帧和合包顺序，不绕过 cwnd、pacer 或候选路径的抗放大限制；ACK-only 和关闭状态机有自己的时限处理。

`EAGAIN`/`EWOULDBLOCK` 表示 socket 暂不可写，PacketOut 保持未发送并由 Context 的统一 writable 事件重试。`ENOBUFS`/`WSAENOBUFS` 以及普通业务包的 `EMSGSIZE` 是致命本地发送错误：不得按可写重试，也不得再发送关闭包；握手阶段报告 connect error，已建立连接报告 connection error。仅 `UTP_PO_MTU_PROBE` 探测包的 `EMSGSIZE` 交给 MTU 状态机消化。

## 4. 边界

ACK 丢失不直接导致连接关闭；只有重传预算/PTO 或连接保活策略判定路径不可用时，连接才进入错误收敛。MTU 探测包独立记账，`EMSGSIZE` 只收窄 MTU 探测上界。
