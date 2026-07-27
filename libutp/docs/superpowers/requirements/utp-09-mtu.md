# utp-09 MTU 探测 / DPLPMTUD 需求文档

> 依据：C++ 实现为唯一 ground truth。主要文件 `cpp/src/mtu/mtu.cpp`、`cpp/src/mtu/mtu.h`、`cpp/include/utp/config.h`，调用点在 `cpp/src/context/send_ctl.cpp`、`cpp/src/context/connection_impl.cpp`。doc/ 仅交叉参考，不一致以代码为准（见 §8）。

---

## 1. 职责与边界

`MtuDiscovery`（`cpp/src/mtu/mtu.h:31`）是单条连接路径上的 DPLPMTUD 风格路径 MTU 探测控制器。职责：

- 维护当前可用路径 MTU（`m_currentMtu`）及可发送最大包体（payload）字节数，供发送侧做分片/预算决策。
- 通过主动发送“探测包”（PING + PADDING 填充到目标大小）向上探测更大 MTU；根据 ACK/丢失反馈收敛。
- 检测“黑洞”（大包持续丢失）并回退到安全 MTU 后重启探测。

边界（MtuDiscovery 只是纯状态机，不做 I/O）：

- 不负责发送/接收数据包、不加解密、不计时器注册。探测包的实际构造与发送在 `SendControl::onCanWrite`（`cpp/src/context/send_ctl.cpp:550`）中完成；ACK/丢失/超时事件由 `SendControl` 回调进来。
- 不做内核 PMTU 交互（`IP_MTU_DISCOVER` 等在 `cpp/src/socket/util.cpp` 属 socket 模块，与本控制器解耦）。
- 头部开销换算（IP/UDP/UTP 头）由本类静态函数完成，但 MTU 值本身的语义是“IP 层数据报总长（含 IP+UDP 头）”。

---

## 2. 机制

### 2.1 MTU 与包体大小换算

- `PacketSizeFromMtu(mtu, family)`（`cpp/src/mtu/mtu.cpp:342`）：`packetSize = mtu - (ipHeader + UDP_HEADER_SIZE)`，其中 IPv4 头 20B、IPv6 头 40B、UDP 头 8B。若 `mtu <= overhead` 返回 0。这里的 `packetSize` 是 UTP 层可写入 UDP payload 的字节数（含 20B UTP 头）。
- `MtuFromPacketSize(...)`（`cpp/src/mtu/mtu.cpp:352`）：逆运算。
- `currentMaxPacketSize()`（`:165`）= 由 `m_currentMtu` 换算；`absoluteMaxPacketSize()`（`:170`）= 由 `m_ceilingMtu` 换算。
- `MinimumSupportedMtu(family)`（`cpp/src/mtu/mtu.cpp:89`）= `ipHeader + UDP_HEADER_SIZE + UTP_HEADER_SIZE + 1`（至少能装下 20B UTP 头 + 1 字节）。`UTP_HEADER_SIZE = 20`（`cpp/src/proto/proto.h:18`）。

### 2.2 探测策略：梯队 + 二分 + 稳定

三阶段状态 `ProbePhase`（`cpp/src/mtu/mtu.h:62`）：`kProbePhaseLadder` / `kProbePhaseBinary` / `kProbePhaseStable`。

- **梯队阶段**：固定候选表 `kProbeLadderMtu = {1380, 1450, 1492, 1500}`（`cpp/src/mtu/mtu.cpp:18`）。`nextLadderTarget()`（`:372`）取第一个满足 `m_searchLowMtu < ladder <= min(m_ceilingMtu, m_mtuMax)` 且 `>= m_mtuMin` 的候选。
- **二分阶段**：梯队走完或探测包尺寸与梯队目标不符时进入。`nextBinaryTarget()`（`:393`）在 `(m_searchLowMtu, m_searchHighMtu]` 取中点；当 `m_searchHighMtu <= m_searchLowMtu + m_probeStep` 时视为收敛，不再产生新目标。
- **稳定阶段**：收敛完成，`m_currentMtu` 提交为最后确认可达的 MTU，`m_nextProbeTimeMs` 推到 `now + m_probeIntervalMs` 周期性重探。

`nextProbeMtu()`（`cpp/src/mtu/mtu.cpp:134`）综合上述：先尝试梯队目标，梯队耗尽再退到二分目标，否则返回 `m_searchLowMtu`（表示无更大目标）。

### 2.3 探测包构造

`BuildMtuProbePayload(packetSize, payload)`（`cpp/src/context/send_ctl.cpp:100`）：

