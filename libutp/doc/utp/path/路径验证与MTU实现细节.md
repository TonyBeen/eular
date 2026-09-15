# 路径验证与 MTU 实现细节

## 1. 路径验证

Connection 维护 active path 和 candidate path。来自新 endpoint 的有效包触发 PATH_CHALLENGE，只有匹配 PATH_RESPONSE 后候选路径才晋升为 active。响应优先发回触发当前报文的来源 endpoint。

未验证路径按候选路径单独记录已认证接收字节、已实际发送字节和已排队字节。可发送额度按以下规则计算：

`limit = 3 * candidate_rx_bytes + 3 * UTP_PACKET_MTU_FLOOR`

发送前同时扣除已有 `candidate_tx_bytes` 和 `candidate_queued_bytes`。因此候选路径只有在收到足够的有效数据后才会获得更多发送额度；候选失败不会关闭仍可用的 active path。

## 2. MTU 探测

`c/src/mtu/mtu.c` 只负责状态机；Connection 负责构造 Ping + Padding 探测包并把 ACK、丢失、超时事件回灌给状态机。

- 握手期间使用 MTU floor。
- 连接建立后按梯队探测，再在上下界之间二分收敛。
- 探测包不与业务 STREAM 包共用重传语义。
- 只有带 `UTP_PO_MTU_PROBE` 标志的探测包将 `EMSGSIZE`/`WSAEMSGSIZE`（以及本地 overflow 映射）交给 `utp_mtu_discovery_on_probe_send_failed()`，收窄探测上界并释放当前探测包。普通业务包的发送错误仍按统一 socket 错误处理，不能笼统视为 MTU 探测反馈。
- 黑洞检测触发安全 MTU 回退和冷却后重新探测。
