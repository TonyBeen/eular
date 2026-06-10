# NTRS Hub + STUN Node 协同架构设计

## 1. 目标

本文档用于将当前“多机器协作 STUN”架构整理为以 NTRS 为核心的两层模型：

- 数据面：每个 NTRS 实例同时作为一个 STUN Node，对外提供 STUN 能力，并与其他 STUN Node 协同完成多点探测。
- 控制面：引入一个 NTRS Hub，负责节点注册、上下线事件汇聚、服务列表快照发布，以及异常掉线治理。

目标如下：

- 让 NTRS 节点自动发现其他可协作的 STUN Node。
- 让所有节点都能感知哪些节点上线、下线、异常掉线。
- 使用 MQTT Last Will and Testament 管理异常断连。
- 将 Hub 保持在控制面，不让 Hub 进入 STUN 实时转发路径。
- 为后续代码重构提供明确的主题、消息和生命周期约束。

## 2. 设计原则

- NTRS Node 负责 STUN 探测与节点间协作。
- NTRS Hub 负责成员关系和集群视图，不参与具体探测包转发。
- 节点状态以“事件流 + 快照”双通道发布，避免仅靠瞬时消息导致新节点视图不完整。
- 节点异常退出依赖 MQTT 遗嘱兜底，正常退出走显式下线流程。
- 节点注册信息与运行期心跳分离，减少重复载荷。
- 单个节点的在线状态以稳定 node_id 标识，以 boot_id 区分进程代次，避免旧遗嘱污染新会话。

## 3. 总体架构

```text
                      +----------------------+
                      |       NTRS Hub       |
                      |  membership / view   |
                      |  snapshot / events   |
                      +----------+-----------+
                                 ^
                                 | MQTT
                                 v
     +------------------+   +------------------+   +------------------+
     |    NTRS Node A   |   |    NTRS Node B   |   |    NTRS Node C   |
     |  STUN + Control  |   |  STUN + Control  |   |  STUN + Control  |
     +--------+---------+   +--------+---------+   +--------+---------+
              \                    |                        /
               \                   |                       /
                +----------------------------------------+
                         node-to-node coordination
                      probe assist / peer validation
```

说明：

- 客户端只需要访问某一个 NTRS Node。
- 发起探测的 NTRS Node 会根据 Hub 下发的集群视图选择协作节点。
- 协作节点之间直接通信，Hub 不参与探测数据面的转发。

## 4. 角色与职责

### 4.1 NTRS Node

NTRS Node 是新的 STUN 服务主体，职责包括：

- 对外提供 STUN Binding 能力。
- 维护本地对等节点列表。
- 发起或协助多节点 NAT 探测。
- 启动后向 Hub 注册自身元数据。
- 订阅 Hub 发布的集群事件和快照。
- 周期性上报心跳、负载、健康状态。
- 设置 MQTT 遗嘱，用于异常掉线后自动广播离线状态。

### 4.2 NTRS Hub

NTRS Hub 是控制面组件，职责包括：

- 订阅所有节点的注册、状态、心跳、离线事件。
- 维护权威节点目录和最新快照。
- 将节点上线、下线、负载变化转换为标准集群事件。
- 输出一份可保留的集群快照，供新节点冷启动时快速收敛。
- 识别异常掉线与正常下线。
- 为后续调度策略提供统一入口。

### 4.3 MQTT Broker

MQTT Broker 作为事件总线，职责包括：

- 维持连接状态。
- 在节点异常断连时发布遗嘱消息。
- 持久化保留型主题，使新订阅者拿到最新快照。

## 5. 控制面与数据面边界

控制面：

- 节点注册
- 集群成员状态传播
- 服务目录快照
- 心跳与健康状态
- 调度元数据同步

数据面：

- STUN Binding Request / Response
- 多节点协作探测请求
- Node 之间的辅助探测控制消息

约束：

- Hub 不参与 STUN 包收发。
- Hub 不持有每个探测事务的临时状态。
- Hub 的故障不应阻断已知节点之间的短期协作，只影响成员视图收敛与新节点发现。

## 6. 节点标识模型

建议区分三个标识：

