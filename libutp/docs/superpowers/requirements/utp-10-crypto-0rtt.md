# utp-10 加密 / 密钥调度 / 会话票据 / 0-RTT / 重放过滤

> 本文档由现有 C++ 实现（`cpp/`）反推得到。**代码是唯一 ground truth**；凡与 `doc/` 方案不一致，以代码为准并在 §8 标注差异。
> 引用格式：`文件:行号`。数值均来自代码；无法从代码确定处标注“待确认”。

---

## 1. 职责与边界

本模块负责 libutp 数据面与握手的密码学原语与会话恢复机制，覆盖：

1. **X25519 密钥交换**（`X25519Wrapper`）：临时密钥对生成、共享密钥派生。
2. **方向隔离的流量密钥调度**（`TrafficKeySchedule`）：由 X25519 共享密钥 + 握手转录 HKDF 派生 client→server / server→client 两套 (key, nonce_prefix)。
3. **方向隔离的报文 AEAD**（`AesGcmContext`）：AES-128/256-GCM，每方向一个 tx/rx 上下文，nonce = 4B prefix ‖ 8B packet number。
4. **无状态会话票据**（`TokenAuth` + `TokenMeta`）：服务端用轮换对称密钥 seal/open 的路径校验票据与 0-RTT 恢复票据。
5. **会话恢复状态编解码**（`ResumptionStateCodec` + `Base64`）：客户端可导出/导入的加密恢复串。
6. **0-RTT 决策与抗重放**（`ContextImpl` 内）：票据校验、地址绑定校验、时效校验、`(ticketCid, nonce)` 重放窗口过滤、统计。

边界说明：
- 本模块**不做** CID 混淆 / 全包加密 / 无状态可验证 CID（那是 `doc/` 里的未实现方案，见 §8）。
- 报文头（20 字节）始终明文，仅作为 AEAD 的 AAD；只有 payload 被加密。见 `cpp/src/crypto/aes_gcm_context.cpp:229`（AAD 长度 = `UTP_HEADER_SIZE`）。
- 报文的加密/解密调度由连接层（`ConnectionImpl`）驱动，本模块提供原语。

---

## 2. 密钥体系 / 数据结构 / token 格式

### 2.1 X25519（`cpp/src/crypto/x25519_wrapper.h`）
- `PRIVATE_KEY_SIZE = 32`、`PUBLIC_KEY_SIZE = 32`、`SHARED_SECRET_SIZE = 32`（`x25519_wrapper.h:32-34`）。
- 类型：`PrivateKey`/`PublicKey`/`SharedSecret`（32B），`SharedSecretShort`（16B，`SHARED_SECRET_SIZE/2`，`x25519_wrapper.h:39`）。
- 优先使用 BoringSSL `curve25519.h`（`X25519_keypair` / `X25519`），否则回退 OpenSSL EVP（`x25519_wrapper.h:15-21`，`x25519_wrapper.cpp:49-79`）。
- 析构时用 volatile 循环清零私钥（`x25519_wrapper.h:54-61`，`x25519_wrapper.cpp:91-94`）。
- 全零共享密钥视为错误抛异常（`x25519_wrapper.cpp:143-149`）。

### 2.2 流量密钥调度（`cpp/src/crypto/traffic_key_schedule.h`）
- `MAX_KEY_SIZE = 32`、`NONCE_PREFIX_SIZE = 4`（`traffic_key_schedule.h:23-24`）。
- `struct Secret { key[32]; noncePrefix[4]; }`；`struct Material { Secret clientToServer; Secret serverToClient; size_t keySize; }`（`traffic_key_schedule.h:26-35`）。
- 派生标签（`traffic_key_schedule.cpp:24-25`）：
  - salt label：`"libutp-handshake-v2"`
  - info label：`"libutp-traffic-keys-v2"`
- 转录（transcript）结构，固定 `4+4+4+1+32+32 = 77` 字节（`traffic_key_schedule.cpp:60-72`）：
  `BE32(UTP_PROTOCOL_VERSION=2) ‖ BE32(clientCid) ‖ BE32(serverCid) ‖ cryptoType(1B) ‖ clientPubKey(32) ‖ serverPubKey(32)`。

### 2.3 报文 AEAD（`cpp/src/crypto/aes_gcm_context.h`）
- `GCM_TAG_SIZE = 16`、`GCM_NONCE_SIZE = 12`（`aes_gcm_context.h:29-30`）。
- key 类型：`AesKey128`（16B）、`AesKey256`（32B）；`NoncePrefix`（4B）；`Nonce`（12B）（`aes_gcm_context.h:33-36`）。
- nonce 构造：`prefix(4B) ‖ BE64(counter)`，counter 为 packet number（`aes_gcm_context.cpp:560-568`）。
- 线程本地加密缓冲池，size class = `{1280, 1500, 4096, 9000, 65535}`，每类缓存上限 `kEncryptBufferCacheLimitPerClass = 8`（`aes_gcm_context.cpp:29-37`,`:98-111`,`:613`）。

### 2.4 Token（`cpp/src/crypto/token.h`）
- `AEAD_KEY_SIZE = 32`、`AEAD_NONCE_SIZE = 12`、`AEAD_TAG_SIZE = 16`（`token.h:39-41`）。
- `enum class TokenType : uint8_t { kPathValidation = 1, kZeroRttResumption = 2 }`（`token.h:45-48`）。
- `struct TokenMeta`（`token.h:50-66`），字段与线序编码（`token.cpp:209-221`）：

  | 字段 | 类型 | 字节 |
  |---|---|---|
  | token_type | uint8 | 1 |
  | timestamp（unix 秒） | uint32 | 4 |
  | cid（connection id） | uint32 | 4 |
  | encryption_mode（`Context::EncryptionMode`，0=None） | uint8 | 1 |
  | version（peer utp version） | uint32 | 4 |
  | secret（local secret number） | uint32 | 4 |
  | family（peer 地址族） | uint16 | 2 |
  | host（union in_addr/in6_addr） | 16B | 16 |

  合计 `TOKEN_META_SIZE = 36` 字节（`token.h:68`）。
