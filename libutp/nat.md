# P2P 打洞与端到端身份握手演进方案

## 1. 分层边界

后续实现拆成三个相互独立的层次：

1. 中心服务层：TLS 登录、设备 Ed25519 公钥目录、候选地址和握手消息转发、吊销状态分发。
2. NAT traversal 层：收集 host/server-reflexive/relay candidate，执行 STUN 探测、打洞和路径验证，必要时使用 TURN 类中继。
3. 端到端安全层：Ed25519 认证临时 X25519 公钥，HKDF 派生方向隔离密钥，Finished 确认后开放业务数据。

中心服务和 STUN/TURN 只解决身份目录、信令与可达性，不参与端到端密钥计算。Ed25519 用于签名，不能用于 ECDH；X25519 临时私钥每次握手重新生成且不得上传。

## 2. 当前实现边界

当前仓库已经具备：

- UDP 可靠传输、多流、ACK/重传、拥塞控制和路径验证。
- 临时 X25519 shared secret。
- SHA-256/HKDF traffic key schedule。
- client->server 与 server->client 独立 AES-GCM key/nonce prefix。
- packet number 重放过滤、非法 ACK/参数校验和接收流控。

当前尚不具备：

- Ed25519 长期设备身份和中心签名公钥目录。
- userId/deviceId/keyId、nonce、sessionId 绑定的规范化握手 transcript。
- 双向 Finished key confirmation。
- STUN candidate gathering、中心 rendezvous、同时打洞和 TURN fallback。

因此当前加密只能视为未认证的临时密钥交换，不能宣称抵御主动 MITM。加密 0-RTT 已禁用，直到恢复 PSK、方向密钥和重放模型单独完成。

## 3. 推荐状态机

```text
Disconnected
  -> DirectoryVerified
  -> CandidatesGathered
  -> Punching
  -> TransportReachable
  -> IdentityHandshake
  -> FinishedPending
  -> SecureConnected
```

`TransportReachable` 只表示 UDP 路径可用。双方 Ed25519 签名、transcript 和 Finished 全部验证成功前，不得触发安全连接可用回调，也不得投递业务明文。

## 4. 握手集成顺序

1. A/B 从中心获取并验证目标设备的有效 Ed25519 公钥目录项。
2. 双方通过中心交换候选地址并并行发送探测包；路径通过现有 PathChallenge/PathResponse 后标为可达。
3. A 发送带 sessionId、双方设备身份、aEphPub、nonceA、timestamp 的签名 ClientHello。
4. B 校验目录状态、目标绑定、时间窗和重放缓存，再返回签名 ServerHello，包含 bEphPub、nonceB 和 transcriptHash。
5. 双方计算 X25519 shared secret，并以完整身份和握手 transcript 作为 HKDF 上下文派生方向加密密钥、nonce base 和 Finished keys。
6. 双方完成 Finished 校验后销毁临时私钥和中间密钥材料，进入 SecureConnected。

规范化编码必须使用确定性的结构化序列化，不能用字段字符串拼接。任一身份、设备、keyId、CID、临时公钥、nonce、sessionId 或协议版本变化都必须改变签名输入和派生结果。

## 5. 模块建议

- `IdentityKeyStore`：设备 Ed25519 私钥安全存储、签名、轮换。
- `PeerDirectory`：中心目录查询、目录签名验证、吊销和缓存有效期。
- `RendezvousClient`：候选地址和不透明握手消息转发。
- `CandidateGatherer`：host/STUN/TURN candidate 收集与优先级排序。
- `PunchCoordinator`：双方同时探测、重试预算和候选对选择。
- `SecureHandshake`：transcript、签名、X25519、HKDF、Finished 和重放缓存。
- `SecureSession`：方向 counter、AEAD、rekey 阈值和密钥清除。

这些模块不应耦合到 Stream 重传和拥塞控制内部。握手帧可以复用现有可靠传输能力，但身份状态机必须独立于传输可达状态。

## 6. 回归与验收矩阵

- 正常：双方 key material 一致，双向同 packet/message counter 的 nonce/key 组合仍不同。
- 身份：篡改任一签名字段、目录签名、目标 deviceId/keyId 均失败。
- 密钥：全零 shared secret、复用临时私钥、缺失 Finished 均失败。
- AEAD：篡改密文、tag 或 AAD 均不得交付明文。
- 重放：重复 ClientHello、ServerHello、Finished、packet number 和业务 counter 均拒绝。
- 吊销：目录项撤销后不能建立新会话，已有会话按产品策略关闭或限时失效。
- NAT：全锥、受限锥、端口受限、对称 NAT、地址变化和 relay fallback 分别覆盖。
- 资源：伪造候选、Initial 和握手洪泛受全局/单 IP 配额、TTL 和内存上限保护。

离线消息、群聊、多端同步、消息级前向保密和长期会话持续 ratchet 不在第一阶段范围内；需要这些能力时应采用 X3DH + Double Ratchet 等成熟协议，而不是继续扩展一次性在线握手。

## 7. 早期讨论记录（已被第 8 节取代）

本节保留 2026-07 的早期讨论，便于追溯决策过程，**不得作为实现输入**。其中
`PeerResolver`、应用自行决定连接目标、跨服务临时注册和应用/统一身份服务直接
签发 NTRS token 的模型，均已被第 8 节的 Scheduler、`NodeRef`、
`RendezvousOpen` 和 Scheduler 签发 `ntrs_access_grant` 模型取代。NAT 缓存、
逻辑/传输方向分离、明文可达性与端到端身份分离等不冲突的原则仍由第 8 节继承。

