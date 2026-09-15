# ACK 与重传实现细节

## 1. 发送记账

`utp_send_control_t` 为每个 Connection 保存未确认 PacketOut、飞行字节、发送时间、ACK 包号和丢失状态。一个逻辑包可以关联多个发送尝试，但每次重传都产生新的包号。

## 2. ACK

接收历史记录 ack-eliciting 包号并按 AckFrequency 触发 ACK。触发条件包括累计阈值、乱序和延迟 ACK 到期；ACK 可与其他控制或 STREAM 帧合包。

发送侧收到 ACK 后：

1. 回收已确认 PacketOut；
2. 更新 RTT、带宽采样和拥塞控制；
3. 根据包号重排序和发送时间识别丢失；
4. 推进流和控制帧的确认状态。

## 3. 定时器和重传

Context 定时器驱动 ACK、PTO、握手和控制帧重试。`WOULD_BLOCK` 由统一 writable 事件继续发送，不能为每种帧单独注册写事件。丢失 PacketOut 进入重传队列，发送时重新构造包头和加密 payload。

## 4. 边界

ACK 丢失不直接导致连接关闭；只有重传预算/PTO 或连接保活策略判定路径不可用时，连接才进入错误收敛。MTU 探测包独立记账，`EMSGSIZE` 只收窄 MTU 探测上界。