- **TOKEN 线格式**：`nonce(12) ‖ ciphertext(36) ‖ tag(16)`，`TOKEN_SIZE = AEAD_NONCE_SIZE + TOKEN_META_SIZE + AEAD_TAG_SIZE = 64` 字节（`token.h:72-73`）。
- AEAD 为 AES-256-GCM（`token.cpp:22-26`）；AAD 按类型区分（`token.cpp:28-34`）：
  - `TOKEN_AAD_PATH = "UTP-PathToken-AAD"`（kPathValidation）
  - `TOKEN_AAD_0RTT = "UTP-0RTTToken-AAD"`（kZeroRttResumption）
- 密钥轮换：`TOKEN_KEY_UPDATE_INTERVAL_MS = 60*60*1000`（1 小时，`token.cpp:15-17`,`:38-39`）；持有 `m_key`+`m_oldKey`，`open()` 先试 `m_key` 再试 `m_oldKey`（`token.cpp:186-195`）。

### 2.5 会话恢复状态（`cpp/src/crypto/resumption_state_codec.cpp`）
- `KEY_SIZE = 32`（`resumption_state_codec.h:22`）。
- sealed 线格式（`resumption_state_codec.cpp:99-103`）：
  `magic("URS1",4) ‖ version(1=0x01) ‖ nonce(12) ‖ ciphertext(N) ‖ tag(16)`。
- 常量：`kResumptionStateMagic={'U','R','S','1'}`、`kResumptionNonceSize=12`、`kResumptionTagSize=16`、`kResumptionFormatVersion=1`、AAD=`"UTP-SessionResumptionState-V1"`、AEAD=AES-256-GCM（`resumption_state_codec.cpp:18-23`,`:49`）。
- 明文内层结构（由连接层构造，`connection_impl.cpp:3524-3550`）：
  `BE32(UTP_PROTOCOL_VERSION) ‖ encryption_mode(1) ‖ BE64(expiresAt) ‖ ticketSize(2) ‖ sessionTicket(ticketSize) ‖ pskSize(1) ‖ resumptionPsk(pskSize)`。
- 导出串再经标准 Base64 编码（`Base64::EncodeStd`，`connection_impl.cpp:3558`；`base64.cpp` 用 OpenSSL `EVP_EncodeBlock`/`EVP_DecodeBlock`）。

---

## 3. 流程

### 3.1 流量密钥派生（`TrafficKeySchedule::Derive`，`traffic_key_schedule.cpp:46-104`）
1. 校验：`keySize ∈ {16,32}`，`clientCid≠0`，`serverCid≠0`，共享密钥非全零，否则 `UTP_ERR_CRYPTO_INIT_FAILED`。
2. 组装 77B transcript（§2.2）→ `transcriptHash = SHA256(transcript)`。
3. `salt = SHA256("libutp-handshake-v2" ‖ transcriptHash)`。
4. `info = "libutp-traffic-keys-v2" ‖ transcriptHash`。
5. `HKDF-SHA256(IKM=sharedSecret, salt, info)` 输出 `2*(keySize+4)` 字节。
6. 切分：`[c2s.key | c2s.noncePrefix | s2c.key | s2c.noncePrefix]`；`OPENSSL_cleanse` 中间缓冲。

### 3.2 建立方向隔离 AEAD 上下文（`CreateAesGcmContexts`，`traffic_key_schedule.cpp:106-171`）
1. 由本端 X25519 私钥与对端公钥 `deriveSharedSecret`。
2. 依据 `localIsClient` 归一化 (clientPubKey, serverPubKey, clientCid, serverCid)——保证双方派生一致。
3. 调 `Derive` 得 `Material`。
4. tx 用本方向 secret、rx 用对向 secret 初始化 `AesGcmContext`；派生 32B key 时取 256-GCM，16B 取 128-GCM。
5. 全程 `OPENSSL_cleanse` 共享密钥、临时 key、Material。
- 连接层调用点：握手收到 `kFrameCrypto` 帧时以 `localIsClient=true`、`cryptoType=crypto.crypto_type`、`keySize=EncryptionKeySize(crypto_type)` 建上下文（`connection_impl.cpp:909-912`）。

### 3.3 报文加/解密（`AesGcmContext`）
- 加密 `encrypt(PacketOut*)`（`aes_gcm_context.cpp:186-251`）：AAD = 明文头 20B（`UTP_HEADER_SIZE`），payload → ciphertext+tag；支持原地或从池分配缓冲；写回 payload_length、置 `kPoEncrypted` 标志。
- 解密 `decrypt(PacketIn*)`（`aes_gcm_context.cpp:253-306`）：payload_size 至少 `GCM_TAG_SIZE`，否则 `UTP_ERR_CRYPTO_DECRYPTION`。
- `encryptScatter`：多段明文一次 GCM 加密（`aes_gcm_context.cpp:385-484`）；连接层聚合发送使用（`connection_impl.cpp:2820`）。
- 连接层实际调用点：`m_txAesCtx->encryptScatter/encrypt`（`connection_impl.cpp:2820`,`:2842`）。

