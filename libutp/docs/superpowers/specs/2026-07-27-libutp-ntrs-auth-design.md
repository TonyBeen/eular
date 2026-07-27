# libutp NTRS 服务端认证设计(自签 Ed25519 根)

- 日期:2026-07-27
- 状态:初稿(基于 nat.md §8.3.2 + 首轮审核修正 + 快可达设计的认证边界厘清)
- 关系:punch/fast-connect spec 的**前置**;peer 身份/抗 MITM 仍归 crypto spec

---

## 1. 目标与范围

**目标**:让**节点认证它的 home NTRS**,防 DNS 投毒 / 网络 MITM 冒充 NTRS 控制 rendezvous。用**离线自签 Ed25519 根** + 每节点 NodeCertificate + 签名握手 + Finished,认证整条 node↔home-NtrsA 的 utp 控制连接。

**范围内**
- **仅 home NtrsA↔节点**(有 keepalive 的控制连接)的**单向服务端认证**(节点验 NtrsA)。
- 自签 Ed25519 信任根、NodeCertificate、握手签名、双向 Finished(key confirmation)。
- **根证书轮换**(泄漏可换)。

**范围外(明确)**
- **跨服务 A→NtrsB(单包 CONNECT)不认证**——靠 `rendezvous_id` + M1/M2/M3(punch spec §3/§12),本 spec 不覆盖。
- **peer↔peer 身份 / 抗主动 MITM** → crypto spec。
- **客户端认证**(NtrsA 认证节点):首期只单向;NtrsA 认证节点靠已认证信道内的 access 凭据,或后补 mTLS/客户端签名(§8)。

**为什么单向优先**:NTRS 是固定已知服务,节点验它成本低(一个 root pin)、收益高;双向要设备密钥管理,**节点数量大,双向浪费资源和时间**,首期不必。

**安全定位(重要)**:**P2P 只需弱安全**——不追求 TLS/X.509 级别的强保证。故:只做自签 Ed25519 profile(**不支持 TLS**,生态过重、不适合 P2P);只单向;跨 NtrsB / peer 身份等更强的保证按需下沉到各自 spec。本 spec 的目标是"以最小代价挡住冒充 home NTRS"。

---

## 2. 信任根与轮换

### 2.1 预置信任锚(root pins)
客户端通过**安装包 / 受保护配置 / 其他已认证通道**预置(**绝不能**在首次连接时从被认证的连接、DNS 或未认证 resolver 获取):

```text
service_id                    // 固定逻辑服务/集群身份,如 "ntrs-prod-cn";不随 DNS/IP/节点变
root_key_id
root_ed25519_public_key
next_root_pin (可选)
```

- `service_id` 是本次连接的**固定期望**;DNS 只用于拿网络 endpoint,不提供也不决定服务身份。
- root 是**部署期/离线签发根**,不是运行期 Hub,不转发任何控制/业务数据。
- 多服务:互不联通的 NTRS 用**不同 `service_id` + 不同 root pin**;节点连哪个服务用哪把 root。

### 2.2 运行期更新接口
```text
setNtrsTrustAnchors(generation, anchors[])    anchor = { root_key_id, ed25519_public_key }
```
- **原子替换整个集合**,要求 `generation` 单调递增。
- 每个新连接尝试取一份**不可变 trust snapshot**。
- **只能**经安装包更新 / 受保护远程配置 / 独立已认证通道调用;**禁止**从正在验证的 NTRS 连接接受新 root。

### 2.3 轮换流程
- **正常**:先发布 `{old_root, new_root}` → 部署由 new_root 签发的节点密钥 → 宽限期后删 old_root。
- **紧急(old 私钥疑似泄漏)**:从独立通道立即切 `{new_root}`,并**关闭所有只由 old_root 认证的 NTRS 连接**后重握手。

### 2.4 NodeCertificate(每 NTRS 节点,root 签发)
每个 NTRS 节点持独立 Ed25519 node signing key,握手中携带 root 签发的轻量证书:
```text
NodeCertificate {
  service_id, node_id, node_key_id, node_ed25519_public_key,
  not_before, not_after,
  root_signature = Ed25519_Sign_Pure(root_private, canonical(上述字段))
}
```
- `node_key_id` 是可轮换、可按节点不同的**提示**字段,**永不替代 root pin**。
- 节点全部共享同一 `service_id` + root pin,但各持不同 node key;DNS 被篡改到攻击者 IP 时,攻击者拿不到 root 签发的 NodeCertificate → 握手失败。

---

## 3. 握手(叠加在 node↔NtrsA 的加密 utp 连接上)

