# libutp NTRS 半连接与 Rendezvous 规格

- 日期: 2026-08-22
- 状态: 已确认，实施前的协议权威规格
- 范围: Context 与 NTRS 的注册、保活、端口样本、Rendezvous 信令、预连接开洞、直连握手归并

> NAT 服务仅负责探测；NTRS 负责注册、端口样本和打洞协调。调用
> `utp_context_connect()` 的 A 始终是 UTP 握手发起方。

## 1. 基本原则

- NTRS 指打洞服务器。它维护注册表、地址样本、端口预测与 rendezvous 转交，不转发业务数据。
- Peer 与 NTRS 不建立 UTP connection。不存在 NTRS connection CID、stream、拥塞控制、传输参数、
  普通 UTP `PING + ACK` 保活或 `CONNECTION_CLOSE`。
- Peer 与 NTRS 维护无连接半连接关联，只保存注册身份、对端 UDP endpoint、保活和少量待投递状态。
- 普通 peer-to-peer UTP connection 仍使用既有 `PING` 帧与 `ACK` 帧保活；不得与本规格的
  `FrameRendezvous(PING/PONG)` 混用。
- NTRS 不根据 NAT 类型交换 A/B 的 UTP 握手角色。A 永远发送最终 `INITIAL` 或 `0RTT`，B 永远
  响应 `HANDSHAKE` 或 0-RTT 的 `HANDSHAKE_DONE`。
- NAT 类型仅影响 CandidatePlan 的端口来源和候选发送调度。NTRS 生成端口候选，Peer 不自行预测。
- NTRS 请求方 A 可以注册也可以不注册。注册只决定节点能否作为目标 B 被查找、接收 `FORWARD`、
  保存端口样本；NTRS 不因 A 未注册拒绝 `REQUEST`。

## 2. 包类型、帧包络与包号

```c
UTP_PACKET_TYPE_RENDEZVOUS = 0x06
```

`FrameRendezvous` 是长度界定的通用帧，线上包络为：

```text
type:u8 | message_type:u8 | payload_length:u16 | payload
```

- 所有多字节整数使用网络字节序。
- `payload_length` 不包含四字节包络，且不得超过包剩余字节数。
- 未知 `message_type` 按 `payload_length` 跳过；同一包中同类消息不得重复。
- `FrameRendezvous` 只允许出现在 `UTP_TYPE_RENDEZVOUS`，以及 `INITIAL`/`0RTT` 的明文首帧。

所有 `UTP_TYPE_RENDEZVOUS` 包固定满足：

```text
scid = 0
dcid = 0
packet_number != 0
reserve = 0
```

包号规则：

- 客户端 Context 对所有主动发出的 `UTP_TYPE_RENDEZVOUS` 使用同一个 Context 级递增包号域，
  不区分 NTRS、calibration endpoint 或 peer 候选地址。
- NTRS 对外是一个逻辑服务 endpoint。多 worker/多 Context 部署时，NTRS 所有主动发出的
  `UTP_TYPE_RENDEZVOUS` 使用进程级原子递增包号域，不能让各 worker 独立分配。
- 新构造的逻辑包分配新包号；同一逻辑包重传复用原包号与原始 payload。一次 fanout 的多个 UDP
  副本也是同一个逻辑包，仅 UDP 目标地址不同。
- 半连接接收端不要求包号连续。`PONG.acknowledged_packet_number` 只与仍待确认的本地逻辑包匹配。

## 3. 注册、身份与回调

每个 Context 都设置一个文本 `peer_id`，长度为 `1..128` 字节。它是路由与应用识别信息，
不是身份认证凭据。NTRS 注册的 target 节点 B 以此键被查找；A 的 `REQUEST` 也携带本 Context 的
`peer_id`，无论 A 是否注册。

`registration_token` 为 NTRS 签发的 8 字节随机值。它是当前注册关联的唯一凭据；不使用
`generation`。