- 载荷总长 = `packetSize - UTP_HEADER_SIZE`（去掉 20B UTP 头）。
- 第 0 字节写 `kFramePing`；其余用一个 PADDING 帧填满：`padding_length = (payloadSize - 1) - FRAME_PADDING_HDR_SIZE`（`FRAME_PADDING_HDR_SIZE = 3`，见 `cpp/src/proto/frame/padding.h:13`）。
- 探测包以 `PacketOutFlags::kPoMtuProbe`（`cpp/src/proto/packet_out.h:29`）标记，经 `sendPacket(UTP_TYPE_CTRL, ...)` 发出（`cpp/src/context/send_ctl.cpp:626`）。

### 2.4 黑洞检测与被动大包监测

- `onDataPacketAck(packetSize, now)`（`cpp/src/mtu/mtu.cpp:288`）/ `onDataPacketLoss(...)`（`:303`）：仅当 `packetSize + m_probeStep >= currentMaxPacketSize()`（即“接近当前 MTU 的大包”）才计入。ACK 刷新 `m_lastLargeAckMs` 并清零连续丢失计数；丢失累加 `m_largeLossStreak`。
- “大包相关”仅指携带 STREAM 帧的数据包（`IsMtuRelevantDataPacket`，`cpp/src/context/send_ctl.cpp:148`，排除探测包）。
- 黑洞触发条件（`onDataPacketLoss`）：`m_largeLossStreak >= m_blackholeLossThreshold`，且统计窗口 `m_blackholeLossWindowMs` 内连续，且近期无大包 ACK（`now > m_lastLargeAckMs + m_probeTimeoutMs*2`）。触发后回退到 `safetyMtu()`（= `m_mtuMin`，`cpp/src/mtu/mtu.cpp:412`），重置搜索窗、回到梯队阶段，并进入冷却期 `m_blackholeCooldownMs`。

---

## 3. 状态机 / 流程

### 3.1 生命周期

1. **init**（`cpp/src/mtu/mtu.cpp:41`，由 `ConnectionImpl` 在 `cpp/src/context/connection_impl.cpp:364` 调用）：从 `Config` 载入参数并做归一化/clamp，`m_currentMtu = m_mtuBase`，phase = Ladder，`m_nextProbeTimeMs = 0`（可立即探测）。
2. **onPathValidated**（`cpp/src/mtu/mtu.cpp:100`，唯一调用点在路径校验成功处 `cpp/src/context/connection_impl.cpp:3018`，即 `handlePathResponseFrame`）：把 `m_currentMtu` 重置回 `m_mtuBase`、清黑洞状态、清在途探测；若启用则重置搜索窗到 `[base, mtuMax]`、回到梯队阶段、`m_nextProbeTimeMs = now`；若禁用则把 ceiling/low/high 全钉在 base 并进入 Stable。
3. **周期探测**：由 `SendControl::onCanWrite`（`cpp/src/context/send_ctl.cpp:601`）驱动，且仅在连接 `kStateConnected` 后。每次 `onProbeTimeout` → `shouldProbe` → `nextProbeMtu` → 构造并发送 → `onProbeSent`。

> 注意：初始连接不依赖 `onPathValidated` 也能探测——`init()` 已把状态置为可探测（phase=Ladder、nextProbe=0）。`onPathValidated` 主要服务于路径迁移后的重探（待确认：是否在首次握手完成时另有调用，当前 grep 仅见路径响应处一个调用点）。

### 3.2 单次探测事件流

- `shouldProbe(now)`（`:175`）：`m_enabled && !m_hasInFlightProbe && now >= m_blackholeCooldownUntilMs && now >= m_nextProbeTimeMs && nextProbeMtu() > m_searchLowMtu`。
- `onProbeSent(packNo, probeMtu, now)`（`:189`）：`packNo==0` 拒绝；把 `probeMtu` clamp 到 `[searchLow+1, ceiling]`；置在途标记与截止时间 `now + m_probeTimeoutMs`；若梯队目标不匹配则切到 Binary。
- `onProbeAck(packNo, now)`（`:215`）：`m_searchLowMtu = max(searchLow, 探测MTU)`；刷新大包 ACK；若达到 `m_mtuMax` 或二分已收敛（`searchHigh <= searchLow + probeStep`）则提交 `m_currentMtu` 并进入 Stable、推迟到 `now + interval`；否则 `now + 1`（尽快下一轮）。
- `onProbeLost(packNo, now)`（`:252`）：把 `m_searchHighMtu` 收到 `探测MTU - 1`，同步压低 ceiling，切 Binary；若已收敛则提交 `m_currentMtu = m_searchLowMtu` 进 Stable，否则 `now + 1`。
- `onProbeTimeout(now)`（`:280`）：`now >= 截止时间` 时等价于 `onProbeLost`。

---

## 4. 不变量与规则（MUST / MUST NOT）

