# UTP-07 路径验证 / 连接迁移 / 抗放大 需求文档（代码反推）

> Ground truth = C++ 代码（`cpp/`）。`doc/` 仅交叉参考，不一致以代码为准并在 §8 标注。
> 引用格式：`文件:行号`。数值均取自代码；无法确定处标注"待确认"。

---

## ① 职责与边界

本模块负责在连接生命周期内检测对端网络地址变化、通过 PATH_CHALLENGE / PATH_RESPONSE 验证候选路径的可达性、在验证成功后完成"业务路径迁移"，并在验证期间通过 anti-amplification 门控抑制向未验证路径的放大式发送。

- 核心类：`NetworkPath`（`cpp/src/util/network_path.h:20`），封装单条路径的状态机与 challenge 生命周期。
- 帧：`FramePathChallenge` / `FramePathResponse`（`cpp/src/proto/frame/path.h:22`、`:34`）。
- 驱动方：`ConnectionImpl`（`cpp/src/context/connection_impl.cpp`），持有 `m_networkPath`（`cpp/src/context/connection_impl.h:321`），负责触发检测、发送/响应帧、门控发送、超时重试。

边界（当前实现明确不做的事）：
- 仅维护"单一 candidate + 单一 active"的保守双路径模型；不做多候选路径并发竞争。
- 激进迁移（`kPathMigrationAggressive`）配置位存在但被强制按保守策略执行（`cpp/src/context/connection_impl.cpp:2922-2925`，TODO path-migration）。
- 无 PathToken / 路径授权机制。
- candidate 验证失败不关闭连接，仅回退到 active 路径。

---

## ② 数据结构 / 帧

### NetworkPath 成员（`cpp/src/util/network_path.h:65-75`）
- `Address m_peerAddress`：当前该路径绑定的对端地址。
- `State m_state`：路径状态，初值 `kPathUnknown`。
- `std::array<uint8_t, FRAME_PATH_DATA_SIZE> m_pendingChallenge`：本端已发出、等待回显的 challenge 随机数。
- `bool m_hasPendingChallenge`：是否有在途未完成的 challenge，初值 `false`。
- `utp_time_t m_challengeDeadlineMs`：当前 challenge 的超时截止时刻（单位 ms）。
- `uint32_t m_challengeTimeoutMs`：单次 challenge 超时（ms），默认 1500。
- `uint8_t m_retryCount`：已发送 challenge 次数，初值 0。
- `uint8_t m_maxChallengeRetries`：最大重试次数，默认 3。

### 帧格式（`cpp/src/proto/frame/path.h:15-17`、`cpp/src/proto/frame/path.cpp`）
- `FRAME_PATH_DATA_SIZE = 8`（challenge/response 载荷字节数）。
- `FRAME_PATH_HDR_SIZE = 1`（帧类型 1 字节）。
- `FRAME_PATH_FRAME_SIZE = 9`（HDR + DATA）。
- 线格式：`[type:1B][data:8B]`。type 为 `FrameType` 序号：`kFramePathChallenge = 9`、`kFramePathResponse = 10`（按 `cpp/src/proto/frame.h:28-44` 从 0 计数）。
- `data`：`std::array<uint8_t, 8>`（`cpp/src/proto/frame/path.h:31`、`:43`）。
- encode/decode 校验 `size >= 9`，否则 `UTP_ERR_OVERFLOW`；decode 校验帧类型匹配，否则 `UTP_ERR_FRAME_UNEXPECTED`（`cpp/src/proto/frame/path.cpp:25-53`）。

### ConnectionImpl 相关成员
- `NetworkPath m_networkPath`（`cpp/src/context/connection_impl.h:321`）。
- `uint64_t m_bytesIn` / `uint64_t m_bytesOut`（`cpp/src/context/connection_impl.h:336-337`）：连接生命周期累计收/发字节，用于 anti-amplification（见 §④、§⑦）。

---

## ③ 状态机

### NetworkPath::State（`cpp/src/util/network_path.h:22-27`）
- `kPathUnknown = 0`
- `kPathValidated = 1`
- `kPathValidating = 2`
- `kPathFailed = 3`