### 3.4 票据式 0-RTT（`connect0Rtt`，`context_impl.cpp:583-615`）
1. 校验 `ip/port/session_ticket` 非空；`UTP_HEADER_SIZE + payload > 1280` 报 `UTP_ERR_OVERFLOW`。
2. 强制 `base.encrypted = kEncryptionNone`（§7）。
3. 构造 `ZeroRttConfig{ sessionTicket, earlyData, earlyFin, source=kSourceSessionToken, expiresAtSec = now + max(zero_rtt_token_max_lifetime,1) }`。
4. 走 `connectInternal`。

### 3.5 状态式 0-RTT（`connect0RttWithState`，`context_impl.cpp:617-661`）
1. `parseSessionResumptionState(state)`：Base64 解码 → `ResumptionStateCodec::Open`（用 active 恢复密钥）→ 解出 version/mode/expiresAt/ticket/psk（`context_impl.cpp:672-725`）。
2. **若 `parsed.encrypted != kEncryptionNone` → 返回 `UTP_ERR_NOT_IMPLEMENTED`**（加密 0-RTT 被禁用，`context_impl.cpp:633-636`）。
3. 时效校验：`now > expiresAt` → `UTP_ERR_TIMEOUT`。
4. 构造 `ZeroRttConfig{ sessionTicket, resumptionPsk, earlyData, earlyFin, source=kSourceResumptionState, expiresAtSec }` → `connectInternal`。

### 3.6 服务端 0-RTT 校验与抗重放（接收路径）
- 服务端签发票据：`buildZeroRttSessionToken`（`context_impl.cpp:770-826`）—— 仅当 `encrypted == kEncryptionNone` 才签（否则返回 false）；`TokenMeta.token_type=kZeroRttResumption`、绑定 timestamp/cid/family/host。
- 校验：`validateZeroRttTicket`（`context_impl.cpp:1126-1192`）：
  1. `ticket.size() == TOKEN_SIZE(64)`；
  2. `TokenAuth::open(..., kZeroRttResumption)`；
  3. 地址族与 host 与来源地址逐字节比对；
  4. `now >= timestamp` 且 `now - timestamp <= min(zero_rtt_token_max_lifetime, validityPeriod)`；
  5. 解出 `encryptionMode` 与 `ticketCid`。
- 抗重放：`rememberZeroRttNonce(ticketCid, packetNumber)`（`context_impl.cpp:1229-1254`）——键 `(ticketCid, nonce=pn)`；命中即重放（返回 false）；否则记入缓存，过期时刻 `now + max(zero_rtt_replay_window,1)*1000ms`；`purgeZeroRttReplayCache` 惰性清理（`:1214-1227`）。接收路径调用点见 `context_impl.cpp:1687`,`:1717`,`:1933`。

---

## 4. 不变量与规则（MUST / MUST NOT）

- **MUST** 方向隔离：client→server 与 server→client 使用不同 (key, nonce_prefix)；tx/rx 分开（`traffic_key_schedule.cpp:134-135`）。
- **MUST** nonce 每方向每密钥唯一：nonce = prefix ‖ BE64(pn)，依赖 pn 单调（`aes_gcm_context.cpp:560-568`）。**MUST NOT** 复用同一 pn 加密两个不同 payload。
- **MUST** keySize ∈ {16,32}，cid≠0，共享密钥非全零，否则拒绝派生（`traffic_key_schedule.cpp:56`）。
- **MUST** 双方 transcript 完全一致（版本、cidclient/server 归一化、双方公钥顺序），否则密钥不匹配（`traffic_key_schedule.cpp:121-124`）。
- **MUST** 报文头 20B 作为 AAD 参与鉴权（明文但防篡改，`aes_gcm_context.cpp:229`,`:285`）。
- **MUST** token/state 使用 AEAD + 独立 AAD 字符串；`open()` **MUST** 校验 `token_type == expectedType`（`token.cpp:176-181`）。
- **MUST** 0-RTT 票据绑定来源地址（family + host）与时效；**MUST** 抗重放键为 `(ticketCid, pn)`（`context_impl.cpp:1149-1167`,`:1241-1248`）。
- **MUST NOT** 使用加密的 0-RTT——加密恢复态当前被显式禁用（`context_impl.cpp:633-636`；`buildZeroRttSessionToken` 对 encrypted 返回 false，`:779`）。
- **MUST** 敏感缓冲清零：私钥、共享密钥、key、Material、expanded 均 `OPENSSL_cleanse`/volatile 清零（`x25519_wrapper.cpp:93`,`traffic_key_schedule.cpp:91`,`:129`,`:148-160`,`:163`,`aes_gcm_context.cpp:570-576`）。
- **MUST** token 密钥轮换：每 1 小时换 key，旧 key 再宽限一轮（`token.cpp:39`,`:198-207`）。
- `ResumptionStateCodec::Open` **MUST** 校验 magic("URS1") 与 version(1)，且总长 ≥ `4+1+12+16`（`resumption_state_codec.cpp:116-125`）。

---

## 5. 参数与默认值（确切值 + 变量名）