**算法固定**:SHA-256、**pure Ed25519**(对 32B 预哈希签名,非 Ed25519ph)、HKDF-SHA256、HMAC-SHA256。所有 `canonical(...)` 用唯一确定的二进制编码。

### 3.1 消息(映射到 utp 握手包)
```text
ClientHello (Initial, node→NtrsA, 明文,本地保存不可变)
  protocol_version, profile, service_id, client_cid,
  client_eph_x25519_pub, nonce_c, cipher_suite

ServerHelloUnsigned (Handshake, NtrsA→node, 明文)
  service_id, profile, selected_cipher_suite, NodeCertificate,
  server_eph_x25519_pub, nonce_s
ServerHello = ServerHelloUnsigned ‖ server_signature
  server_signature = Ed25519_Sign_Pure(node_private, h_server_signature)

FinishedC (HandshakeDone 同程, node→NtrsA)   = HMAC-SHA256(finished_key_c, h_client_finished)
FinishedS (NtrsA→node)                        = HMAC-SHA256(finished_key_s, h_server_finished)
```

### 3.2 哈希定义(精确,评审修正)
```text
CH  = 节点本地保存的 canonical(ClientHello)
SHu = canonical(ServerHelloUnsigned)
SH  = canonical(ServerHello)

h_server_signature = SHA256("libutp-ntrs-server-signature-v1" ‖ CH ‖ SHu)
h_client_finished  = SHA256("libutp-ntrs-client-finished-v1"  ‖ CH ‖ SH)
h_server_finished  = SHA256("libutp-ntrs-server-finished-v1"  ‖ CH ‖ SH ‖ canonical(FinishedC))
```

### 3.3 验证规则(安全支点,逐条 MUST)
1. **节点验签必须用本地保存的 `CH` 原始字节重建 `h_server_signature`**;NtrsA **不得回显** ClientHello,节点**不得**接受任何回显副本作为验签输入。——这是"服务端签名绑定客户端临时公钥/随机数/CID/profile/cipher/服务身份"的必要条件,防 MITM 替换。
2. 节点先验 **NodeCertificate**:`service_id == 本地期望`、有效期(`not_before/not_after`)、`root_signature` 由预置 root pin 验过;再用 `node_ed25519_public_key` 验 ServerHello 的 `server_signature`。
3. 节点要求 ServerHello 的 `profile`、`selected_cipher_suite` == 本地预期值。
4. **profile 由受保护配置在发包前固定选定(当前只有 Ed25519 一种),对端无协商/切换权**;收到非预期 profile/cipher → 关闭,**绝不明文回退**。

### 3.4 密钥派生(channel-binding)
```text
dh = X25519(local_eph_private, peer_eph_public)   // 拒绝全零结果
salt = SHA256("libutp-ntrs-kdf-v1" ‖ h_client_finished)
prk  = HKDF-Extract-SHA256(salt, dh)
finished_key_c = HKDF-Expand-SHA256(prk, "libutp-ntrs-finished-c-v1" ‖ h_client_finished, 32)
finished_key_s = HKDF-Expand-SHA256(prk, "libutp-ntrs-finished-s-v1" ‖ h_client_finished, 32)
// directional AEAD key / nonce base 用同一 prk、不同固定 label + h_client_finished 派生
```
- 有意选择的 **transcript-in-salt channel binding**;与现有 utp traffic key schedule(`libutp-handshake-v2`)是不同 label 体系,两者独立。

### 3.5 Finished 顺序(严格)
- **NtrsA 先验 FinishedC 才发 FinishedS**;节点验 FinishedS 才认为认证完成。
- FinishedS 覆盖 `canonical(FinishedC)`(见 h_server_finished)→ 服务端 Finished 绑定客户端 Finished,无同一 transcript 上的可交换确认。
- 任何签名 / profile / cipher / HMAC / nonce / 顺序 / 有效期错误 → 关闭连接;**双向 Finished 完成前,NTRS 控制消息(注册/心跳/候选/目录)一律不处理**。

### 3.6 密钥清除
Ed25519 私钥只用于签名;X25519 临时私钥每连接新建、结束清零;shared secret / Finished key / 中间 HKDF 输出在成功/失败/超时/关闭路径都清零。

---

## 4. 与现有 utp 集成

