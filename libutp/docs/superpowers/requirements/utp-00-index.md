# utp 现有实现需求 — 索引 / 依赖图 / 统一 spec 拆分

- 日期:2026-07-27
- 来源:反推自 `cpp/`(**代码为唯一 ground truth**),`doc/` 仅交叉参考。
- **目标已定:用 C11 在 `c/` 重写传输核心**(不再改 `cpp/`),再 C++ 薄封装。本 12 份需求 = **C 要复刻的行为蓝图**,非 cpp 补丁清单。
- **单一入口见 [`../00-C-IMPLEMENTATION-ROADMAP.md`](../00-C-IMPLEMENTATION-ROADMAP.md)**(总纲 + 决策汇总 + 迁移顺序 + P0–P3 分期)。
- 目的:把现有 utp 实现落成需求基线,消除 punch spec 里"复用现有 X"的二义性,并给出统一 spec 拆分。

---

## 1. 12 份需求文档(现有实现基线)

| # | 模块 | 文件 | 主要代码 |
|---|---|---|---|
| 01 | 包头 + 包类型 + 帧体系 | `utp-01-packet-frame.md` | `proto/` |
| 02 | 连接生命周期 / 状态机 / CID / HandshakeDone | `utp-02-connection-lifecycle.md` | `context/connection_impl`、`context_impl` |
| 03 | 流:多路复用 / StreamID / FIN / 优先级 / 零拷贝接收 | `utp-03-stream.md` | `context/stream_impl`、`util/ring_buffer` |
| 04 | 可靠性:ACK / 丢包检测 / 重传 / PTO | `utp-04-reliability-ack.md` | `context/send_ctl`、`util/ack_info` |
| 05 | 流量控制(连接级 + 流级) | `utp-05-flow-control.md` | connection_impl、frame/max_data 等 |
| 06 | 拥塞与速率:BBR/CUBIC/pacer/bw/rtt | `utp-06-congestion.md` | `congestion/` |
| 07 | 路径验证 / 迁移 / 抗放大 | `utp-07-path-validation.md` | `util/network_path`、frame/path |
| 08 | Keepalive | `utp-08-keepalive.md` | connection_impl、config |
| 09 | MTU / PLPMTUD | `utp-09-mtu.md` | `mtu/` |
| 10 | 加密 / 密钥调度 / 票据 / 0-RTT / 重放 | `utp-10-crypto-0rtt.md` | `crypto/` |
| 11 | socket 收发 / 地址 / mmsg | `utp-11-socket.md` | `socket/` |
| 12 | 公共 API / 配置 / 错误码 / 回调 | `utp-12-public-api.md` | `include/utp/` |

---

## 2. 依赖图(粗粒度)

```
12 公共API/配置 ── 贯穿所有
        │
01 包/帧 ── 被 02/03/04/05/07/10 依赖(线上格式)
        │
11 socket ── 承载 01(收发字节)
        │
02 连接/CID/HandshakeDone ──┬─ 03 流 ── 05 流控
                            ├─ 04 可靠性/ACK ── 06 拥塞
                            ├─ 07 路径验证/抗放大 ── 09 MTU
                            ├─ 08 keepalive
                            └─ 10 加密/0-RTT
```
关键耦合:07 路径验证的 challenge 超时/probes **复用 08 keepalive 配置**;04 丢包重排阈值**复用 ACK 协商值**;09 MTU 探测由 07 `onPathValidated` 触发。

---

## 3. 统一 spec 拆分表

| Spec | 覆盖需求模块 | 状态 |
|---|---|---|
| **utp-core**(现有传输基线) | 01/02/03/04/05/06/09/11/12 | 已实现,本次反推为需求 |
| **NTRS rendezvous**(`specs/2026-08-22-libutp-ntrs-rendezvous-half-association.md`) | 在 core 之上新增 RENDEZVOUS/FrameRendezvous、注册、保活、候选开洞与握手归并；**依赖并修改** 02/07/08/09/10/11 | 设计定稿，待实现 |
| **NTRS 认证** | 基于半连接重新设计服务端认证、凭据保护和根密钥轮换；不复用已删除的常驻连接握手。 | 待设计 |
| **crypto**(后续) | 扩展 10:**peer 身份**/Ed25519/显式 Finished/全包加密+CID 混淆(`doc/全包加密...` 是其目标方案,未实现)。加密恢复 0-RTT 已在 utp-10 §10 确定两消息目标，尚待实现 | 未开始 |
| **relay**(后续) | 双对称 / UDP 阻断兜底转发 | 未开始 |