| 参数 / 常量 | 值 | 位置 |
|---|---|---|
| `Config::zero_rtt_token_max_lifetime` | `600`（秒） | `config.h:77` |
| `Config::zero_rtt_replay_window` | `10`（秒） | `config.h:78` |
| `X25519Wrapper::PRIVATE_KEY_SIZE/PUBLIC_KEY_SIZE/SHARED_SECRET_SIZE` | `32` | `x25519_wrapper.h:32-34` |
| `TrafficKeySchedule::MAX_KEY_SIZE` | `32` | `traffic_key_schedule.h:23` |
| `TrafficKeySchedule::NONCE_PREFIX_SIZE` | `4` | `traffic_key_schedule.h:24` |
| salt label | `"libutp-handshake-v2"` | `traffic_key_schedule.cpp:24` |
| info label | `"libutp-traffic-keys-v2"` | `traffic_key_schedule.cpp:25` |
| `AesGcmContext::GCM_TAG_SIZE` | `16` | `aes_gcm_context.h:29` |
| `AesGcmContext::GCM_NONCE_SIZE` | `12` | `aes_gcm_context.h:30` |
| 加密缓冲 size class | `{1280,1500,4096,9000,65535}` | `aes_gcm_context.cpp:31-37` |
| `kEncryptBufferCacheLimitPerClass` | `8` | `aes_gcm_context.cpp:30` |
| `AEAD_KEY_SIZE/NONCE_SIZE/TAG_SIZE` | `32/12/16` | `token.h:39-41` |
| `TOKEN_META_SIZE` | `36` | `token.h:68` |
| `TOKEN_SIZE` | `64`（12+36+16） | `token.h:73` |
| `TOKEN_KEY_UPDATE_INTERVAL_MS` | `3600000`（1h） | `token.cpp:16` |
| `TOKEN_AAD_PATH` | `"UTP-PathToken-AAD"` | `token.h:69` |
| `TOKEN_AAD_0RTT` | `"UTP-0RTTToken-AAD"` | `token.h:70` |
| `TokenType::kPathValidation/kZeroRttResumption` | `1/2` | `token.h:45-48` |
| `ResumptionStateCodec::KEY_SIZE` | `32` | `resumption_state_codec.h:22` |
| resumption magic / version / AAD | `"URS1"` / `1` / `"UTP-SessionResumptionState-V1"` | `resumption_state_codec.cpp:18-23` |
| `kDefaultResumptionSecret` | 固定 32B 常量数组 | `context_impl.cpp:57-62` |
| `UTP_PROTOCOL_VERSION` | `2` | `proto/proto.h:30` |
| `UTP_HEADER_SIZE`（AEAD AAD 长度） | `20` | `proto/proto.h:18` |
| `FrameCryptoType::kFrameCryptoAESGCM128/256` | `0/1` | `proto/frame.h:71-73` |
| `Connect0Rtt*Info::timeout` 默认 | `3000`（ms） | `context.h:95`,`:109` |

---

## 6. 对外接口 / 回调

### 6.1 `Context` 加密/0-RTT API（`cpp/include/utp/context.h`）
- `enum EncryptionMode { kEncryptionNone=0, kEncryptionAesGcm128, kEncryptionAesGcm256 }`（`context.h:44-48`）。
- `enum ConnectAttemptType { kConnectAttemptNormal, kConnectAttemptZeroRttToken, kConnectAttemptZeroRttState, kConnectAttemptPassive }`（`context.h:54-59`）。
- `int32_t connect0Rtt(const Connect0RttInfo&)`（票据式，仅非加密，`context.h:258`；实现 `context.cpp:119`→`context_impl.cpp:583`）。
- `int32_t connect0RttWithState(const Connect0RttWithStateInfo&, const std::string& state)`（状态式，`context.h:229`；实现 `context.cpp:99`→`context_impl.cpp:617`）。
- `void setResumptionSecret(const std::vector<uint8_t>&)` / `void clearResumptionSecret()`（32B 自定义恢复封装密钥，`context.h:216`,`:221`）。
- `Statistic statistic() const`（`context.h:235`）。
- 结构体：`Connect0RttInfo`（含 `session_ticket`,`early_data`,`early_fin`，`context.h:92-100`）、`Connect0RttWithStateInfo`（`context.h:106-113`）、`ZeroRttDecisionInfo`（`accepted`,`reason`，`context.h:131-138`）、`ConnectAttemptInfo`（`context.h:144-155`）。
- `Statistic` 0-RTT 字段：`zero_rtt_offered/accepted/rejected/replay_rejected/invalid_ticket_rejected`（`context.h:66-70`）。

### 6.2 回调
- `OnZeroRttDecision = std::function<void(const ZeroRttDecisionInfo&)>`，`setOnZeroRttDecision`（`context.h:161`,`:209`）。
- `OnConnectError` 携带 `ConnectAttemptInfo`（含 `type`,`session_token_size`,`resumption_state_size`,`early_data_size`，`context.h:158`）。

### 6.3 `Connection` 导出接口（`cpp/include/utp/connection.h`）
- `virtual int32_t exportSessionToken(std::vector<uint8_t>& outToken)`（`connection.h:166`；实现 `connection_impl.cpp:3457`，无缓存则 `UTP_ERR_SESSION_TOKEN_UNAVAILABLE`）。
- `virtual int32_t exportSessionResumptionState(std::string& outState)`（`connection.h:174`；实现 `connection_impl.cpp:3469`，无缓存则 `UTP_ERR_RESUMPTION_STATE_UNAVAILABLE`；内部 `buildSessionResumptionState` seal+base64）。
- 注：这两个方法声明在 `connection.h`，非 `context.h`（任务描述将其归于 context，实际位置以此为准）。

---

## 7. 当前实现边界（已实现 / 禁用 / 预留 + 代码位置）