### 7.1 目标与边界

- libutp 内部实现与现有 ntrs 服务兼容的控制面、私有 UDP 探测和打洞，不链接或依赖 `ntrs` 库。
- 直连与打洞共用 `Context::connect()`；不增加一个仅用于穿透的 connect API。
- 未调用或未配置 P2P 能力时，现有 `ip:port` 直连行为必须完全保持不变。
- 首期不实现 TURN/中继。对称 NAT、UDP 被阻断或打洞失败应返回明确错误，而不是伪装为已连接。
- 现有 ntrs 服务之间不联邦。要连接某个 `peer_id`，必须能够得到该 peer 所在 ntrs 服务的地址和访问凭据。

### 7.2 面向应用的简化模型

应用希望按 `peer_id` 建连，而不是关心打洞的实际方向。建议由应用已有目录，或 Context 配置的 `PeerResolver`，完成下列解析：

```text
peer_id -> ntrs host:port + 面向该服务的短期 access token
```

`ConnectInfo` 可保留直连 `ip:port`，并增加可选 `peer_id`：

- 仅有 `ip:port`：普通直连。
- 仅有 `peer_id`：解析对端 ntrs 服务，执行 P2P 会话协调和打洞。
- 两者均有：直连与 P2P 尝试并行，首个完成完整 libutp 握手的路径获胜。

本端 P2P 身份、默认 ntrs 服务和访问凭据应作为 Context 配置；在 Context 成功 `bind()` 后自动完成本端服务的认证、NAT 探测、注册和保活，不要求显式 `startRendezvous()`。纯直连 Context 不做这些后台操作。

### 7.3 多 ntrs 服务认证

当前 ntrs 的 `bootstrap_token` 是单服务共享密钥模型，不适合一个节点访问多个独立 ntrs 服务。建议演进为：

- 应用或统一身份服务签发短期 access token，至少绑定 `sub=本端 peer_id`、`aud=ntrs-service-id`、过期时间和最小权限范围。
- 每个 ntrs 服务验证同一发行方公钥，或验证其被授权的 token issuer；libutp 只保存短期 token，不保存各服务的长期共享密钥。
- Context 以 ntrs `host:port` 为键维护短期控制会话池。连接远端 peer 时，按需认证并临时注册本端到该远端服务；空闲、失效或断线后释放或重建。
- 为兼容部署，服务可继续接受旧 `bootstrap_token`，但它只适合单服务或受控环境，不能作为多服务默认方案。

这要求 ntrs 服务端扩展认证校验能力。仅修改 libutp 无法让 A 在不知道 B 服务凭据的前提下使用一个互不联通的 ntrs 服务。

### 7.4 NAT 探测缓存

NAT 探测不应按 peer 重复执行。缓存键建议为：

```text
(Context UDP socket, 地址族, ntrs 探测组)
```

- Full Cone 等稳定映射结果通常可复用给多个 peer 和同类探测组；连接新服务时优先进行轻量验证。
- 缓存必须有 TTL；网络接口变化、UDP socket 重绑、地址族改变、探测组变化或观察到映射变化时，重新执行完整探测。
- 不得将一次 Full Cone 结果无条件推广到所有远端服务和路径，因为地址相关映射、对称 NAT 和多线路网络可能得到不同结果。

### 7.5 逻辑连接方向与传输方向

业务发起方不等于发送首个 libutp Initial 的一方。例如 A 调用 `connect(B)`，但 ntrs 根据 NAT 分类判定 A 为 Full Cone、B 更难被访问时，可下发 `B = transport initiator`：

```text
A: 逻辑调用 connect(B)
  -> ntrs 协调候选和 connect_role
  -> B 向 A 打洞并发送 libutp Initial
  -> A 验证该 Initial 属于等待中的 A->B P2P 会话
  -> A 的 connect(B) 成功
```

实现需要一个内部逻辑连接协调器，而不是要求业务层改为由 B 调用 `connect(A)`：

- 每次 P2P 尝试生成短期 `rendezvous_session_id` 和随机 nonce。
- UDP 打洞请求/确认、候选地址和随后的 libutp Initial 必须绑定该会话，避免任意入站 UDP 包或无关 Initial 被关联为成功。
- 若实际 Initial 从 B 发往 A，A 将匹配的被动握手提升为自己原始 `connect(B)` 的结果；应用仍只收到一次成功回调。
- 未匹配的普通入站 Initial 继续遵循现有 `OnNewConnection` / `accept()` 语义。
- 直连、打洞和反向 Initial 需要在同一逻辑尝试中竞速；首个完成完整 libutp 握手的路径获胜，其他内部尝试必须取消并清理 CID、定时器和状态。

### 7.6 产品优先级与端到端安全层的关系

P2P 第一目标是在 UDP 路径可达时稳定完成连接。ntrs 负责 peer 发现、候选分发、打洞编排和连接方向协调；它不能解决 UDP 被阻断、对称 NAT 或 peer 离线等不可达情形，但这些情形不应影响可达路径上的建连成功率。