- node_id：节点稳定标识，对应一台逻辑 STUN 节点。
- mqtt_client_id：MQTT 连接标识，建议格式为 ntrs-node-{node_id}。
- boot_id：节点进程启动代次，每次进程重启生成新的 UUID。

其中：

- node_id 用于集群成员身份。
- mqtt_client_id 用于 MQTT 会话管理。
- boot_id 用于防止旧连接遗嘱覆盖新连接状态。

如果同一个 node_id 重连，Hub 只认可最新 boot_id 对应的在线状态；旧 boot_id 发出的离线事件必须忽略。

## 7. MQTT 主题设计

建议统一使用 ntrs 前缀。

### 7.1 节点上报主题

1. ntrs/node/{node_id}/register
用途：节点启动后上报静态元数据。

2. ntrs/node/{node_id}/presence
用途：节点在线状态主题。
特点：retain=true，正常上线和遗嘱下线都发到这个主题。

3. ntrs/node/{node_id}/heartbeat
用途：节点周期性上报运行指标。

4. ntrs/node/{node_id}/metrics
用途：可选，承载更细颗粒度负载、RTT、错误率统计。

### 7.2 Hub 下发主题

1. ntrs/hub/cluster/snapshot
用途：Hub 发布当前完整节点视图。
特点：retain=true，新节点订阅后立即获得完整列表。

2. ntrs/hub/cluster/events
用途：Hub 发布成员变化增量事件。
特点：retain=false。

3. ntrs/hub/node/{node_id}/commands
用途：可选，Hub 对指定节点下发控制命令。

### 7.3 订阅关系

- Hub 订阅 ntrs/node/+/register。
- Hub 订阅 ntrs/node/+/presence。
- Hub 订阅 ntrs/node/+/heartbeat。
- 所有 NTRS Node 订阅 ntrs/hub/cluster/snapshot。
- 所有 NTRS Node 订阅 ntrs/hub/cluster/events。

## 8. 消息模型

### 8.1 register 消息

```json
{
  "node_id": "bj-prod-01",
  "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
  "region": "cn-bj",
  "public_ip": "203.0.113.10",
  "stun_endpoints": [
    "203.0.113.10:3478/udp"
  ],
  "control_endpoint": "203.0.113.10:19000/tcp",
  "capabilities": [
    "binding",
    "change-request",
    "probe-assist"
  ],
  "version": "1.1.0",
  "started_at": "2026-04-10T10:00:00Z"
}
```

说明：

- register 主要承载静态信息。
- 节点重启后 boot_id 必须变化。
- Hub 收到 register 后不立即认为节点稳定在线，还需要结合 presence online。

### 8.2 presence 在线消息

```json
{
  "node_id": "bj-prod-01",
  "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
  "status": "online",
  "reason": "startup",
  "ts": "2026-04-10T10:00:02Z"
}
```

要求：

- retain=true
- qos=1

### 8.3 presence 离线消息

正常下线：

```json
{
  "node_id": "bj-prod-01",
  "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
  "status": "offline",
  "reason": "graceful_shutdown",
  "ts": "2026-04-10T12:00:00Z"
}
```

异常下线遗嘱：

```json
{
  "node_id": "bj-prod-01",
  "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
  "status": "offline",
  "reason": "lwt",
  "ts": "2026-04-10T11:23:45Z"
}
```

说明：

- 两类离线消息都发布到同一个 presence 主题。
- Hub 根据 reason 区分正常退出和异常掉线。

### 8.4 heartbeat 消息

```json
{
  "node_id": "bj-prod-01",
  "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
  "load": 12,
  "session_count": 83,
  "probe_rtt_ms": 21,
  "health": "ok",
  "ts": "2026-04-10T10:00:10Z"
}
```

### 8.5 Hub 事件消息

```json
{
  "event": "node_online",
  "node_id": "bj-prod-01",
  "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
  "reason": "startup",
  "cluster_version": 102,
  "ts": "2026-04-10T10:00:03Z"
}
```

可选 event 取值：

- node_online
- node_offline
- node_abnormal_offline
- node_updated
- node_drained

### 8.6 Hub 快照消息

