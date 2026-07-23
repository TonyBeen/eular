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

## 7. NTRS 兼容穿透设计记录（待定，未实现）

本节记录 2026-07 的讨论结论，作为后续设计输入，不代表已确定的 API 或协议实现。

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

### 7.7 备选：Connection 级身份认证（暂不作为 P2P 前置实现）

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

若采用该备选，需要新增身份/签名和 Finished 帧、握手重传与超时状态、证书或目录验证接口、错误码、资源上限以及篡改/降级/重放回归测试。首期 P2P 实现不得依赖这些改动才能完成可达路径上的连接。