首次注册、REGISTERED 响应丢失重传和已注册更新的语义如下：

```text
首次注册：
  Context 生成 registration_request_id，REGISTER 中 token 全零。
  NTRS 创建记录并签发 token。

REGISTERED 丢失：
  Context 使用相同 request_id 与相同 REGISTER 内容重传。
  NTRS 返回缓存的同一 REGISTERED，不创建第二条记录、不更换 token。

已注册后再次调用注册接口：
  Context 生成新的 request_id，携带当前 token。
  NTRS 原子更新 NAT 类型和 local candidates，token 保持不变。
```

注册成功回调使用可扩展结构体而非位置参数：

```c
typedef struct utp_ntrs_registered_info {
    const char*    peer_id;        // 仅回调期间借用
    utp_endpoint_t ntrs_endpoint;  // 实际注册成功的 NTRS IP:port
} utp_ntrs_registered_info_t;
```

后续字段只在该结构体末尾追加，不改变回调签名。回调只表达注册成功，不暴露端口预测或校准结果。

### 3.1 REGISTER 与 REGISTERED

`REGISTER` 消息体：

```text
registration_request_id:u64
registration_token:8              // 首次注册全零，更新时为当前 token
peer_id_length:u8                 // 1..128
peer_id:bytes                     // 当前 Context peer_id
nat_class:u8
local_family:u8                   // 4 或 6
local_port:u16                    // 当前 Context UDP 端口
local_candidate_count:u8          // 0..4
local_addresses[count]:4 或 16    // 由 local_family 决定
```

- 注册时 Context 重新枚举 local candidates。显式绑定网卡时只枚举该网卡；绑定未指定地址时排除
  loopback、link-local、Docker、bridge、veth、tunnel 等地址，按接口优先级保留最多四个。
- 一个 Context 只绑定一种地址族，因此 `local_family`、`local_port` 在一批地址中只编码一次。
- NTRS 以 `peer_id` 建立 B 的目标索引，并从 UDP 源地址观察 B 的公网 endpoint；客户端不得自报
  公网 endpoint、NAT 探测端口样本或覆盖服务端观测。
- 首次注册的 `peer_id` 已有活动记录时，NTRS 拒绝该请求；只有携带该记录当前
  `registration_token` 的注册更新可以原子覆盖自身记录。

`REGISTERED` 消息体：

```text
registration_request_id:u64
registration_token:8
calibration_id:u64                // 非 SYMMETRIC 时为 0
calibration_endpoint_count:u8     // 非 SYMMETRIC 时为 0
calibration_endpoints[count]:
  family:u8
  port:u16
  address:4 或 16
```

Context 与 NTRS 各自配置本地保活周期与超时，不在 `REGISTERED` 中协商 lease 或 keepalive interval。

### 3.2 保活与 calibration

`PING` 消息体：

```text
registration_token:8
calibration_id:u64                // 普通保活为 0
```

`PONG` 消息体：

```text
registration_token:8
acknowledged_packet_number:u64
```

双方在自己的有效入站关联报文长期缺失时发送 `PING`。收到任一有效关联报文后更新最近活动时间；
只因本地 `send` 成功不得延长对端存活判定。哪一侧本地保活周期更短，通常由哪一侧先发起，另一侧
回 `PONG` 即可。`PING` 未得到对应 `PONG` 时，发送方按自己的超时/重试配置失效该关联。

仅 `UTP_NAT_CLASS_SYMMETRIC` 在首次注册后进入本轮 calibration：

```text
B -> calibration endpoints:
  按 REGISTERED 给定顺序，各发送一次 PING(token, calibration_id)。

endpoint:
  校验 token、calibration_id 与目标端口；记录 B 实际公网源 IP:port；回复 PONG。

B:
  收齐 PONG 或本轮 calibration 超时后，触发注册成功回调。
```

`UNKNOWN` 不进行 calibration，打洞时按对称 NAT 的随机端口候选处理。calibration 样本完整、部分
成功或完全超时都不改变“注册成功”的回调语义。