### 状态转换
- **构造/绑定** `bindPeerAddress`（`network_path.cpp:22-29`）：重置 retry/pending/deadline；若地址有效 → `kPathValidated`，否则 → `kPathUnknown`。
- **地址变化检测** `detectPeerAddressChange`（`network_path.cpp:31-52`）：
  - 新地址无效 → 返回 false，不变。
  - 当前无有效地址 → 直接 `bindPeerAddress`（进入 `kPathValidated`），返回 false（视为首次绑定，非迁移）。
  - 地址相同 → 返回 false。
  - 地址不同 → `m_peerAddress = new`，`m_state = kPathValidating`，重置 retry/pending/deadline，返回 true（触发验证）。
- **发送 challenge** `makePathChallenge`（`network_path.cpp:59-82`）：仅在 `kPathValidating` 有效，否则 `UTP_ERR_INVALID_STATE`；有在途且未超时 → `UTP_ERR_IN_PROGRESS`；重试耗尽 → `kPathFailed` + `UTP_ERR_TIMEOUT`；否则生成随机 8 字节，置 pending、deadline=now+timeout、`++retry`。
- **收到 response** `onPathResponse`（`network_path.cpp:90-105`）：需处于 `kPathValidating` 且有 pending 且回显数据完全相等（`IsChallengeEqual`，`network_path.cpp:131-135`）→ `kPathValidated`，清 pending/retry/deadline，返回 true；否则 false。
- **超时** `onTimeout`（`network_path.cpp:107-124`）：需 pending 且 `kPathValidating` 且已过 deadline；清 pending，若重试耗尽 → `kPathFailed` 返回 true，否则返回 false（允许再次发起）。

### 连接层迁移流程（`connection_impl.cpp`）
1. 收包 `onReceive`：非 active 路径且非 closing → `detectPeerAddressChange`；若返回 true → `notePathValidationStarted()` + `maybeSendPathChallenge()`（`connection_impl.cpp:668-674`）。
2. candidate 路径（`fromCandidatePath`）在验证成功前只处理 PathChallenge/PathResponse/ConnectionClose，其余帧跳过（`connection_impl.cpp:677-678`、`:737-740`）。业务数据仍走 active 路径（active 不切换）。C 实现对已认证的非白名单 PacketIn 额外保留零拷贝引用，验证成功后按接收顺序重放；缓存期间不执行业务帧副作用。
3. 收 PathChallenge → `handlePathChallengeFrame`：原样回显生成 PathResponse 发回 `fromAddress`（`connection_impl.cpp:2984-3001`）。被动响应不改变本端状态。
4. 收 PathResponse → `handlePathResponseFrame`：仅当 `needPathValidation()` 且 `fromAddress == candidate` 才处理；`onPathResponse` 成功后 **才** 将 `m_peerAddress` 切到 candidate，通知 `m_mtuDiscovery.onPathValidated`，停验证定时器，`notePathValidationSucceeded()`（`connection_impl.cpp:3003-3025`）。
5. 验证超时 `onPathValidationTimeout`：`onTimeout` 若 → `kPathFailed`，则 `notePathValidationFailed()` 并 `bindPeerAddress(m_peerAddress)` 回退到 active 路径、停定时器（不关连接）；否则若仍需验证且无在途 → 重发 challenge（`connection_impl.cpp:3027-3044`）。

---

## ④ 不变量与规则（MUST / MUST NOT）

- MUST：只有处于 `kPathValidating` 才能生成 challenge（`network_path.cpp:61-63`）。
- MUST：PathResponse 的 data MUST 与本端 pending challenge 逐字节相等才算验证成功（`network_path.cpp:96-98`、`:131-135`）。
- MUST：业务路径（active）在验证成功前 MUST NOT 切换到 candidate；仅在 `onPathResponse` 成功后才更新 `m_peerAddress`（`connection_impl.cpp:3015-3017`，保守策略）。
- MUST：candidate 路径在验证成功前 MUST NOT 处理除 PathChallenge/PathResponse/ConnectionClose 外的帧（`connection_impl.cpp:737-740`）。
- MUST：验证期间（`needPathValidation()`）除白名单帧（PathChallenge/PathResponse/Ping/Ack/HandshakeDone/ConnectionClose）外，向路径发送受 anti-amplification 限制（`connection_impl.cpp:2926-2944`）。
  - 公式：`outBytesNext = m_bytesOut + packetLen`；`limit = m_bytesIn * 3 + kPathValidationSendCredit`；允许发送 ⇔ `outBytesNext <= limit`（`connection_impl.cpp:2942-2944`）。
  - 超限 → `sendPacket` 返回 `UTP_ERR_PATH_VALIDATION_BLOCKED`（`connection_impl.cpp:2674-2676`）。
