# NTRS 双机最小部署指南（M1）

适用场景：
- 你已经有两台公网服务器。
- 目标是先跑通 M1 控制链路，并具备双 STUN 探测能力。

## 1. 拓扑建议

- Server-A：NTRS-Control（TCP） + STUN-1（UDP）
- Server-B：STUN-2（UDP），后续可扩展为 Relay

推荐端口：
- NTRS-Control: 19000/tcp
- STUN-1: 3478/udp
- STUN-2: 3478/udp

## 2. 防火墙与安全组

两台机器都需要放行：
- `19000/tcp`（仅 Server-A 必需）
- `3478/udp`

若云厂商有安全组和主机防火墙，两个层面都要放行。

## 3. 构建

在项目根目录执行：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build -j
```

会生成：
- `build/examples/ntrs_server`
- `build/examples/ntrs_client`

## 4. 服务启动

### 4.1 启动 NTRS-Control（Server-A）

```bash
./build/bin/ntrs_server 19000 <ServerA_STUN_IP>:3478 <ServerB_NTRS_IP>:19000
```

### 4.2 启动 NTRS-Control（Server-B）

```bash
./build/bin/ntrs_server 19000 <ServerB_STUN_IP>:3478 <ServerA_NTRS_IP>:19000
```

### 4.3 STUN 说明

当前 `ntrs_server` 已内置 STUN Binding Response 能力。

含义：
- 启动 `ntrs_server` 后，同进程会监听你配置的 `self_stun_host:port` 中的端口（通常 3478/udp）。
- 不再强制依赖外部 `turnserver/coturn`。

## 5. 客户端联调

### 5.0 仅 NAT 探测（客户端只需一个入口）

```bash
./build/bin/ntrs_client <ServerA_NTRS_IP> 19000
```

说明：
- `ntrs_client` 会先向 Server-A 请求探测端点。
- Server-A 会与 Server-B 通信，返回双 STUN 端点（若对端不可达则降级为单 STUN）。

### 5.1 被叫端（Client-B）先上线

```bash
./build/bin/ntrs_client <ServerA_IP> 19000 peer_b dev_b -
```

### 5.2 主叫端（Client-A）发起会话

```bash
./build/bin/ntrs_client <ServerA_IP> 19000 peer_a dev_a peer_b
```

客户端会输出：
- 两次 NAT 探测结果（#1/#2）
- `stable=true/false`
- `risk=low/high`
- 每个 STUN 的 `ok` 与 `rtt_ms`
- 每个 STUN 的 `success/rounds` 与 `distinct mappings`
- NAT 分类 `nat_type`（`cone_like` / `symmetric_like` / `single_stun_limited` 等）
- 精细 NAT 分类（双节点可用）：
	- `full_cone_nat`
	- `ip_restricted_nat`
	- `port_restricted_nat`
	- `symmetric_nat`
	- 以及弱网/降级状态（如 `single_stun_limited`）

## 6. 参数建议（M1）

- STUN 超时：1000ms
- STUN 重试：3 次（当前示例已内置）
- 采样轮数：3 轮（当前示例已内置）
- lease_sec：30
- heartbeat_interval_sec：10
- session 过期：60

当前示例代码新增：
- 客户端退出时主动发送 `UNREGISTER_REQ`
- 服务端按 `lease_sec` 自动剔除超时 peer
- 客户端会触发过滤行为探测（同 IP 异端口、异 IP 探测包）用于精细 NAT 分类

## 7. 你现在最该做的三件事

1. 先把双 STUN 探测跑通，确认两台服务器都可返回 Binding Response。
2. 观察 `stable` 与 `risk`，验证双服务器是否能识别映射不稳定场景。
3. 再进入 M1 硬化：服务端 lease 超时淘汰 + unregister + 基础鉴权校验。

## 8. 常见问题

1. 两台服务器是否必须都跑 NTRS-Control？
- M1 不必须。先单控制节点 + 双 STUN 即可。

2. 两台服务器是否能显著提升判断能力？
- 是。双 STUN 能做映射一致性对照，能更早识别高风险 NAT。

3. 没有第二台 STUN 可以先上线吗？
- 可以。功能可用，但 NAT 风险判断精度会下降。

## 9. M1 生命周期验证

1. 正常下线验证
- 启动客户端后等待心跳结束，客户端会发送 `UNREGISTER_REQ`。
- 服务端日志应出现 `UNREGISTER peer=...`。

2. 异常断线验证
- 强制 kill 客户端进程（不走正常退出）。
- 在约 30 秒后，服务端应出现 `peer lease expired: ...`。