**punch spec 里"复用现有 X"→ 对齐到 core 需求条目**:

| punch 引用 | 对应 core 需求 |
|---|---|
| Initial/Handshake/0RTT/CTRL、帧类型、未知帧报错 | utp-01 |
| CID 模型、连接状态机、HandshakeDone、被动创建、`(dcid==scid,peer ip:port)` 解复用 | utp-02 |
| PATH_CHALLENGE/RESPONSE、抗放大 `3×+credit` | utp-07 |
| keepalive 参数(间隔/probes/timeout) | utp-08 |
| MTU floor / PLPMTUD | utp-09 |
| X25519/HKDF/AES-GCM、token、resumption、重放窗口、统一 0-RTT API | utp-10 |
| 一个 Context 一个 socket/端口、SO_REUSEPORT | utp-11 |
| 可靠重传/ACK | utp-04 |

---

## 4. C 实现内建决策

> 这些决策约束 C 实现，不要求修改冻结的 C++ 实现。汇总权威表见
> [总纲 §5.1](../00-C-IMPLEMENTATION-ROADMAP.md)。

**决策状态(2026-07-27 后续更新)**:C1 ✅ 不设无条件 SO_REUSEPORT / C2 ✅ 普通 1-RTT 由 client→server HandshakeDone 帧驱动 promote；加密恢复 0-RTT 采用 server→client 两消息 `HANDSHAKE_DONE`（详见 utp-10 §10.6）/ C3 ✅ credit 3×MTU + 按候选地址额度 / C4 ✅ MTU floor 1280 / C5 ✅ 公共 API 直接返负错误码(C 原生,`utp_status_t` 已负值)。下文各条描述的"现状/矛盾"是 **cpp 的行为记录**,"须"改为 **C 实现要内建的目标**。

**C1 [P0] SO_REUSEPORT 与 scid 解复用矛盾**
- 现状:`bind()` **无条件设 `SO_REUSEPORT`**(`socket/udp.cpp:225`);一个 Context = 一个 socket/端口(`context_impl.h:226`)。
- NTRS 服务端的受控多 worker 例外见半连接规格 §8；普通 Context 不得默认启用 `SO_REUSEPORT`。
- **矛盾**:无条件 REUSEPORT 下,两个 Context 可绑同端口 → 内核跨 socket 负载均衡 → 误投。**须二选一**:改代码(去掉/收窄 REUSEPORT)或改 punch 解复用推理。

**C2 [P0] connected 触发 / HandshakeDone 方向**
- 现状(utp-02):HandshakeDone 是 **client→server**;服务端被动连接**惰性创建**——`accept()` 只发 server hello,真正 `ConnectionImpl` 在**收到 client 的 HandshakeDone 回声后**才建(`context_impl.cpp:1608-1642`);`initPassive` 一旦调用即置 `kStateConnected`(`:617`)。
- **C 目标**:普通 1-RTT 仍按 client→server HandshakeDone 驱动惰性建连；加密恢复 0-RTT 是明确例外，服务端验证后发送 early_s2c 加密 `HANDSHAKE_DONE`，客户端验证后立即 connected。两者均不得泛化为“任一非 Initial 包即 promote”。

**C3 [P1] 抗放大:credit 值 + 每路径 vs 整连接**
- 现状(utp-07):`kPathValidationSendCredit = 256`;`m_bytesIn/out` 是**整连接累计、迁移不清零**(非 RFC9000 每路径额度)。
- **须**:(a) 改常量 256→3×MTU;(b) **新增每候选地址的收/发字节跟踪**(现有整连接模型不够);(c) 明确迁移时是否清零。

