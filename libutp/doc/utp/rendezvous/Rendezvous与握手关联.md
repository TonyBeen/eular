# Rendezvous、attempt_id 与握手关联

## 1. 连接尝试

每次 `utp_context_connect()` 或 `utp_context_connect_0rtt()` 都生成随机 16 字节 `attempt_id` 和本地 CID，并将目标 peer-id 写入 Rendezvous REQUEST。普通直连也使用同一连接尝试模型；只是没有 NTRS 控制面时，候选地址直接来自调用参数。

## 2. NTRS 路径

REQUEST -> REDIRECT -> FORWARD -> PUNCH 是协调顺序。REDIRECT 和 FORWARD 只提供候选及 token，不承载业务数据。收到合法 PUNCH 后，主动端向触发它的 endpoint 重发保留的 Initial，并以该 PUNCH 的本地目的地址作为 UDP 源地址；之后按普通 Initial/Handshake/HandshakeDone 处理。

握手阶段使用 `attempt_id`、本地 active CID 和候选 endpoint 验证来源；同一 Context 可以并发向多个 peer 建连。连接成功后，数据面主要按 DCID/SCID 解复用，attempt_id 仅用于握手完成、0-RTT 和 Rendezvous 短期清理。

## 3. Endpoint 规则

收到哪个 endpoint 的控制包，就向该 endpoint 回包；候选列表仅用于尚未获得路径反馈时的 fan-out。完整 endpoint 比较包含地址族、地址、端口和 IPv6 scope；不同 endpoint 的迟到 Handshake 必须通过 CID、包号和加密校验后才能被接受。

## 4. 清理

握手超时、显式取消、连接成功或对端关闭后，Context 清理对应 attempt 索引和候选状态。已建立 Connection 不因旧 attempt_id 的迟到协调包重新创建 pending。
