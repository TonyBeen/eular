# NAT 服务控制面实现细节

> 本文描述 `natd_hub` 与 `natd_node` 的当前控制面。它们为 NAT 探测集群服务，不保存 NTRS UDP Rendezvous 注册，也不转发 P2P 业务流。

## 1. 服务角色与地址族

`natd_hub` 维护在线 Node 表、健康状态和协作分配；`natd_node` 对外提供 UDP Binding 探测，并通过到 Hub 的 TCP 控制连接获得同族协作 Node。一个 Node 在每个地址族上公开三类 endpoint：

```text
probe        主 Binding 请求与普通响应
change-port  同 IP 的第二 UDP 端口，用于 CHANGE_PORT 响应
control      Node 间 TCP/TLS 控制连接
```

Node 的公网 IP 由 Hub 从注册控制连接的来源观察得出，Node 不能自行上报或覆盖。所有 endpoint 必须同族；IPv6 使用独立的 IPv6-only Hub/Node 实例，不能与 IPv4 socket 共用。`--interface` 时所有对应 socket 使用 `SO_BINDTODEVICE`，多线路部署应显式指定网卡。

默认端口为 Hub control `7700`、Node probe `7800`、Node change-port `7801` 和 Node control `7900`。Hub/Node 控制连接默认是明文 TCP；同时配置证书和私钥时启用 TLS 1.3。

## 2. Node 身份、注册与健康检查

Node 的稳定身份是 1 至 128 字节的 `node_id`，一次进程启动另生成 16 字节随机 `boot_id`。二者共同组成实例身份：相同 node_id 但不同 boot_id 表示新实例，Hub 原子替换旧实例；旧实例的心跳和断连不能续租或删除新实例。

注册内容包括实例身份、每个地址族的公网/probe/change-port/control endpoint、当前 load 和心跳周期。Hub 成功接受后返回它观察到的 Node 公网地址。Node 后续发送携带实例和 load 的 HEARTBEAT；Hub 淘汰连续三个心跳周期未更新的 Node，并在控制连接断开时立即删除完全匹配的实例。

Node 到 Hub 的控制连接失败采用指数退避：从初始延迟翻倍，达到上限后固定间隔重试。注册成功后重置为初始延迟。重连、替换和心跳均不改变 Node 的稳定 `node_id`。

## 3. 主备协作分配

Hub 对每个 Node、每个地址族发布单调递增的 assignment。assignment 可包含 primary、backup 或两者都为空；每个 peer 都携带实例身份、probe endpoint 和 control endpoint。Hub 优先选择健康、同族、非自身且负载较低的 Node。

Node 只在收到更高版本 assignment 时切换协作对象。primary/backup 任一失效时，Node 发送 `NODE_ASSIGNMENT_REQUEST`，其中带当前版本、地址族和失效角色；Hub 只替换被标记的角色，仍健康的角色保持不变。这样一次控制链路故障不会导致整个协作集同时抖动。

Node 间 TCP 连接以 `NODE_LINK_HELLO` 完成身份确认。双方同时拨号时，由发起 node_id 和随机 nonce 建立确定性顺序，仅保留一条连接；重复链路收到 `NODE_LINK_DUPLICATE` 后关闭，避免互相重连形成多条等价通道。

## 4. 控制帧和重组边界

控制流使用固定 8 字节头：

```text
version:u8 | type:u8 | reserved:u16 | payload_length:u32
```

当前版本为 3；reserved 必须为零，单条完整消息（包含头）不超过 4096 字节。TCP/TLS 是字节流，不保证消息边界，因此接收侧使用有界重组器：先积累固定头，再只接受声明长度的完整 payload；非法版本、类型、保留位、长度或固定消息尺寸立即视为协议错误并关闭控制流。

当前类型包括 NODE_REGISTER、NODE_REGISTER_OK、NODE_REGISTER_REJECT、NODE_ASSIGNMENT、NODE_ASSIGNMENT_REQUEST、NODE_HEARTBEAT、NODE_LINK_HELLO、NODE_LINK_DUPLICATE、NODE_LINK_PING、NODE_LINK_PONG 和 NAT_FORWARD_BINDING_RESPONSE。编码、严格解码与字段约束以 `ntrs/include/ntrs/service.h` 和 `ntrs/src/control.c` 为准。

## 5. 跨 Node 的 Binding 响应

客户端向 primary Node 发出 PRIMARY_BINDING 并请求 `CHANGE_IP|CHANGE_PORT` 时，primary 通过已确认的 Node control 链路向协作 Node 投递 `NAT_FORWARD_BINDING_RESPONSE`。该消息携带：目标 Node 实例、客户端源 endpoint、短期 forward id、原 NAT_PROBE 包号、原 probe token 和步骤。

协作 Node 仅在目标实例与当前实例、步骤和 token 都有效时，从自身 change-port socket 向客户端发送一次响应。这样客户端能观察到不同公网 IP 和端口的返回路径，而不需要让 Hub 参与每个 UDP 数据报。forward id 仅用于短期去重；任何无效、过期或无法定位协作 Node 的转发请求直接丢弃。

## 6. UDP Worker 与资源限制

每个 Node 的 UDP probe 服务可启用多个 `SO_REUSEPORT` worker；每个 worker 独立运行 libevent loop 和收发 socket。所有 worker 共享按源 IP 分片的令牌桶限速表，默认每源 128 请求/秒、突发 256，长期空闲条目会回收。接收端严格要求 NAT_PROBE 包为固定 128 字节、零 CID、合法包号和完整 token；响应长度不超过请求，避免 UDP 反射放大。

worker 的跨 Node 响应请求只投递回 Node 主循环，再由该循环沿控制链路发送。其他线程不直接操作 worker 的 event loop 或输出队列。worker 创建、socket 错误、跨线程通知失败和 event loop 无法启动都属于进程级故障，应由外部守护进程重启。

## 7. 与 Context NAT 探测的边界

客户端 Context 使用同一个已 bind UDP socket 向 Node 发送 NAT_PROBE；因此探测得到的映射才对后续 P2P 发送有意义。探测使用零 CID、独立包号和 probe token，不进入 UTP Connection 的 ACK、拥塞控制或重传状态。Context 只消费 Binding 响应并输出 NAT 分类；它不知道 Hub、Node assignment 或跨 Node 转发细节。