**已实现且启用：**
- X25519 密钥交换（`x25519_wrapper.cpp`），BoringSSL 优先、OpenSSL EVP 回退。
- HKDF-SHA256 方向隔离流量密钥派生（`traffic_key_schedule.cpp`）。
- AES-128/256-GCM 报文加解密，且连接层实际使用（`aes_gcm_context.cpp`；调用点 `connection_impl.cpp:2820`,`:2842`,`:909`）——即**加密数据面是启用的**（非加密与加密均由握手 `kFrameCrypto` 协商）。
- `TokenAuth` seal/open + 1h 密钥轮换（`token.cpp`）。
- **非加密**票据式 0-RTT（`connect0Rtt`）与非加密状态式 0-RTT（`connect0RttWithState`）：签发、校验、地址绑定、时效、抗重放全链路（`context_impl.cpp`）。
- `ResumptionStateCodec` Seal/Open + Base64（`resumption_state_codec.cpp`,`base64.cpp`）。

**显式禁用：**
- **加密 0-RTT（early data with encryption）被禁用**：`connect0RttWithState` 遇到 `parsed.encrypted != kEncryptionNone` 直接返回 `UTP_ERR_NOT_IMPLEMENTED`，注释：*"encrypted 0-RTT is disabled until an authenticated early-data key format exists"*（`context_impl.cpp:633-636`）。
- `connect0Rtt` 硬编码 `base.encrypted = kEncryptionNone`（`context_impl.cpp:603`）；`MakeConnectAttemptInfo` 也把 0-RTT 尝试的 encrypted 归零（`context_impl.cpp:241`）。
- `buildZeroRttSessionToken` 对 `encrypted != kEncryptionNone` 返回 false，不签发加密 0-RTT 票据（`context_impl.cpp:779`）。

**预留 / 尚未使用：**
- `TokenMeta.secret`（local secret number）在 0-RTT 签发处恒为 0（`context_impl.cpp:799`），`version` 恒为 1（`context_impl.cpp:798`）——预留字段。
- 恢复状态明文格式已预留 `resumptionPsk`（加密恢复所需 PSK）与 `encryption_mode`，但加密路径未启用（校验逻辑存在于 `connection_impl.cpp:3517-3522`、`context_impl.cpp:718-721`）。
- `X25519Wrapper::deriveSharedSecretShort`（16B 短共享密钥）已定义但当前 traffic key 路径未调用（`x25519_wrapper.cpp:154-162`；未在本模块外发现使用，待确认调用方）。

**内部注释与代码不一致（非 doc/，属实现内注释）：**
- `aes_gcm_context.h:97-98` / `:127-128` 注释称 AAD 为“packet 头部 24 字节”，但实际传入长度为 `UTP_HEADER_SIZE = 20`（`proto/proto.h:18`，`aes_gcm_context.cpp:229`,`:285`）。以代码 20B 为准。

---

## 8. 与 doc/ 差异（方案 vs 现状）

参考 `doc/全包加密与无状态可验证CID混淆方案.md`（1133 行）。该文档是**未实现的设计提案**，与当前代码差异显著：

1. **CID 混淆 / 无状态可验证 CID**：doc 定义 `OpaqueCID = salt(2B) ‖ body_masked(6B) ‖ tag(8B)`，用 SipHash 派生 `K_cid_mask`/`K_cid_tag`（doc §7.4,`:228`,`:277-278`,`:308-326`）。**代码未实现**：CID 为明文 uint32，无 SipHash、无 mask/tag。
2. **全包加密**：doc 主张“最小明文外层 + 内层全包 AEAD”，头部也进入加密（doc `:5`,`:38`）。**代码现状**：20B 头保持明文，仅 payload 加密，头作 AAD（§1）。
3. **派生标签**：doc 用 `HKDF(handshake_secret, "pkt-c2s")` / `"pkt-s2c")`（doc `:490-491`）。**代码**用 SHA256 转录哈希 + salt label `"libutp-handshake-v2"` / info label `"libutp-traffic-keys-v2"`，一次 HKDF 双向切分（`traffic_key_schedule.cpp:24-25`,`:95-99`）。
4. **AEAD 套件**：doc 建议未来抽象 AEAD 接口，含 ChaCha20-Poly1305（doc `:594-599`）。**代码**仅 AES-128/256-GCM（无 ChaCha20，`aes_gcm_context.cpp`）。
5. **nonce 构造一致**：doc `nonce = nonce_prefix(4B) ‖ full_packet_number(8B)`（doc `:529`）与代码一致（`aes_gcm_context.cpp:560-568`）——此项方案与现状相符。
6. **0-RTT anti-replay**：doc 仅原则性要求“配套 anti-replay 机制”（doc `:798-805`）。**代码**给出具体实现：`(ticketCid, pn)` + 时间窗 `zero_rtt_replay_window`（§3.6）；且**加密 0-RTT 在代码中被禁用**（§7），而 doc 假定 early data key 可用。

结论：doc 描述的是目标架构（混淆数据面 + opaque CID），当前代码实现的是“明文头 + payload AES-GCM + 无状态票据 + 非加密 0-RTT + 抗重放”的较早阶段。

---

## 9. 依赖