**C4 [P1] MTU / padding 口径不统一(四个数)**
- 现状:MTU 模块 base=1400 / floor=1280 / cap=1500(utp-09);`connect0Rtt` 单包上限**硬编码 1280**(`context_impl.cpp:594`);`doc` 提 Padding 1260(代码无此常量,utp-01)。
- **须统一一条口径**。握手、打洞和 RENDEZVOUS floor 对齐 1280(IPv6 min,与现有 0-RTT 上限一致)，连接后 PLPMTUD 走 1280→1400→1500。

**C5 [P1] 公共 API 返回值语义**
- 现状(utp-12):公共 API 实际 **成功 0 / 失败 -1 + `utp_get_last_error()`**(`context.cpp:17` `NormalizePublicStatus`),非头注释/doc 说的"返回错误码";`createStream` 返回流 ID 或 -1。
- 新增公开 API 必须遵循同一返回值约定。

---

## 5. "代码 ≠ doc" 偏差汇总(doc/ 已过时/不实之处,以代码为准)

| doc 说 | 代码实为 | 出处 |
|---|---|---|
| 全包加密 + opaque CID(SipHash)+ ChaCha20 | 未实现;现状 = 20B **明文头**作 AAD + payload AES-128/256-GCM + HKDF v2 转录派生,无 CID 混淆/无 ChaCha20 | utp-10 |
| 加密 0-RTT 可用 | **C++ 现状**为显式禁用(`connect0RttWithState` 加密→`NOT_IMPLEMENTED`)，仅非加密 0-RTT；**C 版目标**为 utp-10 §10 的两消息恢复流程 | utp-10 |
| PN-space(Init/Hsk/App)分离 | **未落地**,单一 App 语义;`LostAckInit/Hsk` 等定义未置位 | utp-04 |
| stream_id 为 u64 | **u32**(帧与 API 均 u32) | utp-03 |
| BBR 若干常量待配置化 | 多数已配置化;真正未配置化的是 `m_maxCwnd`/MinMax 窗口/burst_tokens 等;`bbr_init_cwnd_mss` **16(config) vs 32(doc/无Config默认)不一致** | utp-06 |
| mmsg 待评估 | 已用 `recvmmsg`+`sendmmsg`(GSO/GRO 未做) | utp-11 |
| Padding 填到 1260 | 无此常量 | utp-01 |
| 主动方包号从 0 | `m_packetNumber{1}`,`pn==0` 直接丢弃 | utp-02 |

**模块内部默认值/死代码疑点**(实现时清理):AckFrequency 两套默认(构造 10/3/150 vs normalize 5/3/25);流控窗口 TransportParams 内联 64/16/16MiB 被 Config 8MiB/256KiB 覆盖;`kMaxDataUpdateStep` 等疑似死代码;`StreamsBlocked(7)/MaxStreams(8)/Ping(5)` 有枚举无完整帧类;`abortConnection` 关闭收敛(1×PTO+立即通知)与 `close()`(3×PTO+排空后通知)不一致(潜在 bug);`IP_PKTINFO` 从未启用(localAddress 永远回退);Windows `SetReusePort`=`SO_REUSEADDR`、`GetIPPktInfo` 无 return UB。

---

## 6. 下一步(C 迁移,详见 [总纲 §4/§7](../00-C-IMPLEMENTATION-ROADMAP.md))

1. **C1–C5 已定稿并内建进总纲 §5.1**,实现时照做,无需再"收口 cpp"。
2. **立即下一步 = 迁移第 2 步 proto 模块**(包头 + 帧编解码,总纲 §7 的 task #13–#19):纯值模块、零分配、可单测。
3. 之后按迁移顺序:地址/时间 → crypto 封装 → socket/事件循环/拥塞/MTU → stream/connection/context。
4. crypto / relay 各自另立 spec(依赖 utp-10 / 07);punch(P1)在 connection/context 就绪后叠加。
