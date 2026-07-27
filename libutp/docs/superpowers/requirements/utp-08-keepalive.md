# UTP 需求文档 08 — Keepalive（空闲保活）

> 反推来源：C++ 实现（`cpp/` 为唯一 ground truth）。doc/ 仅交叉参考，不一致以代码为准。
> 引用格式 `文件:行号`；数值均取自代码。标注"待确认"处为代码未明确处。

---

## ① 职责与边界

Keepalive 子系统负责在**连接建立后的空闲期**主动探测对端存活，避免连接因长时间无流量被中间设备（NAT/防火墙）回收，并在对端确实失联时主动断开连接。

- 仅在 `State::kStateConnected` 状态下生效（`connection_impl.cpp:3190`、`3241`）。任何其他状态（握手中、关闭中、已断开、PTO timed-wait）下 keepalive 定时器被停止，探测不发出。
- 探测手段为发送**仅含一个 `Ping` 帧的 CTRL 包**（`connection_impl.cpp:3270-3271`）。
- 空闲的判定与"对端活动"的记录统一由 `markPeerActivity()` 驱动（`connection_impl.cpp:3201-3206`），任何收到的包都会刷新空闲计时。
- 探测失败达阈值后调用 `abortConnection()` 主动断连（`connection_impl.cpp:3266`）。
- 边界：keepalive **不负责** 握手超时、连接空闲总超时（`max_idle_timeout` 作为传输参数下发/推算用，见 ⑤）、close 重传等；这些由各自的定时器处理。

相关成员集中在 `ConnectionImpl`：
- 定时器 `m_keepaliveTimer`（`connection_impl.h:350`）
- 连续丢失探测计数 `m_keepaliveMissedProbes`（`connection_impl.h:368`，`uint16_t`，初值 0）
- 最近一次对端活动时间 `m_lastActivityUs`（`connection_impl.h:367`，`utp_time_t`，微秒，初值 0）

---

## ② 机制 / 帧

**Ping 帧（`kFramePing`）**
- 枚举定义 `cpp/src/proto/frame.h:34`（注释"心跳帧"）。
- 线格式：单字节帧，`frameLen = 1`（`cpp/src/proto/packet_in.cpp:129-131`）。载荷即 `payload[0] = kFramePing`（`connection_impl.cpp:3270`）。
- **ACK-eliciting**：接收侧对 Ping 帧本身**无专门处理分支**——收包主循环的 `switch(frameType)` 未列出 `case kFramePing`，落入 `default: break`（`connection_impl.cpp:742` 起，`969-971`）。但因为包含 Ping 帧的包 `frame_types != ackMask`，`ackOnly=false`（`connection_impl.cpp:1005-1006`），故被判为 ACK-eliciting（`noteAckElicitingPacket`，`connection_impl.cpp:1018-1019`），触发对端回 ACK。探测方随后收到该 ACK，经收包路径 `markPeerActivity()` 重置空闲计时与丢失计数——这是 keepalive 的闭环判活机制。
- **不重传**：`kFTBitPing` 在可重传帧掩码 `UTP_FRAME_RETX_MASK` 中被显式注释掉（`cpp/src/proto/packet_common.h:43-65`，第 48 行 `/* | kFTBitPing */`）。因此纯 Ping 包丢失时：
  - 在丢包检测中 `pkt->frame_types & m_retxFrames == 0`，被 `continue` 跳过，既不进入 `m_lostPackets` 重传队列，也不走 `handleRegularLostPacket`（`cpp/src/context/send_ctl.cpp:1307-1308`、`1331-1333`）。
  - keepalive 依赖自身的 probe 重试逻辑（③）在下次超时发送**新的** Ping，而非重传旧包。此行为与 `doc/frame/帧重传.md:70-113`（"PING 丢失不重传旧包，发新 PING"）一致。
- **被 send control 跟踪但不重传**：Ping 包经 `sendPacket(UTP_TYPE_CTRL, ...)`，其 `shouldTrackPacket = !isAckOnlyPacket && allowSendCtlRetrans = true`（`connection_impl.cpp:2642-2645`），故进入调度与 unacked 跟踪（用于 RTT/拥塞记账与被 ACK 后清理），但因不在 retx 掩码内而不会被重传。

---