- 明文连接是 libutp 的正式能力，用于调试、抓包和不要求保密的业务；P2P 打洞不能要求签名或加密才能建立明文连接。
- 使用者只选择两种传输方式：加密或不加密。当前可选的传输加密与 P2P 可达性解耦，不得因为缺少中心目录签名而拒绝一条原本可用的直连或打洞路径。
- ntrs 后续可提供设备 Ed25519 公钥目录及目录签名，作为可选的握手附加材料。提供目录记录时，双方必须验证目录签名和对端握手签名；验证失败必须以对应错误码关闭连接，且不得交付业务数据。
- 未提供目录记录或目录签名时，连接按所选的明文或加密模式正常处理，不得因缺少身份目录而拒绝。目录不是连接成功的前提条件。
- 不为连接增加 `authenticated` 或 `unauthenticated` 这类强制状态标签；应用通过所选的明文/加密模式及是否提供目录材料表达需求。日志和诊断仍可记录协商的加密模式，但不得泄露密钥或业务内容。

换言之，ntrs 控制面认证、候选地址和打洞 nonce 解决的是可达性与会话关联；Ed25519 身份验证解决的是对端身份与抗主动 MITM。两者应可独立演进，不能让后者阻塞前者。

### 7.7 Connection 级身份认证（P2P 可达性后的安全阶段）

如果后续要求一条加密 Connection 的所有 stream 都具备对端身份验证能力，可以在现有 X25519 + HKDF + AES-GCM 基础上增加 Ed25519 握手签名和双向 Finished。该方案会改变握手报文、状态机和 Connected 的触发时机，因此先作为备选，不与首期 ntrs 打洞同时实施。

建议流程：

```text
A Initial:
  version、CID、session_id、nonceA、加密算法、aEphPub

B Handshake:
  nonceB、bEphPub、可选身份标识
  signatureB = Ed25519_Sign(B private key, transcriptHash)

A:
  若提供了可信目录或预置公钥，则验证目录和 signatureB
  X25519 + HKDF 派生方向隔离的 Connection 密钥和 Finished key
  发送 FinishedA

B:
  验证 FinishedA，返回 FinishedB

双方完成 Finished 后进入 Connected
```

设计约束：

- 使用者仍只选择加密或不加密；身份材料是加密连接的可选附加输入，不增加 `authenticated` / `unauthenticated` 状态。
- 未提供可信目录、预置公钥或证书时，按现有明文或加密模式正常连接。
- 一旦调用方提供了可信身份材料，本地必须要求对端签名，禁止协商降级；目录签名、有效期、对端签名或 Finished 任一校验失败时，以明确错误码关闭连接，且不得交付业务数据。
- transcript 必须使用确定性编码，并绑定协议版本、加密算法、session id、双方身份、CID、nonce、临时公钥以及逻辑/传输角色。
- P2P 反向建连时，必须区分 `logical_initiator` 与 `transport_initiator`，避免将 B 发起的底层 Initial 错误绑定为其他逻辑连接。
- HKDF 应将完整 transcript hash 纳入上下文，并独立派生 client-to-server、server-to-client 和双方 Finished key；握手完成后及时清除临时私钥、共享秘密和中间密钥。
- 所有 stream 继续复用现有 Connection 级 AES-GCM，不在单个 stream 上叠加 TLS。

该阶段需要新增身份/签名和 Finished 帧、握手重传与超时状态、证书或目录验证接口、错误码、资源上限以及篡改/降级/重放回归测试。它不应阻塞首个明文 P2P 可达性阶段，但必须在宣称可信加密直连前完成。

## 8. 首期实施设计

本节将前述讨论收敛为首期可实施方案。它是 libutp 的设计输入；API
名称、帧号和 NTRS 编解码尚未实现，不能在没有互操作测试的情况下视为
线上协议。

### 8.0 Scheduler 调度平面

Scheduler 是独立于 peer 与 NTRS 的逻辑控制面。它不参与 UDP 探测、候选生成、
打洞包或业务数据转发，只维护节点可用性并决定需要建立哪些逻辑连接。本节的
模型取代此前 `PeerResolver` 直接为应用解析目标 peer 的设想。

节点成功注册到某个 NTRS 后，持续向 Scheduler 上报带 lease 和 generation 的
`NodePresence`：

```text
NodeRef = { ntrs_service_id, peer_id }
NodePresence
  node_ref, nat_info, capabilities, load, online_state, generation, lease_expiry
```

`peer_id` 只是所属 NTRS 服务内唯一的字符串，Scheduler 与所有跨服务协议都
必须使用 `NodeRef`，不能把 `peer_id` 单独当作全局身份。NAT 信息用于调度拓扑
和连接优先级；候选与实际 transport direction 仍由目标 NTRS 依据新鲜探测结果
最终决定。

Scheduler 通过节点到 Scheduler 的持久、已认证控制连接推送 `ConnectAssignment`：

```text
ConnectAssignment
  assignment_id, source_node_ref, target_node_ref, expiry
  target_ntrs_service_id, target_ntrs_endpoint
  ntrs_access_grant, route_policy, security_mode
```

节点收到任务后才创建 `ConnectAttempt`。也就是说，节点不自行查询“应该连接谁”；
Scheduler 决定逻辑边，Context 负责执行。连接成功、失败、NAT 变化和 lease 失效
必须回报 Scheduler，供其重新调度。

`ntrs_access_grant` 是 Scheduler 对目标 NTRS 签发的短期授权，至少绑定
`assignment_id`、source/target `NodeRef`、目标 NTRS service id、权限和过期时间。
目标 NTRS 配置 Scheduler 签名公钥后可离线验证该授权。源节点访问目标 NTRS 时
只能执行一次性 `RendezvousOpen`，不得创建普通 peer 注册、NAT 探测、心跳或
映射保活状态。这样即使不同服务都有同名 `peer_id` 也不会冲突。