- **OpenSSL / BoringSSL**：HKDF（`openssl/hkdf.h`）、SHA256（`openssl/sha.h`）、AES-GCM EVP（`openssl/evp.h`）、X25519（`openssl/curve25519.h` 或 EVP 回退）、`RAND_bytes`、`OPENSSL_cleanse`、Base64（`EVP_EncodeBlock`/`EVP_DecodeBlock`）。
- **`util/status.h` / `utp/errno.h`**：`Status`、错误码（`UTP_ERR_CRYPTO_INIT_FAILED`,`UTP_ERR_CRYPTO_ENCRYPTION`,`UTP_ERR_CRYPTO_DECRYPTION`,`UTP_ERR_CRYPTO_UNINITIALIZED`,`UTP_ERR_NOT_IMPLEMENTED`,`UTP_ERR_TIMEOUT`,`UTP_ERR_SESSION_TOKEN_UNAVAILABLE`,`UTP_ERR_RESUMPTION_STATE_UNAVAILABLE` 等）。
- **`proto/proto.h`**：`UTP_PROTOCOL_VERSION`,`UTP_HEADER_SIZE`；`proto/frame.h`：`FrameCryptoType`；`proto/packet_in.h`/`packet_out.h`：`PacketIn`/`PacketOut`。
- **`event/timer.h`（libevent）**：`TokenAuth` 密钥轮换定时器。
- **`utils/serialize.hpp` / `utils/endian.hpp` / `utils/exception.h`**：序列化、字节序、异常。
- **连接层 `ConnectionImpl` / `ContextImpl`**：驱动握手 `kFrameCrypto`、报文加解密、0-RTT 决策与统计（`connection_impl.cpp`,`context_impl.cpp`）。

---

## 10. C 版加密 0-RTT 实现约束与进度（已确认设计）

> 本节是 C 版实现的约束，优先于本文从 C++ 现状反推的轮换设计。
>
> 当前已实现：恢复状态解封、半加密 `0RTT` 包、early C2S/S2C AEAD、固定帧序校验、两消息建连、
> attempt 级 replay 命中的握手响应重建，以及服务端在响应 UDP 写成功后才投递 early stream。
> 仍待补齐：客户端提前收到普通 `CTRL` 的单包缓存与快速重传，以及超过首个 early 包容量的数据转入
> 1-RTT 流发送队列。

### 10.1 恢复根密钥生命周期

- Context 仅提供 `utp_context_set_resumption_key(root[32])` 设置恢复根密钥。
- 内部先对 root 执行 `HKDF-Extract`，再通过不同标签分别执行 `HKDF-Expand` 派生 `ticket_seal_key` 与 `local_state_key`；不得直接将 root 作为两类 AEAD 的工作密钥，也不得以 one-shot `HKDF()` 隐藏两个阶段。
- 未设置 root 时使用库内置固定默认 root，以允许 Context 或进程重启后继续解析此前签发的恢复状态；每个 Context 仅记录一次英文 Warn：`utp: using built-in default resumption key; configure a custom key for production`。
- **不支持平滑轮换**：不维护 `key_id`、旧 keyring、宽限期或自动轮换任务。
- 调用 `utp_context_set_resumption_key()` 替换 root 后，旧 `SESSION_TOKEN` 与客户端本地恢复状态必须立即失效；后续连接应走普通 1-RTT 握手，不得尝试旧 0-RTT 状态。
- Context 销毁时必须清零 root、派生工作密钥及连接持有的 `resumption_psk`。

### 10.2 `SESSION_TOKEN` 与本地恢复状态

- 不新增 `SESSION_TOKEN_V2`；现有帧改为 `type[1] | payload_length[1] | expires_at_seconds_be[8] | payload`。`expires_at_seconds` 是以大端编码的绝对 Unix 秒时间。
- 服务端在已建立连接的 `CTRL` 中签发统一 token，原连接是明文还是加密不改变 payload 结构：`payload = resumption_psk[32] | encrypted_server_info[93]`。明文连接只将 `encryption_mode` 记录为 `NONE`，恢复凭证本身仍必须加密保护。
- 客户端发起加密 0-RTT 时回传：`payload = early_attempt_nonce[16] | encrypted_server_info[93]`。`early_attempt_nonce` 每次新的连接尝试随机生成；同一尝试的 PTO 重传必须复用。
- `encrypted_server_info` 固定 93 字节：`nonce[12] | AES-256-GCM ciphertext[65] | tag[16]`。其明文为 `resumption_psk[32] | encryption_mode[1] | peer_id[32]`；当前 `peer_id` 全零，为后续 NTRS 预留。其 AAD 为 ASCII `"UTP-SessionToken" | expires_at_seconds_be[8]`。
- `encrypted_server_info` 不绑定地址或地址族，也不是对端身份凭证。服务端重启后，只要恢复根密钥不变，仍可解析未过期 token。
- `utp_connection_export_session_token()` 对明文与加密连接都导出相同的本地加密 envelope：`"URS1"[4] | nonce[12] | AES-256-GCM ciphertext | tag[16]`，AAD 为 ASCII `"UTP-LocalResumptionState"`。当前 125 字节 token payload 对应的编码长度是 166 字节，内层明文为 `encryption_mode[1] | expires_at_seconds_be[8] | token_payload`；解码必须以认证后的内层 payload 长度为准，不得通过外层总长度等于 166 来判断恢复状态类型，以允许后续增删字段。
- 客户端不向应用暴露明文 `resumption_psk`；`utp_context_connect_0rtt()` 从上述本地恢复状态恢复它和加密方式。服务端从 `encrypted_server_info` 恢复它们，且双方的 `encryption_mode` 必须与 `CRYPTO.crypto_type` 一致。
- 普通连接（明文或加密）完成以及每次成功的 0-RTT 重连完成后，服务端均应通过新的 `CTRL` 签发 token；加密连接的 `CTRL` 仍按 1-RTT 密钥加密。旧 token 仅自然过期，不支持单独撤销。

### 10.3 early key 派生与报文鉴权