当 NTRS 判断现有端口模型失真时，使用 `CALIBRATE` 重新请求采样：

```text
registration_token:8
calibration_id:u64
endpoint_count:u8
endpoints[count]:
  family:u8
  port:u16
  address:4 或 16
```

NTRS 将 `PING + CALIBRATE` 合在同一 datagram 投递。B 按 `calibration_id` 去重，首次收到时按顺序
发送一次 calibration PING，并回 PONG 确认投递；重复收到只回 PONG，不重复发送校准包。

## 4. 地址观测、样本与预测

端口预测完全由 NTRS 完成。B 不计算、不上报预测结果；它只按 NTRS 指令从当前 Context socket 发
calibration PING，并将其他 peer 观察到的本机公网地址样本上报给 NTRS。

NTRS 初始使用同一公网 IP 的不同 calibration UDP 端口建立对称 NAT 的基础样本。后续由真实 P2P
连接累积样本，提高预测与候选排序质量；不要求 NTRS 额外部署第二公网 IP。

保活发现 B 的公网 IP 改变时，NTRS 更新当前公网 IP，但保留端口模型。NTRS 将新观测端口与旧模型
期望区间比较：差异可接受时继续使用模型；差异过大时清空模型并通过 `CALIBRATE` 重新采样。

### 4.1 建连后 OBSERVED_ADDRESS

`OBSERVED_ADDRESS` 是连接级可靠控制帧，线上格式为：

```text
type:u8 | family:u8 | port:u16 | address:4 或 16
```

IPv4 帧长度为 8 字节、IPv6 为 20 字节。公网观测不编码 IPv6 `scope_id`，`family` 仅允许 4 或 6。
它的语义固定为“接收方从当前连接路径看到的发送方公网源 IP:port”。

每条 connection 进入 CONNECTED 后，在当前路径安排一次延迟路径确认：

1. 延迟到期前若已有业务包或普通可靠控制包要发送，合入尚未发送的 `PATH_CHALLENGE`。
2. 到期仍无包可发送时，主动排一个携带 `PATH_CHALLENGE` 的控制包。
3. 收到 challenge 后，尽快发送 `PATH_RESPONSE + OBSERVED_ADDRESS`。

`PATH_RESPONSE` 是瞬态帧；challenge 重传会促使对端再次产生 response。`OBSERVED_ADDRESS` 是可靠控制
帧，随正常 PacketOut ACK/丢失逻辑重传。重复接收可按 endpoint 去重，不需要 `observation_id`。
这套机制对普通 1-RTT、0-RTT 和直连/打洞连接完全相同，不混入 Initial、Handshake 或 HandshakeDone。

### 4.2 ADDRESS_UPDATE

`utp_context_update_address()` 一次接受多个本机公网地址样本。上层提交与收到
`OBSERVED_ADDRESS` 后产生的样本，均进入同一个 Context 待确认队列。

每个样本只有：

```text
address_family
public_ip
public_port
observed_at_unix_ms
```

Context 为每个待发送批次分配 `update_id`。`ADDRESS_UPDATE` 消息体：

```text
registration_token:8
update_id:u64
sample_count:u8
samples[sample_count]:
  family:u8
  port:u16
  address:4 或 16
  observed_at_unix_ms:u64
```

`ADDRESS_UPDATED` 消息体：

```text
update_id:u64
```

NTRS 只接受 `public_ip` 等于当前从 B 有效保活包观察到的公网 IP 的样本。它可忽略不匹配、非法或
过期样本，但收到并处理一个 `update_id` 后必须回复 `ADDRESS_UPDATED(update_id)`；B 不需要知道采纳
数量、预测模型或拒绝原因。

未收到 `ADDRESS_UPDATED` 时，Context 以 1 秒初始超时指数退避，默认额外重试三次；这两个参数均可
配置。确认后删除整个批次；重试耗尽后删除并记录 warning，不影响既有 UTP connection 或触发连接错误
回调。