- MUST：路径处于 `kPathFailed` 且非 close 包时 MUST NOT 发送，返回 `UTP_ERR_INVALID_STATE`（`connection_impl.cpp:2646-2648`）。
- MUST：challenge 重试次数 MUST < `m_maxChallengeRetries`（`canRetryChallenge`，`network_path.cpp:126-129`），耗尽即 `kPathFailed`。
- MUST NOT：candidate 验证失败不得关闭连接，需回退 active（`connection_impl.cpp:3030-3036`，注释"保守策略"）。
- MUST：PathResponse 只接受来自 candidate 地址的响应，来源地址不符直接忽略（`connection_impl.cpp:3005`）。

---

## ⑤ 参数与默认值（确切值 + 变量名）

| 参数 | 变量名 | 默认值 | 来源 |
|---|---|---|---|
| challenge 载荷字节数 | `FRAME_PATH_DATA_SIZE` | 8 | `frame/path.h:15` |
| path 帧总长 | `FRAME_PATH_FRAME_SIZE` | 9 | `frame/path.h:17` |
| challenge 超时(ms) | `NetworkPath::m_challengeTimeoutMs` | 1500 | `network_path.h:73`、`network_path.cpp:17` |
| challenge 最大重试 | `NetworkPath::m_maxChallengeRetries` | 3 | `network_path.h:75`、`network_path.cpp:18` |
| anti-amplification 附加信用(字节) | `kPathValidationSendCredit` | **256** | `connection_impl.cpp:77` |
| anti-amplification 倍率 | 字面量 `3` | 3 | `connection_impl.cpp:2943` |
| 路径迁移模式 | `Config::path_migration_mode` | `kPathMigrationConservative`(0) | `include/utp/config.h:81` |
| C 候选路径缓存上限 | `utp_context_options_t::path_validation_buffer_capacity` | 16 KiB，`0` 禁用 | C 实现；已认证 PacketIn 零拷贝 FIFO |

**注意（重要）**：`NetworkPath` 的超时与重试并非来自独立的路径验证配置，而是构造时被接到 keepalive 配置：
`m_networkPath(ctx->config()->keepalive_timeout, ctx->config()->keepalive_probes)`（`connection_impl.cpp:339-341`）。
即实际 challenge 超时 = `Config::keepalive_timeout`（默认 1500，`config.h:72`），实际最大重试 = `Config::keepalive_probes`（默认 3，`config.h:73`）。构造参数默认值 1500/3 仅在 `ctx == nullptr` 时作为兜底（且 `NetworkPath` 构造函数还对 `<=0` 做 1500/3 兜底，`network_path.cpp:17-18`）。

---

## ⑥ 接口

### NetworkPath 公开接口（`network_path.h:32-59`）
- `void bindPeerAddress(const Address&)`
- `const Address& peerAddress() const`
- `bool detectPeerAddressChange(const Address&)` → true 表示进入 validating
- `bool needPathValidation() const`（== `m_state == kPathValidating`）
- `bool hasInFlightChallenge() const`
- `State state() const`
- `int32_t makePathChallenge(FramePathChallenge&, utp_time_t nowMs)` → `UTP_ERR_OK`/`UTP_ERR_INVALID_STATE`/`UTP_ERR_IN_PROGRESS`/`UTP_ERR_TIMEOUT`
- `void makePathResponse(const FramePathChallenge&, FramePathResponse&) const`（原样拷贝 data）
- `bool onPathResponse(const FramePathResponse&)`
- `bool onTimeout(utp_time_t nowMs)`
- `bool canRetryChallenge() const`
- `retryCount()` / `maxChallengeRetries()` / `challengeDeadlineMs()`

### 帧接口（`frame/path.h`）
- `FramePathChallenge::encode/decode`、`FramePathResponse::encode/decode`。

### ConnectionImpl 私有接口（`connection_impl.h:229-231`）
- `Status maybeSendPathChallenge()`
- `Status handlePathChallengeFrame(const uint8_t*, size_t, const Address&)`
- `Status handlePathResponseFrame(const uint8_t*, size_t, const Address&)`
- `bool canSendOnCurrentPath(size_t packetLen, FrameType) const`（anti-amplification 门控，`connection_impl.cpp:2920`）
- `void onPathValidationTimeout()`（定时器回调）

