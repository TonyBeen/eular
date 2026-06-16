# STUN 双机最小部署指南（M1）

适用场景：
- 你已经有两台公网服务器。
- 目标是先跑通双 Node 控制链路、完整异步 full STUN 探测以及基本 UDP hole punch。

## 1. 拓扑建议

- Node-A：STUN-Control（TCP） + STUN 主端口 `3478/udp` + 备用端口 `3479/udp`
- Node-B：STUN-Control（TCP） + STUN 主端口 `3478/udp` + 备用端口 `3479/udp`

推荐端口：
- STUN-Control: 19000/tcp
- STUN primary: 3478/udp
- STUN alternate port: 3479/udp

## 2. 防火墙与安全组

两台机器都需要放行：
- `19000/tcp`
- `3478/udp`
- `3479/udp`

若云厂商有安全组和主机防火墙，两个层面都要放行。

## 3. 构建

在项目根目录执行：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build -j
```

会生成：
- `build/bin/stun_hub`
- `build/bin/stun_node`
- `build/bin/stun_peer`
- `build/bin/stun_nat_detect`

## 4. 服务启动

### 4.1 启动 Hub

当前 `stun_hub` 使用 MQTT 作为 Hub 控制面总线。Node 启动时只需要知道这个 Hub/MQTT endpoint，不需要知道其他 Node。

```bash
./build/bin/stun_hub <Hub域名或IP> 1883
```

如果 MQTT broker 与 Hub 进程不在同一台机器，传 MQTT broker 的地址和端口。

### 4.2 启动服务节点（Node-A）

```bash
./build/bin/stun_node \
  --hub <Hub域名或IP>:1883 \
  --node-id node-a \
  --public-host <NodeA公网IP或域名> \
  --control-port 19000 \
  --stun-port 3478
```

### 4.3 启动服务节点（Node-B）

```bash
./build/bin/stun_node \
  --hub <Hub域名或IP>:1883 \
  --node-id node-b \
  --public-host <NodeB公网IP或域名> \
  --control-port 19000 \
  --stun-port 3478
```

Node 行为：
- 本地绑定 `19000/tcp` 作为 STUN control。
- 本地绑定 `3478/udp` 作为 STUN 主端口。
- 本地绑定 `3479/udp` 作为 `change-port` / same-IP diff-port 响应端口。
- 向 Hub 注册 `<public-host>:19000` 与 `<public-host>:3478`。
- 通过 Hub 下发的 assignment 发现其他 Node，不再需要启动参数里手工配置对端 Node。

### 4.4 STUN 说明

当前 `stun_node` 已内置完整异步 full STUN 所需的基础能力。

含义：
- 启动 `stun_node` 后，同进程会监听本地：
  - `0.0.0.0:3478/udp` 作为主 STUN 端口
  - `0.0.0.0:3479/udp` 作为备用端口
- 普通 Binding Response 会返回 `XOR-MAPPED-ADDRESS`、`RESPONSE-ORIGIN`、`OTHER-ADDRESS`。
- `CHANGE-REQUEST(change-port)` 由本节点 `3479/udp` 回应。
- `CHANGE-REQUEST(change-ip)` 由协同 Node 的 `3478/udp` 代发回应。
- 不再强制依赖外部 `turnserver/coturn`。

## 5. Peer 联调

### 5.0 仅 NAT 探测（Peer 只需一个入口）

```bash
./build/bin/stun_nat_detect <NodeA_NTRS_IP> 19000
./build/bin/stun_nat_detect <NodeA_NTRS_IP> 19000 -v
```

说明：
- `stun_nat_detect` 会先向 Node-A 请求探测端点。
- Node-A 会与 Node-B 协同，返回双 STUN 端点（若对端不可达则降级为单 STUN）。
- 探测顺序为：
  - `stun1`
  - `change-port`
  - `change-ip`
  - `stun2`
- 支持：
  - `--bind-ip <ip>`
  - `--bind-device <ifname>`

### 5.1 被叫端（Peer-B）先上线

```bash
./build/bin/stun_peer <NodeA_IP> 19000 peer_b dev_b -
./build/bin/stun_peer -v <NodeA_IP> 19000 peer_b dev_b -
```

### 5.2 主叫端（Peer-A）发起会话

```bash
./build/bin/stun_peer <NodeA_IP> 19000 peer_a dev_a peer_b
./build/bin/stun_peer -v <NodeA_IP> 19000 peer_a dev_a peer_b
```

`stun_peer` 同样支持：

```bash
--bind-ip <ip>
--bind-device <ifname>
```

这会让 NAT 探测和后续 hole punch 复用同一个本地出口。

客户端会输出：
- `local / srflx / srflx2`
- `class / flags / mapping / filtering / risk`
- 每个 STUN 的 `rtt_ms`
- 每个 STUN 的 `success/rounds` 与 `distinct mappings`
- NAT 分类：
  - `open_public`
  - `full_cone_nat`
  - `ip_restricted_nat`
  - `port_restricted_nat`
  - `symmetric_nat`
- `-v` 下会额外打印 candidate 列表，例如：
  - `host_local`
  - `srflx_primary`
  - `srflx_secondary`（仅当与 primary 不同才保留）
- 最终打通后会打印选中的 candidate 类型。

## 6. 参数建议（M1）

- STUN 超时：1000ms
- STUN 重试：3 次（当前示例已内置）
- 采样轮数：3 轮（当前示例已内置）
- lease_sec：30
- heartbeat_interval_sec：10
- session 过期：60

当前示例代码具备：
- Peer 退出时主动发送 `UNREGISTER_REQ`
- 服务端按 `lease_sec` 自动剔除超时 peer
- 客户端默认执行完整异步 full STUN 探测
- 会话时交换 `local_ip:local_port`
- 同机或同局域网场景下会优先命中 `host_local`

## 7. 你现在最该做的三件事

1. 先把完整异步 full STUN 探测跑通，确认 `stun1 / change-port / change-ip / stun2` 都正常。
2. 观察 `class / mapping / filtering / flags`，验证 NAT 分类是否符合预期。
3. 再验证同机/同局域网场景是否优先选择 `host_local`。

## 8. 常见问题

1. 两台节点是否必须都跑 STUN-Control？
- Hub 模式下建议每个 Node 都跑 STUN-Control 和 STUN。Hub 根据在线节点给每个 Node 下发协作对象。

2. 两台节点是否能显著提升判断能力？
- 是。双 Node 既能提供双 STUN 视角，又能协同完成 `change-ip` 回应。

3. 没有第二台 STUN 可以先上线吗？
- 可以。功能可用，但 NAT 风险判断精度会下降。

## 9. M1 生命周期验证

1. 正常下线验证
- 启动 Peer 后等待心跳结束，Peer 会发送 `UNREGISTER_REQ`。
- 节点日志应出现 `UNREGISTER peer=...`。

2. 异常断线验证
- 强制 kill Peer 进程（不走正常退出）。
- 在约 30 秒后，节点应出现 `peer lease expired: ...`。