## ③ 流程 / 计时

**启动**：连接进入 `kStateConnected` 时通过 `markPeerActivity()` 首次 arm keepalive 定时器。
- 客户端：握手包处理后置为 connected（`connection_impl.cpp:1048`）随即 `markPeerActivity(nowUs)`（`1057`）。
- 服务端/被动方：`kStateConnected` 转移后 `markPeerActivity(...)`（`connection_impl.cpp:617-618`）。

**空闲刷新**：任意收包在 connected 下调用 `markPeerActivity(nowUs)`（`connection_impl.cpp:683-684`），其行为（`3201-3206`）：
1. `m_lastActivityUs = nowUs`
2. `m_keepaliveMissedProbes = 0`
3. `armKeepaliveTimer(keepaliveIntervalMs())` 重新以完整间隔 arm。

**arm 定时器** `armKeepaliveTimer(delayMs)`（`connection_impl.cpp:3188-3199`）：
- 前置条件：`m_state == kStateConnected` 且 `enable_keepalive == true`，否则直接返回不 arm。
- `m_keepaliveTimer.stop()` 后 `start(delayMs > 0 ? delayMs : 1)`（即最小 1ms）。

**超时回调** `onKeepaliveTimeout()`（`connection_impl.cpp:3239-3280`），顺序：
1. 若非 connected 或 `enable_keepalive==false`，直接返回（`3241-3248`）。
2. `nowUs = MonotonicUs()`；若 `m_lastActivityUs==0` 则补设为 nowUs（`3250-3253`）。
3. **未真正空闲的重排**：`intervalUs = intervalMs*1000`；若 `nowUs < m_lastActivityUs + intervalUs`，说明期间有活动，按剩余时间 `remainUs/1000`（最小 1ms）重新 arm 并返回，不发探测（`3255-3261`）。
4. **达阈值断连**：`maxProbes = max(keepalive_probes,1)`；若 `m_keepaliveMissedProbes >= maxProbes`，记录 `UTP_ERR_TIMEOUT` 错误并 `abortConnection(UTP_ERR_TIMEOUT, UTP_ERR_TIMEOUT, "keepalive timeout")`，返回（`3263-3268`）。
5. **发送探测**：构造单字节 Ping 载荷，`sendPacket(UTP_TYPE_CTRL, payload, 1)`（`3270-3271`）。
   - 发送成功：`++m_keepaliveMissedProbes`；等待窗口 `timeoutMs = keepalive_timeout>0 ? keepalive_timeout : intervalMs`；`armKeepaliveTimer(timeoutMs)` 等待探测响应（`3272-3276`）。
   - 发送失败（如 WOULD_BLOCK）：`armKeepaliveTimer(10)` 短延迟重试，不增计数（`3279`）。

**判活闭环**：探测触发对端 ACK；ACK 到达 → 收包 → `markPeerActivity` 将 `m_keepaliveMissedProbes` 清 0 并按完整 interval 重排。若在 `keepalive_timeout` 窗口内无任何包到达，则下次 `onKeepaliveTimeout` 因 `nowUs >= m_lastActivityUs + intervalUs`（活动时间未刷新）走到步骤 4/5，累加丢失计数或断连。

**停止**：以下路径均 `m_keepaliveTimer.stop()`：连接建立前重置（`550`）、begin close sent（`3219`）、abortConnection（`connection_impl.cpp` abort 体内，`grep` 命中 `3164/3317/3344` 等各错误/关闭分支 `1129/1154/1169`）。

---

## ④ 不变量与规则（MUST / MUST NOT）

- MUST 仅在 `kStateConnected` 且 `enable_keepalive==true` 时 arm/发送探测（`connection_impl.cpp:3190-3195`、`3241-3248`）。
- MUST 在每次收包（connected 下）重置 `m_lastActivityUs` 与 `m_keepaliveMissedProbes=0`（`3201-3206`、`683-684`）。
- MUST 探测包为**仅含 Ping 帧**的 CTRL 包（`3270-3271`）。
- MUST NOT 重传已发出的 Ping 包（`packet_common.h:48` Ping 不在 retx 掩码；失败/丢失时发新探测，`3270`/`3279`）。
- MUST 连续丢失计数达到 `max(keepalive_probes,1)` 时断连，错误码 `UTP_ERR_TIMEOUT`，原因串 `"keepalive timeout"`（`3263-3266`）。
- MUST 每次探测的等待窗口在 `keepalive_timeout>0` 时为该值，否则回落为 `intervalMs`（`3274`）。
- MUST 探测间隔 `keepaliveIntervalMs()` 同时受本地配置与对端 `max_idle_timeout` 约束封顶（见 ⑤）。
- MUST NOT arm 时使用 0ms（下限被夹到 1ms，`3198`）。
- 探测发送失败不得累加丢失计数（`3272` 的 `++` 仅在 `status.ok()` 分支内）。