- `resumption_psk` 只作为 HKDF 的输入，不得直接用作 0-RTT AEAD 工作密钥。
- 设 `early_secret = HKDF-Extract(salt = SHA256("libutp-0rtt-early-v1"), IKM = resumption_psk)`。两方向的 HKDF info 分别为 `"libutp-0rtt-c2s-v1" | early_attempt_nonce[16] | encrypted_server_info[93] | encryption_mode[1]` 和 `"libutp-0rtt-s2c-v1" | early_attempt_nonce[16] | encrypted_server_info[93] | encryption_mode[1]`，输出 `AEAD key || nonce_prefix[4]`；AEAD key 长度由 `encryption_mode` 决定。
- 两个方向必须使用独立的 early AEAD：客户端 `0RTT` 用 `early_c2s`，服务端 `UTP_TYPE_HANDSHAKE` 响应用 `early_s2c`。这样客户端可确认响应来自持有 `resumption_psk` 的服务端。
- 同一尝试的 PTO 重传必须复用 CID、`early_attempt_nonce` 和 X25519 密钥对，但每次请求和响应都必须分配新的 packet number，并以新 nonce 重新执行 AEAD。新的 `utp_context_connect_0rtt()` 调用必须产生新的 `early_attempt_nonce`；任何方向都不得在相同 early key 下复用 `(key, packet_number)`。
- early AEAD 的 AAD 必须为当前 UTP Header 与本次尝试的完整明文 `SESSION_TOKEN` 帧，防止 token、CID 或包头被替换到另一份密文包。
- early AEAD nonce 固定为 `nonce_prefix[4] | packet_number_be[8]`。early_c2s 与 early_s2c 使用方向独立的 key、nonce prefix 和 packet number 空间。

### 10.4 重放缓存与 pending 握手清理

- Context 的 `zero_rtt_replay_cache_capacity` 必须可配置，默认 `4096` 条。replay key 固定为 `SHA256(encrypted_server_info)[0..15] | early_attempt_nonce[16]`，用于标识整个连接尝试，不能包含 packet number；记录保存到 token 的绝对过期时间，不得为了接纳新记录而淘汰未过期记录。
- Context 的被动握手响应基础超时由 `handshake_timeout` 配置，默认 `800ms`；最大重传次数由 `handshake_max_retries` 配置，默认 `2` 次。普通 1-RTT pending 与 0-RTT 响应 flight 必须共用这组策略，重试耗尽后释放各自持有的 pending 状态。
- 普通 1-RTT Handshake 每次重传也必须使用新 packet number。客户端的 `HANDSHAKE_DONE` 确认本次实际收到的 Handshake packet number；服务端必须接受当前 pending 已发送范围内的任一 Handshake 确认，不能因已经发出更新的响应而拒绝较早响应的迟到确认。只有确认可关联到精确发送时刻的最新响应时才形成 RTT 样本，其他情况按 Karn 原则跳过 RTT 更新。
- 服务端必须先完成 token 与 early AEAD 校验，再查询并插入 replay record；插入成功后才能创建 pending Connection 或缓存 early stream，避免无效包耗尽缓存、重复包创建多个连接。
- replay table 满且不存在可清理的过期记录时，拒绝新的 0-RTT 包且不响应；不得放宽重放保护。
- pending Connection 共享普通握手的 Context 生命周期与清理原则；0-RTT 的差异仅限入站校验、early stream 暂存和独立响应 flight。客户端等待服务端 `UTP_TYPE_HANDSHAKE`；服务端验证通过后生成响应，仅在该包成功写出后进入 `CONNECTED`。遇到暂时发送阻塞时必须保留 PacketOut 与 early PacketIn，等待 UDP writable 后继续发送；达到握手重传上限后必须从 Context 移除并释放 Connection。
- 重复 0-RTT 包不得重置 pending Connection 的总过期时间或重传额度。服务端已连接后仍需在短期握手响应有效期内，按既有 PTO、重传次数和防放大额度重新构造 `UTP_TYPE_HANDSHAKE` 响应；每次响应使用新 packet number、新密文和当前 `HANDSHAKE_DELAY`，不得重复投递 early stream 或创建 Connection。该响应状态必须绑定首次请求的来源地址与完整 token，来源改变的重复包不响应。
- 服务端收到任意可验证的客户端 1-RTT 包后可释放响应缓存；响应缓存超时、达到重传上限或所属 Connection 关闭时也必须释放。replay record 仍保留到 token 过期，仅继续拒绝后续重复包。
- replay cache 是 Context 内存状态，不跨进程或集群共享。服务端重启后，旧 token 仍可被恢复根密钥解析且允许再次连接，但此前的重放记录已丢失；应用层 early data 因此必须幂等。

### 10.5 0-RTT 半加密分帧边界

- 0-RTT payload 的唯一合法布局为 `[明文 SESSION_TOKEN][early_ciphertext][GCM tag]`。`SESSION_TOKEN` 必须是第一个且唯一的明文帧，其长度字段确定密文起点。
- `SESSION_TOKEN` 后的字节在 early AEAD 验证成功前必须一律视为密文，禁止进入普通帧解析路径；校验失败、长度不足、未知帧或截断均静默丢弃。
- 明文 `expires_at_seconds` 必须参与 `encrypted_server_info` 的 AAD，防止被替换或篡改。
- 客户端 0-RTT 解密后的帧顺序固定为：`CRYPTO(client_x25519_pub) | VERSION | TRANSPORT_PARAMS | ACK_FREQUENCY | [STREAM(stream_id=0)] | [PING] | PADDING`。前四帧必须各出现一次且顺序固定；除可选的 stream 0、PING 和末尾 PADDING 外，ACK、关闭、其他 stream 及其他控制帧必须拒绝。
- 服务端响应的 early_s2c 明文帧顺序固定为：`CRYPTO(server_x25519_pub) | VERSION | TRANSPORT_PARAMS | ACK_FREQUENCY | ACK | HANDSHAKE_DELAY | PADDING`。`ACK` 必须只确认触发本次响应的 0-RTT 请求 packet number；`HANDSHAKE_DELAY` 必须携带该请求接收至本次响应构造时的处理耗时。响应重传必须重新编码二者，以便客户端把 RTT 样本关联到具体请求。
- 客户端 0-RTT 和服务端 early_s2c 响应都必须补齐到配置的 `mtu_min`；为补齐加入的 `PADDING` 只能位于对应的 AEAD 密文区。
- early stream 数据超过单包密文容量时，仅首段放入 0-RTT；余量保留在 stream 0 的普通发送队列，待 1-RTT 建立后从连续 offset 发送。FIN 仅能放在最终片段。