```json
{
  "cluster_version": 102,
  "ts": "2026-04-10T10:00:03Z",
  "nodes": [
    {
      "node_id": "bj-prod-01",
      "boot_id": "1e7d5f1c-35e7-4f5f-8b61-7fe4ea3e9af2",
      "public_ip": "203.0.113.10",
      "stun_endpoints": [
        "203.0.113.10:3478/udp"
      ],
      "control_endpoint": "203.0.113.10:19000/tcp",
      "status": "online",
      "region": "cn-bj",
      "load": 12,
      "last_heartbeat": "2026-04-10T10:00:10Z"
    }
  ]
}
```

## 9. 生命周期设计

### 9.1 启动流程

```text
1. NTRS Node 生成 node_id / boot_id
2. 初始化 MQTT 客户端，并设置 presence 主题的遗嘱消息
3. 连接 Broker
4. 发布 register
5. 发布 presence online(retain=true)
6. 订阅 Hub snapshot/events
7. 接收 snapshot，建立本地节点视图
8. 开始发送 heartbeat
9. 对外提供 STUN 服务
```

### 9.2 正常下线流程

```text
1. 节点停止接收新探测任务
2. 节点发布 presence offline(reason=graceful_shutdown, retain=true)
3. 节点可选发布 node drain 完成事件
4. 节点主动断开 MQTT
5. Hub 发布 node_offline 事件并更新 snapshot
```

### 9.3 异常掉线流程

```text
1. 节点进程崩溃、机器断电或网络中断
2. MQTT Broker 检测连接丢失
3. Broker 自动发布节点遗嘱到 presence 主题
4. Hub 收到 offline(reason=lwt)
5. Hub 将节点标记为异常下线
6. Hub 发布 node_abnormal_offline 事件并刷新 snapshot
7. 其他节点停止选择该节点参与协作探测
```

## 10. 遗嘱机制设计

MQTT 遗嘱是本架构处理异常掉线的关键机制。

建议配置：

- will topic：ntrs/node/{node_id}/presence
- will qos：1
- will retain：true
- will payload：offline + reason=lwt + boot_id

关键约束：

- 遗嘱消息必须与正常在线状态使用同一主题，这样该主题始终代表节点最新 presence。
- 节点上线后必须尽快发布 online retained，覆盖之前保留的 offline 状态。
- Hub 必须校验 boot_id，忽略旧连接产生的过期遗嘱。

## 11. NTRS Node 协同探测流程

### 11.1 典型探测流程

```text
1. Client 向 NTRS Node A 发起 Binding Request
2. Node A 查询本地 cluster view，选出 Node B 作为协作节点
3. Node A 向 Node B 发送 probe-assist 请求
4. Node B 从自身公网地址向 Client 发出辅助探测包
5. Node A 汇总本地和远端结果，判断 NAT 类型
6. Node A 返回探测结果给 Client
```

### 11.2 节点选择建议

优先级建议：

- 优先选择 status=online 的节点。
- 优先选择最近心跳新鲜的节点。
- 优先选择与本节点网络出口不同的节点。
- 避免选择被标记为 drained 或 degraded 的节点。
- 当可协作节点不足时，降级为单节点 STUN 模式。

### 11.3 本地视图更新策略

节点侧建议同时维护：

- 一份来自 snapshot 的完整表。
- 一条按顺序消费的 events 流。

处理逻辑：

- 启动时先拿 snapshot。
- 运行期用 events 增量更新。
- 如果发现 cluster_version 跳变或事件丢失，重新拉取 snapshot。

## 12. Hub 状态机建议

节点在 Hub 侧建议具备以下状态：

- unknown：仅收到零散消息，尚未完成注册。
- online：register + presence online 已建立。
- suspect：心跳超时但未收到离线遗嘱。
- offline：正常下线。
- abnormal_offline：由遗嘱或超时推断异常退出。
- drained：不再接新任务，但进程仍在线。

状态转换建议：

- register + online -> online
- heartbeat timeout -> suspect
- graceful offline -> offline
- lwt offline -> abnormal_offline
- admin command drain -> drained

## 13. 一致性与容错

### 13.1 需要解决的问题

- 节点刚重启时，旧遗嘱晚到。
- Hub 重启后需要快速恢复成员视图。
- 某些事件丢失后节点本地视图不一致。