```text
source -> target NTRS: RendezvousOpen(assignment_id, grant, source_node_ref, target_node_ref)
target NTRS:
  1. 验证 grant、目标 peer 在线状态和 assignment 未消费；
  2. 从本次 UDP/control socket 观察 source endpoint；
  3. 创建短 TTL RendezvousTicket，仅保存 assignment、双方 NodeRef、观察地址和候选；
  4. 向已注册的 target peer 下发 source endpoint、候选、session_id 和 punch 参数；
  5. 向 source 返回 target candidate、session_id 和 punch 参数。
source/target: 直接发送 PUNCH_REQ/PUNCH_ACK，随后尝试 libutp Initial。
```

源节点到目标 NTRS 的临时控制连接保持到 ticket 进入终态，但不创建注册、
heartbeat 或 NAT keepalive。双方必须显式回报结果：

```text
source/target -> target NTRS: RendezvousResult(session_id, SUCCESS | FAILED, reason)
target NTRS -> other peer:     RendezvousComplete | RendezvousCancel(reason)
```

任一方在 PUNCH 或 libutp 握手失败时，目标 NTRS 立即向另一方发送
`RendezvousCancel`，使其停止等待和探测；临时控制连接异常断开且尚未成功时也
视为失败。两端都报告成功，或一方报告成功且另一方已观察到匹配连接成功后，
才发送 `RendezvousComplete` 并释放 ticket 与临时控制连接。Cancel 只终止仍在
pending 的尝试，绝不关闭已经完成的端到端 Connection，以处理成功与取消交叉的
竞态。

`RendezvousTicket` 在上述成功、失败或极短 TTL 到期后删除，不续期。发送给目标
NTRS 的 RendezvousOpen 与短期直连 PUNCH 是建连动作，不是 NAT 映射保活；直连
建立后也不需要继续向目标 NTRS 发送任何数据。若 NAT 映射很短或属于对称 NAT，
打洞可能失败，这是首期不做跨服务 NAT 探测和 relay 的预期限制。Scheduler 可
利用节点已上报的 NAT 信息优先选择更可能成功的逻辑边和 transport direction，
但最终可达性仍由直接 PUNCH 结果决定。

### 8.1 不变的产品语义

每个 Scheduler 任务只产生一次逻辑连接结果，节点不选择实际 UDP 建连方向：

```text
ConnectAssignment -> 一个 Connection 结果
```

- 传入显式 `ip:port` 且未启用 P2P 时，是现有普通直连；不访问 NTRS。
- Scheduler 下发 target `NodeRef`、目标 NTRS 地址和短期授权后，Context 执行
  协调和打洞。
- 任务同时带有直连 endpoint 与 P2P 路由时，直连和 P2P 可以并行尝试；只有
  完成 libutp 握手的路径才能赢得逻辑连接。
- NTRS 的 `CONNECT_ROLE` 只决定谁发送第一个 UDP/Initial。无论实际由 A
  还是 B 发送 Initial，Scheduler 下发的 assignment 都只产生同一个成功或失败
  结果。
- 明文和加密仍是调用方仅有的两种传输选择。P2P 不是第三种连接 API，也不
  是一种安全模式。

首期不实现 relay/TURN。UDP 被阻断、两端对称 NAT 或打洞预算耗尽时应明确
失败；不得把未建立的 P2P 会话伪装为普通直连成功。

### 8.2 单一 API 形态

Context 内部仍使用一个请求模型，而不是 `connect_direct()` 与
`connect_p2p()` 两组入口。直接 endpoint 可由应用发起；P2P 请求由 Scheduler
任务驱动。具体 ABI 在 Context C API 落地时再冻结，字段语义先固定为：

```text
ConnectRequest
  target_node_ref         可选；{ target_ntrs_service_id, peer_id }
  direct_endpoint         可选；IP/port 的已知候选
  assignment_id/grant     可选；Scheduler 下发的 P2P 授权
  route_policy            AUTO | DIRECT_ONLY | P2P_REQUIRED
  security_mode           PLAINTEXT | ENCRYPTED
  optional_identity       可选的目录记录、目录签名或预置信任公钥
```

约束如下：

- `DIRECT_ONLY` 要求 `direct_endpoint`，不会启动 Scheduler 或 NTRS。
- `P2P_REQUIRED` 要求有效 Scheduler assignment，不把无关的静态 endpoint 当作成功替代。
- `AUTO` 允许任一候选路径获胜；如果同时存在 endpoint 和 assignment，应并行，
  但只发布一次最终事件。
- 同一 Context 以 `ntrs endpoint + audience` 缓存控制会话；凭据、目录和
  peer session token 都不可写入日志。

`Context::bind()` 之后可对默认 NTRS 做一次 NAT 探测和注册。连接到其他
NTRS 服务时只新增控制会话和轻量映射验证，不能因为每个 peer 都重新做完整
NAT 分类而阻塞建连。NAT 分类缓存键是 `(UDP socket, 地址族, probe group)`，
并在 TTL 到期、网络变化、socket 重绑或映射变化时失效。

### 8.3 NTRS 兼容适配器

libutp 不链接 NTRS 客户端库。它应实现一个私有 `RendezvousAdapter`，只把
NTRS 线协议映射到 libutp 的内部模型。当前 NTRS 已提供以下足够的会话信号：