- MUST：MTU 值始终被 clamp 到 `[m_mtuMin, UTP_ETHERNET_MTU(1500)]`；`NormalizeMtu`（`:95`）下界为 `MinimumSupportedMtu`，上界 1500。
- MUST：`m_mtuMax = clamp(cfgMax, m_mtuMin, 1500)`；`m_mtuBase = clamp(cfgBase, m_mtuMin, m_mtuMax)`（`cpp/src/mtu/mtu.cpp:49-51`）。因此即使配置乱填也不会越界。
- MUST：同一时刻至多一个在途探测（`m_hasInFlightProbe`）；`shouldProbe` 有在途探测时返回 false。
- MUST：探测包 payload 首字节为 PING，其余为 PADDING；`packetSize <= UTP_HEADER_SIZE + 1` 时不构造（`send_ctl.cpp:616`、`mtu.cpp` 换算返回 0 的分支）。
- MUST：非探测数据包线上长度 `> currentMaxPacketSize` 时拒绝发送（`connection_impl.cpp:2666`）；探测包只受 `absoluteMaxPacketSize`（ceiling）约束（`:2670`）。
- MUST：探测收敛/黑洞回退后提交的 `m_currentMtu` 必须是“最后确认可达”的 MTU（ACK 提升 low、丢失压低 high，最终取 low）。
- MUST NOT：`onProbeAck/Lost` 处理与在途探测 `packNo` 不一致的事件（直接返回 false）。
- MUST NOT：`packNo == 0` 作为合法探测（`onProbeSent` 拒绝）。
- MUST NOT：在 `m_blackholeCooldownUntilMs` 或 `m_nextProbeTimeMs` 之前发起探测。
- 规则：仅“接近当前 MTU 的大包”（`packetSize + probeStep >= currentMaxPacketSize`）且携带 STREAM 帧的数据包才参与黑洞统计。
- 规则：黑洞判定要求连续丢失达阈值 且 无近期大包 ACK（拥塞短时丢包不误判为黑洞）。

---

## 5. 参数与默认值（确切值 + 变量名）

配置字段来自 `cpp/include/utp/config.h`，运行态变量来自 `cpp/src/mtu/mtu.h`。init 时的默认兜底见 `cpp/src/mtu/mtu.cpp:41`。

| 语义 | Config 字段 (`config.h`) | 默认值 | 运行态变量 (`mtu.h`) | init 处理 |
|---|---|---|---|---|
| 是否启用 DPLPMTUD | `enable_dplpmtud` | `true` | `m_enabled` | 直接取，null→true |
| MTU 下限 | `mtu_min` | `1280` | `m_mtuMin` | `NormalizeMtu(cfgMin)` |
| MTU 上限 | `mtu_max` | `1500` | `m_mtuMax` | `clamp(cfgMax, min, 1500)` |
| 初始/基准 MTU | `mtu_base` | `1400` | `m_mtuBase` / `m_currentMtu` 初值 | `clamp(cfgBase, min, max)` |
| 探测间隔（秒） | `mtu_probe_interval` | `300`（=5 分钟） | `m_probeIntervalMs` | `max(1000, sec*1000)` ms |
| 二分收敛阈值/步长 | `mtu_probe_step` | `16` | `m_probeStep` | 0→16 |
| 单次探测超时(ms) | `mtu_probe_timeout` | `2000` | `m_probeTimeoutMs` | 0→2000 |
| 黑洞连续丢失阈值 | `mtu_blackhole_loss_threshold` | `3` | `m_blackholeLossThreshold` | 0→3 |
| 黑洞统计窗口(ms) | `mtu_blackhole_loss_window_ms` | `3000` | `m_blackholeLossWindowMs` | 0→3000 |
| 黑洞冷却期(ms) | `mtu_blackhole_cooldown_ms` | `5000` | `m_blackholeCooldownMs` | 0→5000 |

编译期常量（`cpp/src/mtu/mtu.h:17-23`）：`UTP_ETHERNET_MTU=1500`、`ETHERNET_MTU_MIN=1280`、`ETHERNET_MTU_MID=1400`、`IPV4_HEADER_SIZE=20`、`IPV6_HEADER_SIZE=40`、`UDP_HEADER_SIZE=8`；`UTP_HEADER_SIZE=20`（`proto.h:18`）；探测梯队 `{1380,1450,1492,1500}`（`mtu.cpp:18`）。

> 注意：`mtu_probe_interval` 单位是**秒**（默认 300s = 5 分钟），config 注释写“默认 5 分钟”一致；运行态换算为 ms 并有 1000ms 下限。

---

## 6. 接口

`MtuDiscovery` 公有接口（`cpp/src/mtu/mtu.h:33-59`）：