## 5. CandidatePlan 与预连接开洞

NTRS 下发的候选计划固定为：

```text
family:u8                         // 4 或 6
local_port:u16
local_candidate_count:u8          // 0..4
local_addresses[count]:4 或 16
public_address:4 或 16
public_port_count:u8              // 1..4
public_ports[count]:u16           // 有序、去重、非零
```

`local_candidates` 来自目标注册或请求方 REQUEST，适合同局域网与 hairpin 场景；`public_address` 和
端口列表来自 NTRS 的服务端观测与预测。端口顺序是 Peer 发送打洞包和握手 fanout 的顺序。NTRS 可按
顺序模型、有限范围或随机候选构建端口表；预测不足时返回默认四个随机端口，具体数量可配置，但任何
计划最多四端口。

收到 `REDIRECT/FORWARD` 后，A、B 从同一个 Context socket 向对方 CandidatePlan 中每个 endpoint
各发送一次零 CID `PATH_CHALLENGE`：

```text
UTP_TYPE_RENDEZVOUS
scid = 0
dcid = 0
payload = PATH_CHALLENGE(data[8])
```

该包仅用于创建出向 NAT 映射和过滤规则。接收端静默丢弃，绝不回复 `PATH_RESPONSE`，不创建 connection
或 pending，也不做路径选择。每个候选只发送一次；临时本地发送阻塞可在 attempt 期限内重试。最终
`INITIAL/0RTT` 的正常握手 PTO 重传负责后续可达性，不重发零 CID 打洞包。

## 6. REQUEST、REDIRECT、FORWARD 与握手

A 调用带目标 `peer_id` 的 `utp_context_connect()` 或 `utp_context_connect_0rtt()` 时，目标 endpoint
可以是直接 peer，也可以是 NTRS。调用方不需要选择另一套 punch API。

`REQUEST` 消息体：

```text
rendezvous_id:16
source_peer_id_length:u8
source_peer_id:bytes             // 1..128，取 A Context peer_id
target_peer_id_length:u8
target_peer_id:bytes             // 1..128
source_nat_class:u8
local_family:u8
local_port:u16
local_candidate_count:u8         // 0..4
local_addresses[count]:4 或 16
```

`rendezvous_id` 是 128 位随机值，是 REQUEST 重传幂等键、B 侧 pending 归并键和最终握手匹配键。
NTRS 以 `target_peer_id` 查找已注册 B；A 是否注册不参与请求接受条件。A 的公网地址由 NTRS 从 REQUEST
承载 UDP 包的源地址观察，不能在 REQUEST 中自报。

NTRS 的下行消息体：

```text
REDIRECT:
  rendezvous_id:16
  target_plan:CandidatePlan       // B 的候选计划

FORWARD:
  rendezvous_id:16
  source_peer_id_length:u8
  source_peer_id:bytes
  source_plan:CandidatePlan       // A 的候选计划
```

NTRS 收到重复 REQUEST 时重发相同 REDIRECT，不重复创建 FORWARD 事务。向 B 的 FORWARD 与 PING 合包；
B PONG 回显该包号后，NTRS 才移除待投递项。PONG 丢失时重投同一个 `PING + FORWARD`，B 以
`rendezvous_id` 去重并再次 PONG。

`INTRODUCTION` 消息体只有：

```text
rendezvous_id:16
```

帧顺序固定：

```text
A -> NTRS 的 INITIAL：
  FrameRendezvous(REQUEST) 必须为第一个帧，后面是普通 Initial 握手帧。

A -> B candidate 的最终 INITIAL：
  FrameRendezvous(INTRODUCTION) 必须为第一个帧，后面是普通 Initial 握手帧。

A -> B candidate 的最终 0-RTT：
  明文 FrameRendezvous(INTRODUCTION) 为第一个帧；
  明文 SESSION_TOKEN 紧随其后；
  后面是既有加密 0-RTT 帧与 early data。
```