### 13.2 建议策略

- 使用 retained snapshot，让新节点和重启 Hub 快速恢复。
- 所有 presence 消息带 boot_id。
- Hub 对每次成员变更递增 cluster_version。
- 节点发现 cluster_version 不连续时主动重拉 snapshot。
- heartbeat 超时仅进入 suspect，不直接硬删除。

## 14. 部署建议

### 14.1 逻辑部署

- 1 个 MQTT Broker
- 1 个 NTRS Hub
- N 个 NTRS Node

最小可用部署：

- 1 个 Hub
- 2 个 NTRS Node
- 1 个 Broker

### 14.2 端口建议

- MQTT：1883 或 8883(TLS)
- STUN：3478/udp
- NTRS Node 间控制端口：19000/tcp

### 14.3 生产要求

- Broker 启用认证。
- 优先使用 TLS。
- 限制节点可发布和订阅的 topic ACL。
- 对 Hub 和 Broker 做进程级监控与自动拉起。

## 15. 配置草案

```yaml
ntrs_node:
  node_id: "bj-prod-01"
  region: "cn-bj"
  stun_listen: "0.0.0.0:3478"
  public_ip: "203.0.113.10"
  control_listen: "0.0.0.0:19000"

mqtt:
  broker: "mqtt.example.com"
  port: 1883
  client_id: "ntrs-node-bj-prod-01"
  username: "ntrs"
  password: "***"
  keepalive_sec: 30
  will_topic: "ntrs/node/bj-prod-01/presence"
  will_qos: 1
  will_retain: true

hub:
  snapshot_topic: "ntrs/hub/cluster/snapshot"
  events_topic: "ntrs/hub/cluster/events"

cluster:
  heartbeat_interval_sec: 10
  heartbeat_timeout_sec: 35
  suspect_timeout_sec: 60
  peer_select_min_online_nodes: 2
```

## 16. 与当前代码结构的映射建议

仓库中现有 ORION 代码已经接近这个方向，可以按以下方式迁移：

- OrionServer -> NtrsNode
- OrionHub -> NtrsHub
- orion/server/register -> ntrs/node/{node_id}/register
- orion/server/status -> 拆分为 ntrs/node/{node_id}/presence 和 heartbeat
- orion/services/update -> ntrs/hub/cluster/snapshot
- orion/services/status -> ntrs/hub/cluster/events

当前代码还缺少的关键能力：

- MQTT connect 前设置遗嘱。
- Node 订阅 Hub 快照和事件。
- boot_id 与 cluster_version。
- Hub 侧对 wildcard topic 的统一消费与成员表维护。
- 正常下线和异常下线的区分逻辑。

## 17. 实施顺序建议

### Phase 1：控制面打通

- 定义 NTRS Hub 和 NTRS Node 的主题与 JSON 协议。
- Node 接入 register / presence / heartbeat。
- Hub 接入 snapshot / events 维护。
- 增加遗嘱与 graceful shutdown。

### Phase 2：节点视图驱动协同探测

- Node 订阅 snapshot / events。
- Node 建立本地 peer registry。
- 探测流程从静态 peer 改为动态选点。

### Phase 3：可靠性增强

- 引入 boot_id 校验。
- 引入 suspect 状态。
- 引入 drain 和负载阈值。
- 补齐 ACL、TLS、监控和告警。

## 18. 结论

新的目标架构应当明确为：

- NTRS 是 STUN Node，本身承载 NAT 探测能力。
- NTRS Hub 是控制面成员中心，不进入 STUN 数据面。
- 节点通过 MQTT 向 Hub 注册、心跳、上下线。
- 节点通过订阅 Hub 快照和事件，感知其他 STUN Node 的上线与下线。
- 节点通过 MQTT 遗嘱处理异常掉线，Hub 将其转换为标准集群事件。

这样做的收益是：

- 架构边界清晰，Hub 不会成为探测数据瓶颈。
- 节点视图能自动收敛，便于动态扩缩容。
- 异常掉线有 Broker 遗嘱兜底，在线状态更可信。
- 与当前仓库已有 MQTT 雏形兼容，重构成本可控。