- 生命周期：`init(config, family)`、`onPathValidated(nowMs)`、`setAddressFamily(family)`。
- 查询：`enabled()`、`hasInFlightProbe()`、`pathMtu()`、`nextProbeMtu()`、`currentMaxPacketSize()`、`absoluteMaxPacketSize()`、`shouldProbe(nowMs)`。
- 事件回调（返回 bool 表示是否消费/状态变化）：`onProbeSent(packNo, probeMtu, nowMs)`、`onProbeAck(packNo, nowMs)`、`onProbeLost(packNo, nowMs)`、`onProbeTimeout(nowMs)`、`onDataPacketAck(packetSize, nowMs)`、`onDataPacketLoss(packetSize, nowMs)`。
- 静态换算：`MinimumSupportedMtu`、`NormalizeMtu`、`PacketSizeFromMtu`、`MtuFromPacketSize`。

外部消费点：

- 发送预算：`m_mtuDiscovery.currentMaxPacketSize()`（`connection_impl.cpp:1701,1913,2665`、`send_ctl.cpp:1437`）。
- 探测驱动：`SendControl::onCanWrite`（`send_ctl.cpp:606-638`）。
- ACK/丢失接线：`send_ctl.cpp:459-462`（ACK）、`:1222-1263,1335,1405-1417`（丢失/探测丢失 `handleLostMtuProbe`）。
- 统计导出：`stat.pmtu = m_mtuDiscovery.pathMtu()`（`connection_impl.cpp:3426`）。
- 握手前默认包体：`connection_impl.cpp:327-329`、`context_impl.cpp:196-197` 用 `NormalizeMtu(mtu_min)` + `PacketSizeFromMtu` 定初始下限包体。

---

## 7. 当前实现边界

- 只在连接进入 `kStateConnected` 后才探测（`send_ctl.cpp:602`）；握手阶段按 `mtu_min` 保守发送。
- 探测节流由 `m_nextProbeTimeMs` 单一时间点驱动，无独立定时器；实际触发依赖 `onCanWrite`（有可发送机会时）。
- 黑洞检测只统计携带 STREAM 帧、接近当前 MTU 的包；纯控制包/小包不参与。
- IP 头开销按固定 20/40 字节估算，不考虑 IPv6 扩展头/IP options。
- MTU 值来源完全依赖主动探测 + 被动大包监测，不读取内核接口 MTU 作为初值（`socket/util.cpp` 的 `GetMtuByIfname` 未接入本控制器——待确认是否有意）。
- `mtu_max` 硬顶 1500，无法探测 jumbo frame。
- family 变化通过 `setAddressFamily` 更新，但已计算的 MTU 数值不随 family 重新归一化（除非再次 init/onPathValidated）。

---

## 8. 与 doc/ 差异

- `doc/默认值与调参指南.md:143` 称梯队为 `1380,1450,1492,1500` —— 与代码 `kProbeLadderMtu`（`mtu.cpp:18`）一致。
- `doc/默认值与调参指南.md:144` 称 `mtu_probe_step` 是二分收敛停止条件 `high-low <= step` —— 与 `nextBinaryTarget`/`onProbeAck`/`onProbeLost` 的 `searchHigh <= searchLow + probeStep` 一致。
- `doc/默认值与调参指南.md:145` 称黑洞回退到“安全 MTU 后重启探测” —— 与 `onDataPacketLoss` 回退到 `safetyMtu()=m_mtuMin` 并回梯队阶段一致。
- `doc/设计实现文档.md:198-207`（3.8 Keepalive 与 MTU）列出“梯队探测/二分收敛/黑洞回退”均已实现 —— 与代码一致，无额外承诺。
- 未发现 doc 与代码矛盾之处；doc 对 `mtu_probe_interval` 单位（秒）、`mtu_base=1400` 等具体默认值描述较少，本文以代码为准补全。

---

## 9. 依赖

- **Config**（`cpp/include/utp/config.h`）：所有可调参数来源。
- **proto**（`cpp/src/proto/proto.h` 的 `UTP_HEADER_SIZE`、`kFramePing`、`FramePadding`/`padding.h`）：探测包构造。
- **Address**（`cpp/src/socket/address.h`）：地址族决定 IP 头开销。
- **SendControl**（`cpp/src/context/send_ctl.cpp`）：探测发送、ACK/丢失/超时事件的实际驱动者，以及大包监测接线。
- **ConnectionImpl**（`cpp/src/context/connection_impl.cpp`）：持有 `m_mtuDiscovery`（`connection_impl.h:353`），负责 init、family 更新、`onPathValidated` 调用、发送时的 MTU 上限校验、`stat.pmtu` 导出。
- **time**（`time::MonotonicMs/Us`）：时间源。
- **util/mm.cpp**：包缓冲池尺寸档位（`ETHERNET_MTU_MIN/MID`、`UTP_ETHERNET_MTU`）与本模块共享 MTU 常量。

---

_引用格式：`文件:行号` 为 `cpp/` 下相对路径。数值均取自代码。标注“待确认”处为无法从所读文件直接判定的项。_