目标是普通 direct peer 时，它识别并忽略第一个 `REQUEST`，继续普通握手。目标是 NTRS 时，NTRS 只解析
REQUEST，不创建 UTP connection。最终收到带匹配 `INTRODUCTION` 的 B 以 `rendezvous_id` 命中 pending，
再按普通握手流程处理。

同一轮 candidate fanout 中，A 向所有候选发送字节完全相同的 `INITIAL/0RTT` 数据报，仅 UDP 目标地址
不同；相同的包号、ciphertext、early nonce、stream offset 使副本按既有包号与流重组规则去重。后续握手
PTO 仍向未排除候选 fanout，收到合法握手响应后由既有连接状态固定实际对端 endpoint。

## 7. 反注册与拒绝

`UNREGISTER` 消息体：

```text
registration_token:8
```

`UNREGISTERED` 消息体：

```text
registration_token:8
```

NTRS 删除注册后为 token 保留短时 tombstone。重复 `UNREGISTER` 命中 tombstone 时再次返回
`UNREGISTERED`，使响应丢失后的 Context 重传仍可确认成功。新注册会得到新 token，不受旧 tombstone
影响。

通用 `REJECTED` 消息体：

```text
rejected_message_type:u8
reference_length:u8
reference_id:bytes               // REGISTER 为 8，REQUEST 为 16，UNREGISTER 为 8
reason_code:u16
```

`reason_code` 是 NTRS 私有协议码，例如 `INVALID_REQUEST`、`PEER_NOT_FOUND`、`PEER_ID_EXISTS`、
`TOKEN_INVALID`、`OVERLOADED`。Context 仅将其映射为既有公开 `utp_status_t` 或日志，不将其作为公开 ABI。

## 8. NTRS 多线程服务模型

NTRS 服务端仅要求 Linux 部署。为同时保持单一对外 endpoint 与多核处理能力：

```text
同一 NTRS IP:port
  ├─ worker 0: event_base + UDP socket + utp_context
  ├─ worker 1: event_base + UDP socket + utp_context
  └─ worker N: event_base + UDP socket + utp_context
```

所有 worker socket 显式使用 `SO_REUSEPORT`。这是 NTRS 服务端的有意多核分流，不适用于通用 libutp
Context 的默认 socket 行为。每个 worker 运行独立 libevent 循环；不得为此另写事件循环。

NTRS 进程共享以下资源：

```text
peer_id -> Registration
registration_token -> Registration
rendezvous_id -> ForwardPending
```

索引按 peer_id、token 或 rendezvous_id 分片；每次只在对应 shard 上短暂加锁完成查找、更新或取得稳定
记录引用，UDP 编解码与发送均在锁外。任一 worker 都可处理 B 的 PING、PONG、ADDRESS_UPDATE 或
calibration 报文；进程级包号使 PONG 可通过 `(registration_token, acknowledged_packet_number)` 全局匹配
待确认的 `PING + FORWARD`，无需跨线程转发 UDP 数据报。

## 9. 实现边界

- 公开注册、反注册、地址更新、peer_id 设置及回调都在 Context 所属事件循环线程调用，不支持跨线程。
- NTRS 逻辑和 `UTP_TYPE_NAT_PROBE` 分流必须在普通 connection/pending 查找之前处理。
- `UTP_TYPE_RENDEZVOUS` 的未知或非法消息、非法零 CID 组合和无匹配 token/rendezvous_id 的包均静默丢弃。
- 预连接零 CID `PATH_CHALLENGE` 是唯一例外：它是合法但无响应的打洞包；已建连后的 PATH 挑战/响应仍走
  既有 connection 路径。
- NTRS 不分片。注册、CandidatePlan、FORWARD 和请求帧必须受当前 MTU 限制；local candidates 与
  public ports 各自最多四项。