```text
SESSION_CREATE_REQ / session signal
  peer_id, peer_device_id
  session_id, peer_session_token, expire_at
  PUNCH_ORDER, CONNECT_ROLE
  WARMUP_ROUNDS, WARMUP_INTERVAL_MS
  host_local, srflx_primary, srflx_secondary candidates
  peer NAT class
```

适配器必须复用当前 NTRS 的私有 `PUNCH_REQ/PUNCH_ACK` 编解码和 token 校验，
而不是发明一个看似相同的新 UDP 打洞包。与 NTRS 的兼容性应由捕获报文和双端
互操作测试保证，而不是由结构体布局猜测保证。

控制面与探测面的安全边界如下：

| 通道 | 承载内容 | 安全要求 |
| --- | --- | --- |
| NTRS 控制通道 | 认证、注册、心跳、候选、会话信令、目录 | TLS 1.3 或等价的已认证安全连接 |
| NTRS 私有 UDP 探测 | 映射探测、PUNCH_REQ/PUNCH_ACK | 可明文；使用短期 probe/session token 做匹配与授权 |
| peer 到 peer 的 libutp | Initial、握手、业务数据 | 由 `security_mode` 与可选身份材料决定 |

NTRS token 的职责仅是控制面授权、打洞匹配和资源限制。它不是 peer 身份证明，
也不能作为端到端会话密钥或签名信任根。

现有 `bootstrap_token` 只能安全地服务于受控的单 NTRS 部署。多 NTRS 部署的
默认方案是 Scheduler 签发短期 `ntrs_access_grant`，并绑定 source/target
`NodeRef`、`assignment_id`、`audience=NTRS service id`、权限范围和到期时间；
各 NTRS 使用 Scheduler 公钥验证。旧 bootstrap token 可保留作兼容模式，不能
成为多服务默认。

### 8.3.1 NTRS 的 QUIC 型 TLS 1.3 连接认证

NTRS 是固定服务端，客户端知道预期域名，例如 `ntrs.example.com`。与 NTRS
直连时应采用与 QUIC 相同的模型：TLS 1.3 的握手认证整个 libutp Connection，
而不是在某一个 control stream 内嵌 TLS record。

TLS 1.3 profile 与轻量 Ed25519 profile 必须由 Context 的受保护配置在发包前
固定选择；对端没有协商或切换 profile 的权力。客户端收到非预期 profile、TLS
版本、ALPN 或 cipher suite 时必须关闭 Connection，绝不能尝试另一 profile 或
明文回退。

网络 endpoint 与服务身份必须分离。客户端可以向一个 IP 地址发 UDP 包，但仍
用预先配置的 DNS 名称发送 SNI 并校验证书 SAN；DNS 不参与本次连接也不影响该
验证。没有可用名称时，只有两种安全选择：证书含有与目标 IP 精确匹配的
`iPAddress` SAN，或客户端通过受保护配置预置 NTRS 的 SPKI/certificate pin。
IP、首次从该 IP 收到的自签名证书，或同一未认证 resolver 返回的指纹，都不能
成为信任根。服务证书轮换时应配置当前和下一把 pin，或使用受信 CA 链。

当前随库构建的 BoringSSL 已提供 `SSL_QUIC_METHOD`、
`SSL_set_quic_method()`、`SSL_provide_quic_data()`、
`SSL_set1_host()` 和 `SSL_set_verify()`。NTRS Connection 的实现应按下列步骤
集成这些接口：

1. Initial/Handshake 包中的 CRYPTO 帧承载 TLS handshake bytes，支持 offset、
   重组和可靠重传；现有只承载 32 字节 X25519 公钥的 `FrameCrypto` 不能复用。
2. BoringSSL 的 `add_handshake_data` 回调将 TLS 输出放入对应加密级别的 CRYPTO
   帧；收到 CRYPTO 帧后以 `SSL_provide_quic_data` 输入 TLS 状态机。
3. Initial key 由传输层按协议版本和 DCID 独立派生；`set_encryption_secrets`
   回调安装 TLS 导出的 Handshake 和 1-RTT 方向 traffic secret。libutp 从这些
   secret 派生各级别的 AEAD key、IV/nonce 与所需的包头保护材料，并替换当前
   未认证的自定义 X25519 traffic key schedule。
4. 客户端设置 ALPN `libutp-ntrs/1` 与 `SSL_VERIFY_PEER`。若身份是 DNS 名称，
   则设置 SNI 和 `SSL_set1_host("ntrs.example.com")`；若身份是 IP SAN，则做
   精确 IP SAN 校验；若使用 pin，则在链验证后额外校验 SPKI。无论哪种方式都
   必须配置受信 CA store 或受保护的 pin。
5. NTRS 配置完整 X.509 链和私钥，限定 TLS 1.3；`SSL_is_init_finished()` 与 TLS
   Finished 成功前，Connection 不得传递任何注册、心跳、目录或会话信令。

TLS 服务器认证后，现有 bootstrap/access token 仍用于“哪个 peer 有权执行哪个
NTRS 操作”，但不再承担抗中间人职责。需要 NTRS 强认证客户端时可再增加 mTLS；
首期可继续把短期 access token 放在已认证的 TLS 1.3 Connection 内。

这与“在 stream 0 上运行 TLS”不同：后者只保护 stream 0 的字节，其他包和
stream 仍由原有密钥模型处理。使用 QUIC TLS 回调导出的 traffic secret 后，
libutp 的整个 Connection 在包级别受同一次 TLS 认证和密钥阶段保护，才等价于
QUIC 的安全边界。TLS 1.3 encrypted 0-RTT 在重放模型完成前保持禁用。