---

## ⑤ 参数与默认值（确切值 + 变量名）

配置字段（`cpp/include/utp/config.h:69-74`）：

| 变量名 | 类型 | 默认值 | 含义 / 引用 |
|---|---|---|---|
| `enable_keepalive` | `bool` | `true` | 保活总开关（`config.h:70`）。false 时 arm 直接返回、超时回调直接返回。 |
| `keepalive_interval` | `uint32_t` | `0` (ms) | 空闲探测间隔。0 表示由 `max_idle_timeout` 自动推算（`config.h:71`；`connection_impl.cpp:3177-3178`）。 |
| `keepalive_timeout` | `uint32_t` | `1500` (ms) | 单次探测等待响应超时窗口（`config.h:72`；`connection_impl.cpp:3274`）。 |
| `keepalive_probes` | `uint16_t` | `3` | 连续无响应最大探测次数，达此值断连（`config.h:73`；`connection_impl.cpp:3263-3264`）。 |
| `max_idle_timeout` | `uint32_t` | `30000` (ms) | 最大空闲超时阈值；`keepalive_interval==0` 时用作推算基数（`config.h:74`；`connection_impl.cpp:3178`）。 |

**间隔推算** `keepaliveIntervalMs()`（`connection_impl.cpp:3170-3186`）：
- 若 `m_ctx`/`config()` 为空返回硬编码 `1000`（`3172-3174`）。
- `localInterval = keepalive_interval>0 ? keepalive_interval : max(max_idle_timeout,1)`（`3177-3178`）。即 interval=0 时本地间隔取 `max_idle_timeout`（默认 30000ms）。
- `peerIdleTimeout = max(m_peerTP.max_idle_timeout, 1)`（对端下发的传输参数，`3180`）。
- `srttMs = m_rttStats.srtt()/1000`；`guardMs = max(3*srttMs, 50)`（`3181-3182`）。
- `peerSafeInterval = peerIdleTimeout > guardMs+1 ? peerIdleTimeout - guardMs : 1`（`3183`）。即在对端空闲超时前留出 `3*SRTT`（最少 50ms）的安全余量。
- 返回 `max(min(localInterval, peerSafeInterval), 1)`（`3185`）——取本地与对端安全值的较小者，下限 1ms。

其他常量：
- arm 延迟下限 `1`ms（`3198`）；重排最小值 `1`ms（`3259`）；发送失败重试延迟 `10`ms（`3279`）。
- `max_idle_timeout` 亦作为本端传输参数下发：`m_loaclTP.max_idle_timeout = cfg->max_idle_timeout`（`connection_impl.cpp:448`）。

---

## ⑥ 接口

私有方法（`connection_impl.h`）：
- `void onKeepaliveTimeout()`（`h:234`）— 定时器回调，绑定于 `m_keepaliveTimer.reset(...)`（`connection_impl.cpp:358`）。
- `uint32_t keepaliveIntervalMs() const`（`h:238`）— 计算生效间隔。
- `void armKeepaliveTimer(uint32_t delayMs)`（`h:239`）— arm/重排定时器。
- `void markPeerActivity(utp_time_t nowUs)`（`h:240`）— 刷新活动、清丢失计数、重排定时器。

对上层的可配置接口即 `Config` 的 5 个字段（⑤）。无公有 keepalive API（不可由应用手动触发单次探测）。

---

## ⑦ 当前实现边界