- node↔NtrsA 是一条 **utp 连接,MUST 用 ENCRYPTED 模式**(复用现有 X25519+HKDF+AES-GCM 数据面,utp-10),NTRS 认证是**叠加在其握手上的服务端认证层**。
- 消息映射:ClientHello=Initial、ServerHello=Handshake、FinishedC 随 HandshakeDone、FinishedS 为 NtrsA→node 的一个控制帧。
- **新增帧(定案)**:X25519 继续走现有 `kFrameCrypto`(不动),**NodeCertificate/签名/Finished 走新增专用帧**。理由:`kFrameCrypto` 在**直连/打洞/NTRS 所有加密连接共用**,改其格式波及全局;而 NTRS 认证材料**只在直连 NTRS 连接出现**(打洞路径没有),新增帧最干净、隔离。
  - `kFrameNtrsClientHello`(service_id/profile/nonce_c/cipher;client_eph 复用现有 `kFrameCrypto` 的 32B X25519)
  - `kFrameNtrsServerAuth`(NodeCertificate + server_signature + nonce_s)
  - `kFrameNtrsFinished`(FinishedC / FinishedS,方向区分)
- **transport connected(收到 HandshakeDone,C2=A)≠ ntrs-auth-complete(双向 Finished 验过)**:控制消息只在 ntrs-auth-complete 后流动。
- **现有 `kFrameCrypto` 只承载 32B X25519 公钥**,不足以承载 NodeCertificate/签名——故上述新帧,不复用/不扩展 kFrameCrypto 的语义(避免与 punch 的 X25519 复用冲突)。

---

## 5. 失败与错误码

- 验证失败(证书/签名/profile/cipher/Finished/有效期/root 不匹配)→ 以**明确错误码**关闭连接,**不回退明文/不换 profile**。
- 建议错误码(负值,遵循全局约定 C5):`kNtrsCertInvalid`、`kNtrsRootUntrusted`、`kNtrsServerSigInvalid`、`kNtrsProfileMismatch`、`kNtrsFinishedInvalid`、`kNtrsServiceIdMismatch`、`kNtrsCertExpired`。
- root 轮换紧急切换后,只由 old_root 认证的连接 MUST 关闭重握手(§2.3)。

---

## 6. 边界与非目标(诚实声明)

- **跨服务 A→NtrsB 单包 CONNECT 不认证**:本 spec 不覆盖;其 MITM 由 punch 的 rendezvous_id + DoS 有界化,不是消除。
- **peer↔peer 身份/抗 MITM**:crypto spec。
- **DNS/IP 不是信任根**:名字只用于拿 endpoint;信任只来自预置 root pin + `service_id` 期望。
- **客户端认证首期不做**:节点验 NtrsA 是单向;NtrsA 认证节点靠已认证信道内 access 凭据(见 §8)。
- **不支持 TLS 1.3 profile**:TLS/X.509 生态过重,不适合 P2P;**P2P 只需弱安全**——本 spec 只做自签 Ed25519 profile。验证失败必须关闭,**绝不退回明文**。

---

## 7. 参数(建议默认,可调)

| 参数 | 建议 |
|---|---|
| 签名算法 | pure Ed25519(over 32B SHA-256 hash) |
| Hash / KDF / MAC | SHA-256 / HKDF-SHA256 / HMAC-SHA256 |
| cipher_suite(数据面) | 对齐 utp-10:AES-128/256-GCM |
| NodeCertificate 有效期 | 部署定(建议天/周级),支持 not_before/not_after |
| root 轮换宽限期 | 部署定(建议覆盖一个证书有效期) |
| profile | 单一固定(自签 Ed25519),受保护配置选定 |

---

## 8. 客户端认证(后续,预留)

- 首期单向:节点验 NtrsA。NtrsA 需认证节点时,先在已认证的 NTRS 连接内用 **access 凭据**(短期、绑 pid);要密码学强认证再给 ClientHello 增加**客户端 Ed25519 签名**(mTLS 式)。
- 这与 punch 的 DoS 无关:DoS(M1/M2/M3)防的是资源/反射,客户端认证防的是"谁有权注册/操作 NTRS"。

---

## 9. 测试与验收

- **信任根**:未预置 root / root 不匹配 / 过期 NodeCertificate / 错 `service_id` → 全部失败关闭。
- **MITM**:篡改 ClientHello 字段后验签(节点用本地 CH 验)必须失败;NodeCertificate 被换、签名按不同 ClientHello 生成 → 拒。
- **Finished**:FinishedC 篡改/重放/抢先/与另一连接交换,FinishedS 未覆盖已验 FinishedC → 拒。
- **降级**:profile/cipher 被替换/剥离/降明文 → 关闭,不自动重试到另一模式。
- **轮换**:`{old,new}` 并存期新旧节点密钥都能连;紧急切 `{new}` 后 old 认证连接被关。
- **DNS 投毒**:DNS 指向攻击者 IP,攻击者无 root 签发 NodeCertificate → 握手失败。
- **不泄露**:日志/抓包/指标不含 root/node private key、shared secret、Finished key。