### 8.3.2 轻量 NTRS 认证配置文件

受控部署可不用 X.509，而使用 Ed25519 服务信任根实现服务端认证。`service_id`
是固定的逻辑服务/集群身份，例如 `ntrs-prod-cn`；它不随 DNS 回答、IP 或具体
服务节点变化。客户端通过安装包、受保护配置或已有认证通道预置：

```text
service_id, root_key_id, root_ed25519_public_key, next_root_pin (可选)
```

该信任根绝不能在首次连接时从同一个未认证 IP 或 resolver 获取。DNS 只用于
获得网络 endpoint；它不提供、也不决定服务身份。

这里的 root 是部署期或离线签发根，不是运行期 Hub，也不转发任何控制或业务
数据。最简单的多服务部署让所有受管 NTRS 服务共享一个全局 NTRS root public
key pin；root private key 只由发布/运维系统保管，用于给 `NTRS-A`、`NTRS-B`
各自的节点签发证书。若不同 NTRS 服务属于不同运营方，则应用还需预置一个
目录 root public key，由已认证的 peer 目录签发服务信任记录：

```text
PeerServiceRecord
  peer_id, expected_service_id, ntrs_domain_or_ip,
  service_root_key_id, service_root_public_key, expire_at,
  directory_signature
```

A 在连接 B 前先验证 `PeerServiceRecord` 的 directory signature，再把记录中的
`expected_service_id` 和 service root 作为本次连接的固定期望。未认证 resolver
或 DNS 即使将连接指向攻击者 IP，也不能替换这些期望值。

Context 应提供运行期更新 NTRS trust anchor 集合的接口，而不是只暴露一个不可
轮换的 root public key。接口语义应是原子地替换整个集合，并要求单调递增版本：

```text
setNtrsTrustAnchors(generation, anchors[])
  anchor = { root_key_id, ed25519_public_key }
```

每个新 ConnectAttempt 获取一个不可变的 trust snapshot。应用只能通过安装包
更新、受保护远程配置或其他独立已认证通道调用该接口；禁止从正在验证的 NTRS
Connection 中接受新的 root。正常轮换先发布 `{old_root, new_root}`，再部署由
new root 签发的服务密钥，最后在宽限期后删除 old root。若 old root 私钥疑似
泄露，应用必须从独立通道紧急替换为 `{new_root}`，并关闭所有只由 old root
认证的 NTRS Connection 后重新握手。

每个 NTRS 节点使用独立的 Ed25519 node signing key，而不是让全部节点共享根
私钥。节点在握手中携带一个由根签发的轻量证书：

```text
NodeCertificate
  service_id, node_id, node_key_id, node_ed25519_public_key,
  not_before, not_after,
  root_signature = Ed25519_Sign(root_private, canonical(fields above))
```

客户端先验证证书中的 `service_id` 等于本地期望值、有效期和 root signature，
再使用 node public key 验证 ServerHello。`node_key_id` 是可轮换、可按节点不同
的提示字段，永远不能替代 root pin。

```text
ClientHello (明文，本地创建后不可变)
  protocol_version, profile, service_id, client_cid,
  client_eph_x25519_pub, nonce_c, cipher_suite

ServerHelloUnsigned (明文)
  service_id, profile, selected_cipher_suite, NodeCertificate,
  server_eph_x25519_pub, nonce_s

ServerHello
  ServerHelloUnsigned,
  server_signature = Ed25519_Sign_Pure(node_private, h_server_signature)
```

所有 `canonical(...)` 使用唯一确定的二进制编码，所有哈希均为 SHA-256。Ed25519
使用 pure Ed25519，签名消息精确为下列 32 字节 hash，而不是 Ed25519ph：

```text
CH = 客户端本地保存的 canonical(ClientHello)
SHu = canonical(ServerHelloUnsigned)
SH = canonical(ServerHello)

h_server_signature = SHA-256("libutp-ntrs-server-signature-v1" || CH || SHu)
h_client_finished = SHA-256("libutp-ntrs-client-finished-v1" || CH || SH)
```

**客户端验签必须使用本地保存的 `CH` 原始字节重建 `h_server_signature`；服务端
不得回显 ClientHello，客户端也不得接受任何回显副本作为验签输入。** 这是服务端
签名绑定客户端临时 X25519 公钥、随机数、CID、profile、cipher suite 和服务身份
的必要条件。

双方计算 `dh = X25519(local_eph_private, peer_eph_public)`，拒绝全零结果，并使用
有意选择的 channel-binding KDF：

```text
salt = SHA-256("libutp-ntrs-kdf-v1" || h_client_finished)
prk = HKDF-Extract-SHA256(salt, dh)
finished_key_c = HKDF-Expand-SHA256(prk, "libutp-ntrs-finished-c-v1" || h_client_finished, 32)
finished_key_s = HKDF-Expand-SHA256(prk, "libutp-ntrs-finished-s-v1" || h_client_finished, 32)
```

directional AEAD key 与 nonce base 使用同一 `prk`、不同固定 label 和
`h_client_finished` 派生。客户端要求 ServerHello 中的 profile 和
`selected_cipher_suite` 等于本地预期值。Finished 按严格顺序绑定：