### 10.6 两消息加密 0-RTT 与 `CTRL` 乱序处理

- 加密 0-RTT 使用两消息建连：客户端发送 `0RTT`，服务端验证成功后以 `UTP_TYPE_HANDSHAKE` 发送 early_s2c 加密响应。客户端成功验证该响应、校验 §10.5 的服务端帧序列并派生 1-RTT 密钥后立即进入 `CONNECTED`，不发送额外握手确认包。
- `UTP_TYPE_HANDSHAKE` 是服务端的首个必发响应，不能等待业务处理结果或客户端重传触发。服务端成功写出该包后可进入 `CONNECTED` 并发送正常 1-RTT `CTRL`；普通 `CTRL` 不得充当握手响应，因为客户端在取得服务端 X25519 公钥前无法解密它。
- 服务端进入 `CONNECTED` 不等于原路径已验证。在收到客户端任意可验证的 1-RTT 包前，Handshake 响应重发以及随后发送的所有 1-RTT `CTRL` 合计仍受该来源地址的防放大额度约束；客户端 0-RTT padding 提供的接收字节可计入额度。
- 服务端在 Handshake 响应真正写入 UDP 前不得向应用投递 early stream；early PacketIn 由 Connection 槽位持有，UDP `EAGAIN` 时继续保留，响应发送成功后才按普通 stream 接收路径投递一次。
- 客户端调用 `utp_context_connect_0rtt()` 时必须立即创建并注册 Context 所有的 pending Connection，以其 CID 接收后续报文；该对象在握手成功前不得交给调用者。失败时通过不携带 Connection 参数的既有 `on_connect_error` 报告并释放。
- pending 客户端若先收到来自预期对端、CID 匹配的普通 `CTRL`，但尚无 1-RTT 接收密钥，Connection 自身必须零拷贝保留至多一个原始 `PacketIn`，并触发一次受限的 0-RTT 快速重传。Context 仅负责 CID 路由，不持有该缓存。
- 快速重传只允许一次、消耗既有握手重传额度且不延长握手总超时；重传必须沿用 attempt 身份但生成新 packet number 和新密文。服务端命中 replay record 后使用最新有效请求重新构造 Handshake 响应。
- 客户端收到并验证 Handshake 响应、派生 1-RTT 密钥后，必须将缓存的 `CTRL` 交给标准 1-RTT 收包路径解密和处理；认证失败即丢弃。缓存包在成功回放、握手失败或超时时释放。
- early 与 1-RTT 使用不同密钥 epoch，可各自使用独立 packet number 空间；缓存的普通 `CTRL` 只按 1-RTT 包号窗口处理，不与 Handshake 的 early 包号混用。
- 客户端仅在响应 ACK 的请求 packet number 仍可关联到唯一发送时刻时，使用 `本地响应接收时刻 - 对应请求发送时刻 - HANDSHAKE_DELAY` 形成握手 RTT 样本；延迟到达且只确认已退休旧包号的响应按 Karn 原则跳过样本。服务端在连接后通过正常 1-RTT ACK 更新 RTT。

### 10.7 Handshake 响应线格式与被动回调时点

- 加密 0-RTT 的完成响应复用 header `types = UTP_TYPE_HANDSHAKE`，不新增包类型，也不携带旧 `UTP_FRAME_TYPE_HANDSHAKE_DONE` 帧。客户端必须根据本地 pending 尝试类型选择解析路径：普通 1-RTT pending 将其作为 server hello 处理；加密 0-RTT pending 必须使用 `early_s2c` 解密，并严格校验 §10.5 的服务端帧序列及 ACK 关联。不得退回普通 `CTRL` 或明文 Handshake 解析路径。
- 被动加密 0-RTT 的 `on_new_connection` 必须在 token、重放、early AEAD 以及 §10.5 客户端完整帧序列全部校验完成后，且在服务端发送完成响应前调用，以保留应用拒绝机会。回调只接收 `utp_new_connection_info_t`，内部 Connection 仍由 Context 持有并保持 pending；early stream 只在 Connection 内缓存，不向应用投递。
- 普通连接与 0-RTT 使用相同的应用接受语义：应用必须在 `on_new_connection` 回调内部调用 `utp_context_accept(context)`，调用成功后返回 `true`；若不接受或 `accept` 失败则返回 `false`。0-RTT pending 必须在调用回调前进入可被 `utp_context_accept()` 取得的待接受队列。
- 对 0-RTT，`utp_context_accept()` 成功后服务端生成并发送 early_s2c `UTP_TYPE_HANDSHAKE` 响应；仅在 UDP 写成功后进入 `CONNECTED` 并投递 early stream。后续重传重新构造新包，若回调拒绝，则不得发送响应，也不得投递 early stream。
