# 路径验证与 MTU 实现细节

## 1. 路径验证

Connection 维护 active path 和 candidate path。来自新 endpoint 的有效包触发 PATH_CHALLENGE，只有匹配 PATH_RESPONSE 后候选路径才晋升为 active。响应优先发回触发当前报文的来源 endpoint。

未验证路径按候选路径单独记录已认证接收字节、已实际发送字节和已排队字节。可发送额度按以下规则计算：

`limit = 3 * candidate_rx_bytes + 3 * UTP_PACKET_MTU_FLOOR`

发送前同时扣除已有 `candidate_tx_bytes` 和 `candidate_queued_bytes`。因此候选路径只有在收到足够的有效数据后才会获得更多发送额度；候选失败不会关闭仍可用的 active path。

## 2. MTU 探测

`utp/src/mtu/mtu.c` 只负责状态机；Connection 负责构造 Ping + Padding 探测包并把 ACK、丢失、超时事件回灌给状态机。

- 握手期间使用 MTU floor。
- 连接建立后按梯队探测，再在上下界之间二分收敛。
- 探测包不与业务 STREAM 包共用重传语义。
- 只有带 `UTP_PO_MTU_PROBE` 标志的探测包将 `EMSGSIZE`/`WSAEMSGSIZE`（以及本地 overflow 映射）交给 `utp_mtu_discovery_on_probe_send_failed()`，收窄探测上界并释放当前探测包。普通业务包的发送错误仍按统一 socket 错误处理，不能笼统视为 MTU 探测反馈。
- 黑洞检测触发安全 MTU 回退和冷却后重新探测。

## 3. Linux PMTU 缓存与拨号链路参考

Linux IPv4 UDP socket 使用 `IP_MTU_DISCOVER = IP_PMTUDISC_DO`，以禁止 IP 分片并让本地发送过大的
数据报返回 `EMSGSIZE`。这使 DPLPMTUD 可以用真实报文探测路径 MTU，但内核会按目的和选路源地址维护
PMTU 缓存。

一次拨号链路排障中，实际路径 MTU 为 1492。探测阶梯已依次覆盖 1400、1450、1492 和 1500：1492
可达，1500 探测触发 ICMP `fragmentation needed`。该 ICMP 没有提供有效的 next-hop MTU，Linux 随后将
此路径的 PMTU 缓存降至 `net.ipv4.route.min_pmtu` 的默认值 552。于是后续即使是 740 字节的普通 UDP
负载也会在本地返回 `EMSGSIZE`；这不是 UTP 计算出的路径 MTU，也不是网卡的 1500 MTU。

可用下列命令确认当前选路及内核缓存结果：

```sh
ip route get <peer-ip> from <local-ip>
```

处理边界如下：

- 带 `UTP_PO_MTU_PROBE` 的探测包遇到 `EMSGSIZE`，仅作为探测失败反馈给 MTU 状态机，不向应用报告。
- 普通业务包遇到 `EMSGSIZE`，说明当前系统已无法承载协议承诺的最小路径 MTU；Connection 必须以
  `on_connection_error` 终止并向应用报告，不能无限重试。
- `nat_punch --disable-mtu-probe` 仅用于排障和环境对照，不是默认运行策略。1492 拨号路径应保持探测开启，
  让协议以 1492 工作，而不是永久固定为 1400。