### 错误码
- `UTP_ERR_PATH_VALIDATION_BLOCKED = 0x0010`（`include/utp/errno.h:37`，"路径校验阶段受 anti-amplification 限制"）。

### 统计（`include/utp/context.h:71-73`，经 `notePathValidation*` 递增，`context_impl.cpp:1194-1208`）
- `path_validation_started` / `path_validation_succeeded` / `path_validation_failed`。

---

## ⑦ 当前实现边界 / 风险

1. **激进迁移未实现**：`path_migration_mode = kPathMigrationAggressive` 被 `canSendOnCurrentPath` 强制忽略，仍按保守门控执行（`connection_impl.cpp:2922-2928`，TODO path-migration）。
2. **anti-amplification 基于连接累计字节，非按路径重置**：`m_bytesIn`/`m_bytesOut` 在整条连接生命周期累计（收包 `+=`，`connection_impl.cpp:656`；发包 `+=`，`connection_impl.cpp:2895`），迁移进入 validating 时 **不清零**。因此 validating 阶段的发送预算是"历史累计收字节×3+256"，对成熟连接而言 limit 已很大，门控对已建立连接的实际抑制效果有限；credit 常量为 256 字节（约 1/6 MTU）。风险：抗放大语义偏"整连接"而非 RFC 9000 式"每路径未验证额度"。（待确认是否为设计意图。）
3. **超时/重试无独立配置**：复用 keepalive 配置（见 §⑤ 注意）。若用户调 keepalive_timeout/probes，会同时改变路径验证行为，二者耦合。
4. **challenge 随机性依赖 `RandomBytes`**（`network_path.cpp:76`，`util/random.hpp`），随机质量决定抗伪造强度；本模块不做额外加密绑定。
5. **被动响应无速率/来源限制**：`handlePathChallengeFrame` 对任意 challenge 都回显响应到来源地址（`connection_impl.cpp:2984-3001`），响应包本身也走 `sendPacket`（受 §④ 的 kPathFailed 与 anti-amplification 白名单约束——PathResponse 在白名单内，不受 3× 限制）。
6. `detectPeerAddressChange` 首次从无效地址绑定时返回 false 并直接 `kPathValidated`，不触发验证（`network_path.cpp:37-40`）——依赖首包地址被信任。

---

## ⑧ 与 doc/ 的差异

参考 `doc/设计实现文档.md` §3.7（行 183-196）、§6.3（行 456-467）。

- doc §3.7 列出状态为 "Unknown / Validating / Validated / Failed" 与 active/candidate 双路径模型 —— 与代码一致。
- doc 未给出 anti-amplification 的**确切公式与常量**。代码为 `m_bytesIn*3 + 256`（`kPathValidationSendCredit=256`）。任务提示中提到的"credit 统一 256→3×MTU"在本模块代码中**未体现**：当前值仍为 256 字节字面量（`connection_impl.cpp:77`），不是 3×MTU。以代码为准：**256**。
- doc 未说明超时/重试参数来源；代码实际复用 `keepalive_timeout`/`keepalive_probes`（差异见 §⑤），doc 亦未提及独立的 `challengeTimeoutMs=1500`/`maxChallengeRetries=3` 配置项——这两个默认值仅存在于 `NetworkPath` 构造兜底。
- doc §6.3 称"激进策略配置位已预留" —— 与代码 TODO 一致，确认未启用。

---

## ⑨ 依赖

- `Address`（`cpp/src/socket/address.h`）：地址相等/有效性判断（`isValid()`、`operator==`）。
- `FrameBase` / `FrameType`（`cpp/src/proto/frame.h`）：帧基类与类型枚举。
- `Serialize`（`utils/serialize.hpp`）：帧 type 字段序列化。
- `RandomBytes`（`cpp/src/util/random.hpp`）：challenge 随机数生成。
- `Status` / `errno.h`：错误封装（`UTP_ERR_*`）。
- `ConnectionImpl` 侧：`m_pathValidationTimer`（定时器，`connection_impl.cpp:352`）、`m_mtuDiscovery`（`onPathValidated`）、`sendPacket`（`UTP_TYPE_CTRL` 发送 path 帧）、`ContextImpl::notePathValidation*`（统计）、`Config`（keepalive_* / path_migration_mode）。
- `time::MonotonicMs()`：单调时钟，驱动超时。