```text
FinishedC = HMAC-SHA256(finished_key_c, h_client_finished)
h_server_finished = SHA-256("libutp-ntrs-server-finished-v1" || CH || SH || canonical(FinishedC))
FinishedS = HMAC-SHA256(finished_key_s, h_server_finished)
```

服务端验证 FinishedC 后才发送 FinishedS；客户端验证 FinishedS 后才建立
Connection。这样服务端 Finished 覆盖客户端 Finished，双方 Finished 都绑定完整
ServerHello 签名，不存在同一 transcript 上的可交换确认。任何签名、profile、
cipher suite、HMAC、nonce、顺序或有效期错误都关闭 Connection，控制消息不得
在双向 Finished 前处理。

Ed25519 私钥只用于签名，X25519 临时私钥每个 Connection 新建并在结束时清零。
客户端认证不是防服务端中间人的前提，可在握手完成后用已有 access token；若
NTRS 也需要在密码学上认证客户端，再给 ClientHello 增加客户端 Ed25519 签名。
首期应保持单向服务端认证，避免无必要的设备密钥管理。

若所有 DNS 节点确实属于同一 NTRS 服务集群，它们共享同一个 `service_id` 和
root public key，但可各自持有 node key；DNS 被篡改到攻击者 IP 时，攻击者无法
提供该 root 签发的 NodeCertificate，握手失败。互不联通的 NTRS 服务必须使用
不同 `service_id` 和不同 root pin，Scheduler assignment 从已认证的 Scheduler
控制连接下发“目标 endpoint + 期望 service_id”，客户端据此选择 trust anchor。

该配置文件比 TLS 1.3 少了 X.509 生态、通用算法协商和标准会话恢复，但在 root
pin、节点证书、ServerHello 签名和 Finished 均严格校验的前提下，足以防止针对
NTRS 的主动中间人攻击。它与 TLS 1.3 profile 二选一：配置了任一 profile 后，
验证失败必须关闭 Connection，绝不能退回明文或另一 profile。根和 node key 的
轮换分别通过当前/下一把 root pin 与带 key_id/有效期的 NodeCertificate 完成。

### 8.4 路径协调与状态机

每次逻辑 connect 创建一个不可复用的 `ConnectAttempt`。其状态机如下：

```text
Assigned
  -> AcquiringControlSession       (按需连接目标 NTRS)
  -> AwaitingRendezvousSignal
  -> Punching                      (并发候选和 warmup)
  -> TransportHandshaking          (直连 / 正向 / 反向 Initial)
  -> IdentityHandshaking           (仅 ENCRYPTED)
  -> Connected | Failed | Cancelled
```

`DIRECT_ONLY` 从 `Assigned` 直接进入 `TransportHandshaking`。`AUTO` 中的直连
分支和 P2P 分支是同一个 `ConnectAttempt` 的子尝试，不能创建两个对应用可见
的 Connection。

NTRS 指示由 B 承担 transport initiator 时，A 必须预先建立一个有限期的
`PendingP2pAttempt`。B 的 Initial 到达 A 后，只有满足以下全部条件才提升为
A 所执行的原始 Scheduler assignment：

1. 源地址属于该 attempt 已验证或正在探测的候选集合；
2. 在握手扩展中携带匹配的 `session_id` 与 `session_binding_hash`；
3. 未过期、未被其他尝试消费，且逻辑 peer 绑定匹配；
4. 后续 libutp 握手完成。

不匹配的 Initial 必须继续走普通被动连接的 `OnNewConnection/accept` 路径。
匹配只证明它是同一次协调尝试，不能证明发送者身份。

`session_binding_hash` 使用下列确定性二进制字段计算，原始
`peer_session_token` 只保留在 NTRS/PUNCH 层：

```text
SHA-256(
  "libutp-p2p-binding-v1" ||
  session_id ||
  SHA-256(peer_session_token) ||
  logical_initiator_node_ref || logical_responder_node_ref ||
  expire_at
)
```

它防止无关 UDP Initial 被错误提升，并为后续加密握手提供可绑定的会话上下文；
不能抵御能观察或篡改 UDP 的主动攻击者。

候选尝试按 host-local、srflx primary、srflx secondary 以及 NTRS 给出的
warmup 策略并发执行。首个 `PUNCH_ACK` 只表示候选路径可能可达，不能结束
attempt。首个完成完整 transport/identity 握手的路径才获胜；获胜后必须取消
其余探测、关闭临时 CIDs、删除 pending 表项和定时器。每个 attempt 需设候选
数、并发包数、总超时、每源地址 pending 数和全局 pending 数上限。

### 8.5 直连与 P2P 的安全契约

“能连上”与“确认对方是谁”是两个独立属性。直连 endpoint、候选地址、NTRS
session token 或 PUNCH_ACK 都不能证明对端身份。

| 调用条件 | 行为 | 对抗能力 |
| --- | --- | --- |
| `PLAINTEXT` | 不执行 AEAD 或身份握手 | 仅可达性；路径攻击者可观察、篡改业务数据，并可劫持 rendezvous 提升，将自身 Initial 关联为原逻辑连接 |
| `ENCRYPTED`，无身份材料 | 临时 X25519 + HKDF + AEAD + Finished | 保护正常路径上的数据机密性和完整性；不能抵抗主动中间人替换临时公钥 |
| `ENCRYPTED`，提供目录/预置信任公钥 | 额外验证目录和 Ed25519 签名 | 同时验证对端身份，抵抗主动中间人和降级 |