- 接收侧对 Ping 帧**无独立语义处理**，纯靠"非 ackOnly ⇒ ACK-eliciting"间接回 ACK（`connection_impl.cpp:969-971`、`1005-1019`）。若未来加入需响应内容的 Ping 变体需新增 `case kFramePing`。
- 判活以"任意收包刷新活动"为准，而非"探测的 ACK 被显式匹配"；探测包被 send control 跟踪，但 keepalive 逻辑本身不检查该 packno 是否被 ACK，仅看 `m_lastActivityUs` 是否被任何收包刷新。
- `keepalive_probes` 为 `uint16_t`，`m_keepaliveMissedProbes` 同为 `uint16_t`；计数上限受类型约束（正常远小于阈值）。
- interval=0（默认）时本地间隔等于 `max_idle_timeout`（30000ms），实际生效间隔常被 `peerSafeInterval` 拉低到"对端 idle − 3·SRTT"。若对端 idle 亦大，探测频率较低。**待确认**：是否需要在 interval=0 时对间隔再乘以一个 <1 的系数以更早探测（当前无此系数，仅靠 peerSafeInterval 的 guard）。
- `NetworkPath` 构造复用了 `keepalive_timeout` / `keepalive_probes` 作为**路径验证**探测的超时/次数参数（`connection_impl.cpp:340-341`）。这是命名/配置的复用，与空闲保活是两套独立机制，但共享同一组配置值——修改这些配置会同时影响路径验证探测。
- 探测发送失败仅以固定 10ms 退避重试，无指数退避，也不设失败上限（`3279`）。

---

## ⑧ 与 doc/ 差异

- `doc/连接与异常处理流程.md:188-204`（§7 Keepalive）描述与代码一致：connected 后启动定时器、空闲达间隔发 Ping、`keepalive_timeout` 内无响应计数加 1、达 `keepalive_probes` 以 timeout 断开。**差异**：doc 未提及间隔受对端 `max_idle_timeout − 3·SRTT` 封顶的 `peerSafeInterval` 逻辑（`connection_impl.cpp:3180-3185`），也未说明 interval=0 时回落为 `max_idle_timeout`。以代码为准。
- `doc/frame/帧重传.md:70-113`（PING 不重传）与代码一致（`packet_common.h:48` Ping 不在 retx 掩码）。doc 引用的是 lsquic 参考实现代码片段，本实现行为等价（丢失发新 Ping，不重传旧包）。
- `doc/默认值与调参指南.md`：**未收录** keepalive 相关默认值（grep 无 keepalive/保活/30000/1500 命中）。建议补充 ⑤ 表中的 5 项默认值。
- doc 未记录 `enable_keepalive` 关闭时的短路行为、arm 的 1ms 下限、发送失败 10ms 退避、以及 `NetworkPath` 复用 keepalive 配置这一交叉点。

---

## ⑨ 依赖

- **状态机**：依赖 `m_state == kStateConnected`（`ConnectionImpl` 状态枚举）作为唯一激活条件。
- **定时器**：`ev::EventTimer m_keepaliveTimer`（`connection_impl.h:350`），绑定于事件循环 `m_ctx->loop()`（`connection_impl.cpp:358`）。
- **收包路径**：`onUdpPacket` / 帧处理主循环调用 `markPeerActivity`（`connection_impl.cpp:683-684`），是空闲刷新的唯一入口。
- **发送路径**：`sendPacket(UTP_TYPE_CTRL, ...)`（`connection_impl.cpp:2594` 起）+ `SendControl` 调度/跟踪；Ping 帧掩码由 `UTP_FRAME_RETX_MASK`（`packet_common.h:43`）决定是否重传。
- **RTT 统计**：`m_rttStats.srtt()` 用于计算 guard 余量（`connection_impl.cpp:3181`）。
- **对端传输参数**：`m_peerTP.max_idle_timeout` 用于封顶间隔（`connection_impl.cpp:3180`）；本端 `m_loaclTP.max_idle_timeout` 下发给对端（`448`）。
- **错误/断连**：`recordConnectionError` + `abortConnection`（`connection_impl.cpp:3265-3266`），错误码 `UTP_ERR_TIMEOUT`（`cpp/include/utp/errno.h`）。
- **帧编解码**：`cpp/src/proto/frame.h`（`kFramePing`）、`cpp/src/proto/packet_in.cpp`（Ping 帧长解析）、`cpp/src/proto/packet_common.h`（retx 掩码）。