这不是对外额外暴露的 `authenticated/unauthenticated` 状态。调用方只选择
明文或加密，并可选择提供身份材料；但一旦提供身份材料，验证是强制的：目录
签名、有效期、撤销状态、peer id/device id/key id 或对端签名任一失败，必须以
身份错误关闭连接，绝不能回退为无身份的加密或明文。

调用方选择 `ENCRYPTED` 时，Context 必须只接受加密握手和加密数据包；任何
明文 Initial、未协商 AEAD 或被剥离的加密参数都导致失败关闭，不能因可达性
压力回退为 `PLAINTEXT`。

NTRS 使用域名和 X.509 证书只能保护应用到 NTRS 的控制通道。它不能自动保护
NTRS 分发候选后发生的 peer-to-peer UDP 直连。若要让直连本身具有身份安全，
必须使用可信目录/预置公钥上的 Ed25519 验签，或另行实现等价的 peer 证书握手。

### 8.6 加密 Connection 握手

现有临时 X25519 与方向隔离 AEAD 可复用，但 `ENCRYPTED` 的完整握手必须在
Connection 级完成，而不是在某个 stream 内叠加 TLS。为与当前 P2P 设计绑定，
新增或扩展的 Initial/Handshake/Finished 记录至少包含：

```text
protocol_version
security_mode
logical_initiator_node_ref, logical_responder_node_ref
transport_initiator_role
session_id, session_binding_hash       (P2P 时；直连时显式 absent)
双方 device_id, key_id                 (提供目录时)
双方 CID
aEphPub, bEphPub
nonceA, nonceB
```

签名和 HKDF 使用同一个确定性 transcript，算法固定为 SHA-256、Ed25519 pure、
HKDF-SHA256 与 HMAC-SHA256：

```text
transcript_hash = SHA-256(canonical_handshake_record)
salt = SHA-256("libutp-handshake-v1" || transcript_hash)
prk = HKDF-Extract-SHA256(salt, X25519_shared_secret)
```

提供目录或预置信任公钥时，`canonical_handshake_record` 必须包含已验证的对端
Ed25519 签名及其完整身份字段；未提供身份材料时，记录必须编码明确的
`identity_proof = absent`，不能省略该字段或伪造空签名。该 KDF 是有意选择的
transcript-in-salt channel binding，不是 TLS 1.3 的
`Extract(0, DH) + Expand-Label(transcript)` 结构；所有实现必须严格使用上述
写法。HKDF expand 必须用不同 label 派生两个方向的 AEAD key、nonce base 以及
Finished key；Finished 的顺序和 hash 定义必须遵循 §8.3.2 的
`h_client_finished -> FinishedC -> h_server_finished -> FinishedS`，仅替换双方
角色 label。双方交换并验证 Finished 之前，不得交付业务数据、触发
`Connected` 或保存可恢复状态。临时私钥、shared secret、Finished key 和中间
HKDF 输出在成功、超时、取消和连接关闭路径都必须清零。

P2P 绑定信息必须进入 transcript，因而攻击者不能把一个已签名的正向握手重放
到反向建连、另一个 session 或另一个逻辑 peer。接收端还必须拒绝全零共享
秘密、过期 timestamp、重复 nonce/session、错误的角色以及重复 Finished。

### 8.7 分阶段交付与验收

1. **NTRS adapter 与可达性**：实现已认证的 NTRS 控制会话（TLS profile 或选定的
   轻量 profile）、当前私有探测协议、候选 normalizer、`ConnectAttempt` 与正反向
   Initial 关联；首先支持明文 P2P。
2. **统一路由**：实现 `AUTO/DIRECT_ONLY/P2P_REQUIRED` 的单一 connect 请求，
   支持直连与 P2P 竞速、单次回调、取消和资源上限。
3. **安全握手**：实现 transcript、Ed25519 目录验证、X25519/HKDF、Finished、
   密钥清零与身份失败错误。无目录的加密路径保持可用；有目录时严格验证。
4. **控制面迁移评审**：只有 libutp 已具备等价的 Connection 级认证、重连和
   密钥更新能力后，才评估用它承载 NTRS 控制面；在此之前维持 TLS 控制通道。

每阶段至少覆盖以下回归用例：

- NTRS 报文金样与真实 NTRS 的 AUTH/REGISTER/SESSION/PUNCH 互操作；
- 同一 Scheduler assignment 的正向与反向 Initial 都只产生一次成功事件；
- `AUTO` 直连和打洞并发时，只有完整握手的首个路径胜出且其余资源被释放；
- Full Cone、地址限制、端口限制、映射变化、超时、候选洪泛和对称 NAT 失败；
- ServerHello 必须使用客户端本地保存的 ClientHello 字节验签；针对被替换的
  ClientHello、伪造回显或按不同 ClientHello 生成的签名均须拒绝；
- FinishedC 篡改、重放、抢先到达、与另一 Connection 交换，以及 FinishedS 未覆盖
  已验证 FinishedC 的情形均须拒绝；
- 篡改候选绑定、session binding、身份目录、签名、AAD、密文和 nonce/counter 的
  拒绝行为；
- 预期 TLS/轻量 profile、TLS 版本、ALPN、cipher suite 或 `security_mode` 被替换、
  剥离或降级为 PLAINTEXT 时必须关闭，不能自动重试到另一安全模式；
- 目录缺失时的明文与加密连接可用，目录存在但无效时严格失败；
- 所有日志、错误字符串、抓包辅助和指标均不包含 private key、shared secret、
  traffic key、peer session token 或业务明文。
