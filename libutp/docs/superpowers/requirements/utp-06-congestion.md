# UTP-06 拥塞控制与速率 需求文档（反推自 C++ 实现）

> Ground truth：`cpp/src/congestion/` 全部源码。本文档所有数值均取自代码；`doc/` 仅作交叉参考，凡不一致以代码为准并在 §8 标注差异。引用格式 `文件:行号`。不确定处标注“待确认”。
>
> 覆盖模块：拥塞控制统一接口、BBR v1、CUBIC、pacer、带宽采样（bw_sampler）、RTT 估计、minmax 滑窗。

---

## 1. 职责与边界

### 1.1 职责
- 提供**可插拔的拥塞控制抽象** `Congestion`（`congestion.h:27-47`），由发送控制层（`send_ctl`）按连接持有一个实例。
- 提供两种算法实现：
  - `BbrV1`（`bbr_v1.{h,cpp}`）：基于 lsquic 的 BBR v1 移植（函数注释逐一标注 `lsquic ...` 对应项，见 `bbr_v1.h:90-114`）。
  - `Cubic`（`cubic.{h,cpp}`）：CUBIC + Reno 下界的简化实现。
- 提供**速率整形器** `Pacer`（`pacer.{h,cpp}`）：基于突发令牌 + next_sched 时间戳的发包节流。
- 提供**带宽采样器** `BandwidthSampler`（`bw_sampler.{h,cpp}`）：为 BBR 生成 `BWSample{bandwidth, rtt, isAppLimited}`。
- 提供 **RTT 估计** `RttStats`（`rtt.h`，RFC 6298）与 **时间窗极值** `MinMax`（`minmax.{h,cpp}`，Kathleen Nichols windowed min/max）。

### 1.2 边界（不负责）
- 不负责 ACK 解析、丢包检测、重传定时（在 `send_ctl` 中，向本模块回调 `onAck/onLost/onTimeout` 等）。
- 不负责实际发包与定时器；`Pacer` 只计算“下一次可发时间”，由上层调度。
- `Congestion` 与 `Pacer` 之间**无直接耦合**：上层 `send_ctl` 用 `getPacingRate()` 的结果作为 `Pacer::packetScheduled` 的 `txcb` 返回值（`send_ctl.cpp:377-378`）。
- 算法选择由 `Config::cc_algorithm` 决定，不在本模块内（`send_ctl.cpp:235-238`）。

---

## 2. 算法与数据结构

### 2.1 统一接口 `Congestion`（`congestion.h`）
纯虚基类，事件驱动。关键数据结构 `PacketInfo`（`congestion.h:20-25`）：
```
uint64_t packetNo;      // 包序号
uint64_t sendTimeUs;    // 发送时刻(us)
uint32_t packetSize;    // 包字节数
void*    packetState;   // 指向 bw_sampler 的 BWPacketState
```

### 2.2 BBR v1（`bbr_v1.cpp`）
- **带宽估计**：`MinMax m_maxBandwidth`，窗口 = 10 轮（`bbr_v1.cpp:170` `m_maxBandwidth.init(10)`）。
- **ACK 聚合**：`MinMax m_maxAckHeight`，窗口 = 10 轮（`bbr_v1.cpp:171`），`updateAckAggregationBytes()`（`bbr_v1.cpp:589-607`）估计相对期望带宽的超额确认字节，加到目标 cwnd。
- **目标 cwnd**：`getTargetCwnd(gain) = gain * (minRtt * bytesPerSec)`（BDP × gain）（`bbr_v1.cpp:358-372`）。
- **pacing rate 计算** `calculatePacingRate()`（`bbr_v1.cpp:486-526`）：full-bw 时 `bw*pacingGain`；未 full-bw 且首次有 minRTT 时 `initCwnd/minRTT`；STARTUP 检测到丢包（且 `SLOWER_STARTUP+HAS_NON_APP_LIMITED`）降到 `bw*1.5`（`kStartupAfterLossGain`）。
- **cwnd 计算** `calculateCwnd()`（`bbr_v1.cpp:528-549`）：目标窗口叠加 ack 聚合，full-bw 后按 `min(target, cwnd+acked)` 增长，最终 `CLAMP(m_minCwnd, m_maxCwnd)`。
- **恢复窗口** `calculateRecoveryWindow()`（`bbr_v1.cpp:551-587`）：Conservation/Growth 两阶段，下界 `m_minCwnd`，最小段回落 `kMaxSegmentSize=1460`。
- **ACK 事务模型**：`onBeginAck` → 多次 `onAck`/`onLost` → `onEndAck`（`bbr_v1.cpp:234-329`）。`onEndAck` 内按轮次推进 round、更新带宽/minRTT、恢复态、增益循环、状态迁移、再计算 pacing/cwnd/recovery。

### 2.3 CUBIC（`cubic.cpp`）
- **慢启动**：`m_cwnd < m_ssthresh` 时每 ACK `cwnd += acked`（`cubic.cpp:78-82`）。
- **拥塞避免**：取 `max(cubicIncrement, renoIncrement)`（`cubic.cpp:86-89`）——CUBIC 曲线与 Reno 友好下界并存。
- **CUBIC 曲线**：`W(t) = C*(t-K)^3 + Wmax`，以“包”为单位计算（`cubicTargetCwnd()` `cubic.cpp:171-184`），`K = cbrt((Wmax - cwnd)/C)`（`cubic.cpp:164`）。`t` 含 `+ srtt`（`cubic.cpp:177-178`）。
- **丢包回退** `onLost()`（`cubic.cpp:92-105`）：快速收敛（`m_cwnd < m_lastMaxCwnd` 时 `lastMax = cwnd*(2-beta)/2`），`cwnd *= beta`，`ssthresh = cwnd`，重置 epoch。
- **超时** `onTimeout()`（`cubic.cpp:127-133`）：`ssthresh = max(cwnd*beta, minCwnd)`，`cwnd = minCwnd`。
- **epoch 管理**：`ensureEpoch()`/`resetEpoch()`（`cubic.cpp:144-169`）；`wasQuiet(inFlight==0)` 时重置 epoch（`cubic.cpp:114-120`）。
- 注意：CUBIC 的 `onBeginAck/onEndAck/onPacketSent/onLost(packetInfo参数)` 多为空实现，仅 `onAck/onLost/onTimeout/wasQuiet` 有逻辑。

### 2.4 Pacer（`pacer.cpp`）
- **突发令牌** `_burst_tokens`：init 与 inflight==0 且非 recovery 时补满为 **10**（`pacer.cpp:19,72`）。丢包事件 `lossEvent()` 清零（`pacer.cpp:104-107`）。
- **调度判定** `canSchedule(inflight)`（`pacer.cpp:47-59`）：有令牌或 inflight==0 → 可发；否则若 `_next_sched > _now + _clock_granularity` → 置 `LAST_SCHED_DELAYED` 且不可发。
- **调度推进** `packetScheduled()`（`pacer.cpp:61-102`）：消耗令牌优先；否则 `delay = txcb()`（上层给出的 pacing 间隔），据 delayed 状态推进 `_next_sched`，识别 app-limited / making-up。
- **探测调度** `canScheduleProbe()`（`pacer.cpp:109-112`）：令牌>1 或 inflight==0 或 `_next_sched > _now + txTime/2`。

### 2.5 BandwidthSampler（`bw_sampler.cpp`）
- 为每个已发包分配 `BWPacketState`（对象池 `Malo<BWPacketState>`，容量 **256**，`bw_sampler.cpp:18`），记录发送时的累计 sent/acked/lost 与上次 ACK 快照。
- `onPacketAcked()`（`bw_sampler.cpp:69-142`）同时算 **send rate** 与 **ack rate**，取较小者作为样本带宽（`bw_sampler.cpp:127-131`），`rtt = ackTime - sendTimeUs`。
- app-limited 标记：`appLimited()` 设 `BWS_APP_LIMITED` 且记录 `m_endOfAppLimitedPhase`（`bw_sampler.cpp:157-162`）；ACK 越过该包号后退出（`bw_sampler.cpp:91-94`）。
- 带宽单位见宏（`bw_sampler.h:20-31`）：`BandWidth.value` 为 **bits per second**；`BW_FROM_BYTES_AND_DELTA(bytes,us)=bytes*8*1e6/us`。

### 2.6 MinMax（`minmax.cpp`）
- 三样本 windowed min/max（`MINMAX_SAMPLES=3`，`minmax.h:13`），Kathleen Nichols 算法：`updateMin/updateMax` + `subwinUpdate` 维护窗口内 1/4、1/2 分段候选（`minmax.cpp:86-126`）。
- 值被 `clampToU32` 截断到 `uint32_t`（`minmax.cpp:17-23`）——**带宽/聚合值超过 4G(bit or byte) 会饱和**，见 §7 风险。

### 2.7 RttStats（`rtt.h`）
- RFC 6298 平滑：`srtt` 以 α-scaled（×8）内部存储，`ALPHA=1/8`（`ALPHA_SHIFT=3`），`BETA=1/4`（`BETA_SHIFT=2`）（`rtt.h:15-16,37-57`）。
- 首个样本初始化：`srtt = rtt<<3`，`rttvar = rtt<<1`，`minrtt = rtt`。
- 对外：`srtt()`（去 scale）、`rttVar()`、`minRTT()`。**注意 `minRTT` 只在 `update` 中单调取更小值，无老化/过期**（BBR 自身另有 min_rtt 过期逻辑）。

---

## 3. 状态机 / 相位

### 3.1 BBR 模式（`bbr_v1.h:23-28`）
`StartUp → Drain → ProbeBW → ProbeRTT`
- **StartUp**：`pacingGain = highGain(2.885)`，`cwndGain = highCwndGain`（`setStartupValues` `bbr_v1.cpp:331-335`）。连续 `m_nStartupRtts` 轮带宽增长 < `startupGrowthTarget` → full-bw（`checkIsFullBwReached` `bbr_v1.cpp:638-672`）。
- **Drain**：`pacingGain = drainGain = 1/highGain`；当 `inflight ≤ getTargetCwnd(1.0)` → 进 ProbeBW（`maybeExitStartupOrDrain` `bbr_v1.cpp:674-691`）。
- **ProbeBW**：8 相增益循环 `m_pacingGains`（默认 `{1.25,0.75,1,1,1,1,1,1}`），`updateGainCyclePhase` 按 minRtt 推进相位（`bbr_v1.cpp:609-636`）；进入时随机选起始相位并跳过下标 0（`enterProbeBWMode` `bbr_v1.cpp:775-793`）。
- **ProbeRTT**：`pacingGain=1.0`，驻留 `m_probeRttTimeUs`（默认 200ms），cwnd 降到 `getProbeRttCwnd()`（默认 `m_configMinCwnd`，或 BDP*0.75 若置 `PROBE_RTT_BASED_ON_BDP`）（`bbr_v1.cpp:693-733`）。

### 3.2 BBR 恢复态（`bbr_v1.h:30-34`，`updateRecoveryState` `bbr_v1.cpp:456-484`）
`NotInRecovery → Conservation → Growth → NotInRecovery`
- 丢包进入 Conservation（`recoveryWindow=0` 待重算，延长当前轮）；轮次开始转 Growth；无丢包且 `maxPackNo > m_endRecoveryAt` 退出。

### 3.3 BandwidthSampler / Pacer 内部标志
- Sampler：`BWS_CONN_ABORTED / BWS_WARNED / BWS_APP_LIMITED`（`bw_sampler.h:64-68`）。
- Pacer：`LAST_SCHED_DELAYED / DELAYED_ON_TICK_IN`（`pacer.h:19-22`）。

---

## 4. 不变量与规则（MUST / MUST NOT）

- **MUST** ACK 事务严格配对：`onBeginAck` 置 `BBR_FLAG_IN_ACK`，`onEndAck` 清除；`onAck` 断言处于 IN_ACK 中（`bbr_v1.cpp:236,247,290-291`）。重复 begin 会触发 `assert`。
- **MUST** 每个已发包在采样器中唯一登记一次；重复 `onPacketSent`（`packetState != nullptr`）被拒绝并告警（`bw_sampler.cpp:32-35`）。
- **MUST** ACK 时若 `ackTime <= lastAckAckTime` 则丢弃该 send/ack-rate 样本，避免除零/下溢（`bw_sampler.cpp:112-116`）；发送时刻不递增时 send rate 记为 `BW_INFINITE`（`bw_sampler.cpp:103-109`）。
- **MUST** cwnd 恒被 `CLAMP(m_minCwnd, m_maxCwnd)`（`bbr_v1.cpp:548`）；BBR 恢复窗口下界 `m_minCwnd`（`bbr_v1.cpp:586`）。
- **MUST** CUBIC cwnd 不超过 `kMaxCwnd = 2000*MSS`，回退不低于 `m_minCwnd`（`cubic.cpp:80,89,102`）。
- **MUST** Pacer 时钟单调不回退：`_now = max(now, _now)`（`pacer.cpp:32`）。
- **MUST** BBR `getCwnd`：ProbeRTT 用 probeRttCwnd；recovery 中（除 RATE_BASED_STARTUP 的 StartUp）取 `min(cwnd, recoveryWindow)`（`bbr_v1.cpp:220-232`）。
- **MUST NOT** 在 `m_maxBandwidth` 更新中把 app-limited 样本当作有效上界，除非其带宽已超过当前 max（`bbr_v1.cpp:428-430`）。
- **MUST NOT** 在 ProbeBW 增益<1 且 `DRAIN_TO_TARGET` 且 inflight 仍>目标时把增益设回 1.0（提前 return，`bbr_v1.cpp:628-631`）。
- **不变量**：`MinMax` 窗口内保持 3 个按时间/极值排序的候选；值溢出 `uint32_t` 会饱和（`minmax.cpp:17-23`）。

---

## 5. 参数与默认值（确切值 + 变量名）

### 5.1 BBR 可配置参数（构造函数校验，`bbr_v1.cpp:117-151`；配置项 `config.h:88-100`）
| 配置项 (Config) | 默认(config.h) | 成员变量 | 成员默认(header) | 生效范围/校验 |
|---|---|---|---|---|
| `cc_algorithm` | 0 (=BBR) | — | — | 0/1→BBR，2→Cubic（`send_ctl.cpp:235-238`） |
| `bbr_init_cwnd_mss` | **16** | `m_configInitCwnd` | `32*1460` | `max(1,cfg)*1460`；若 < minCwnd 则抬到 minCwnd |
| `bbr_min_cwnd_mss` | 4 | `m_configMinCwnd` | `4*1460` | `max(1,cfg)*1460` |
| `bbr_startup_high_gain` | 2.885f | `m_configHighGain` | 2.885f | 仅当 (1.0, 4.0] 生效 |
| `bbr_cwnd_gain` | 2.0f | `m_configCwndGain` | 2.0f | 仅当 [1.0, 4.0] 生效 |
| `bbr_startup_growth_target` | 1.25f | `m_startupGrowthTarget` | 1.25f | 仅当 (1.0, 2.0] 生效 |
| `bbr_startup_full_bw_rounds` | 3 | `m_configStartupRounds` | 3 | `max(1,cfg)` |
| `bbr_probe_rtt_ms` | 200 | `m_probeRttTimeUs` | 200000us | `max(50,cfg)*1000` |
| `bbr_min_rtt_expiry_ms` | 10000 | `m_minRttExpiryUs` | 10000000us | `max(1000,cfg)*1000` |
| `bbr_probe_rtt_multiplier` | 0.75f | `m_probeRttMultiplier` | 0.75f | 仅当 (0.0, 1.0] 生效 |
| `bbr_similar_min_rtt_threshold` | 1.125f | `m_similarMinRttThreshold` | 1.125f | 仅当 [1.0, 2.0] 生效 |
| `bbr_pacing_gains` | `{1.25,0.75,1,1,1,1,1,1}` | `m_pacingGains` | 同 | 非空则整体替换 |

### 5.2 BBR 硬编码常量（`bbr_v1.cpp:21-104`，尚未配置化）
- `kDefaultTCPMSS = kMaxSegmentSize = 1460`
- `m_maxCwnd = kDefaultMaxCongestionWindowPackets(2000) * 1460`（`bbr_v1.cpp:179`）
- `kDerivedHighGain 2.773f`、`kDerivedHighCWNDGain 2.0f`、`kStartupAfterLossGain 1.5f`（仅部分被引用）
- `kMaxV6PacketSize=1452`、`kMaxV4PacketSize=1472`；`kMaxOutgoingPacketSize=1452`、`kMaxIncomingPacketSize=1472`
- `startup_rate_reduction_multiplier_ = 0`（关闭 startup 降速分支）
- `FLAGS_quic_bbr_no_bytes_acked_in_startup_recovery = 0`、`FLAG_quic_bbr_one_mss_conservation = 0`
- getMinRtt 兜底 `25000us`（25ms）当无 RTT 采样（`bbr_v1.cpp:344-346`）

### 5.3 CUBIC 参数（`cubic.h:34-52`，构造校验 `cubic.cpp:16-34`）
| 配置项 | 默认 | 成员 | 校验 |
|---|---|---|---|
| `cubic_beta` | 0.7 | `m_beta` | 仅当 (0.0, 1.0) |
| `cubic_c` | 0.4 | `m_cubicC` | 仅当 (0.0, 2.0] |
| `cubic_init_cwnd_mss` | 32 | `m_initCwnd` | `max(1,cfg)*1460`，夹在 [minCwnd, kMaxCwnd] |
| `cubic_min_cwnd_mss` | 4 | `m_minCwnd` | `max(1,cfg)*1460`，≤ kMaxCwnd |
- 硬编码：`kDefaultMss=1460`、`kMaxCwnd=2000*1460`、`kDefaultInitCwnd=32*1460`、`kDefaultMinCwnd=4*1460`。
- pacing gain：recovery 时 100%，否则 125%（`cubic.cpp:53`）；srtt 兜底 25000us、下限 1000us（`cubic.cpp:135-142`）。

### 5.4 Pacer / Sampler / MinMax 常量
- `_burst_tokens` 初值/补满 = **10**（`pacer.cpp:19,72`）。
- Sampler 对象池 `Malo<BWPacketState>(256)`（`bw_sampler.cpp:18`）。
- `MINMAX_SAMPLES = 3`（`minmax.h:13`）；BBR 两个 MinMax 窗口均 = **10 轮**（`bbr_v1.cpp:170-171`）。
- RTT：α=1/8、β=1/4（`rtt.h:15-16`）。

---

## 6. 接口（拥塞控制抽象）

`class Congestion`（`congestion.h:27-47`），纯虚，`shared_ptr` 持有：
| 方法 | 语义 | 备注 |
|---|---|---|
| `onInit(RttStats*)` | 绑定 RTT 统计并复位状态 | send_ctl 传 `&conn->m_rttStats`（`send_ctl.cpp:242`） |
| `getPacingRate(int32_t inRecovery)` → bytes/s | 当前 pacing 速率 | 上层传入 recovery 标志；BBR 忽略参数用内部 pacingRate，CUBIC 用之调 gain |
| `getCwnd()` → bytes | 当前拥塞窗口 | 用于 `canSchedulePacket`（`send_ctl.cpp:336`） |
| `onBeginAck(ackTimeUs, inFlight)` | 开始一次 ACK 事务 | BBR 有实现；CUBIC 空 |
| `onAck(PacketInfo*, nowUs, appLimited)` | 单包被确认 | |
| `onLost(PacketInfo*)` | 单包判丢 | |
| `onPacketSent(PacketInfo*, inFlight, appLimited)` | 发包登记 | |
| `wasQuiet(nowUs, inFlight)` | 静默期通知 | BBR 仅日志；CUBIC 重置 epoch |
| `onEndAck(inFlight)` | 结束 ACK 事务（核心推进） | |
| `onLoss()` | 丢包事件（默认空） | CUBIC 未覆盖；send_ctl 调用（`send_ctl.cpp:1352`） |
| `onTimeout()` | RTO 超时（默认空） | CUBIC 覆盖；BBR 未覆盖 |

说明：基类为 `onLoss/onTimeout` 提供空默认实现（`congestion.h:45-46`），其余为纯虚。

---

## 7. 当前实现边界（已实现 / 待配置化 / TODO）

### 7.1 已实现
- BBR v1 完整四相状态机 + 两阶段恢复 + ack 聚合 + app-limited（`bbr_v1.cpp` 全文）。
- CUBIC 慢启动/避免/快速收敛/超时（`cubic.cpp` 全文）。
- Pacer 突发令牌 + 延迟补偿（`pacer.cpp`）。
- 带宽双速率采样、MinMax 滑窗、RFC6298 RTT（`bw_sampler.cpp`/`minmax.cpp`/`rtt.h`）。
- §5.1/§5.3 列出的 BBR/CUBIC 参数均已通过 `Config` 配置化。

### 7.2 待配置化 / 未启用（硬编码或标志关闭）
- BBR `m_maxCwnd`（2000 MSS）、MinMax 窗口轮数（10）、getMinRtt 兜底（25ms）——硬编码（`bbr_v1.cpp:179,170-171,345`）。
- BBR 一批行为标志默认全 0/未设置：`BBR_FLAG_SLOWER_STARTUP / RATE_BASED_STARTUP / DRAIN_TO_TARGET / ENABLE_ACK_AGG_IN_STARTUP / EXPIRE_ACK_AGG_IN_STARTUP / PROBE_RTT_BASED_ON_BDP / PROBE_RTT_DISABLED_IF_APP_LIMITED / PROBE_RTT_SKIPPED_IF_SIMILAR_RTT`（`onInit` 未置位，`bbr_v1.cpp:159-199`）。因此如 `getProbeRttCwnd` 恒走 `m_configMinCwnd` 分支、`calculatePacingRate` 的 startup 降速分支不触发（`startup_rate_reduction_multiplier_=0`）。
- `Pacer::_burst_tokens=10` 硬编码；无配置项（`pacer.cpp:19,72`）。
- Sampler 池容量 256 硬编码。

### 7.3 TODO / 空实现 / 风险点
- `BbrV1::wasQuiet()` 仅打日志，无静默处理（`bbr_v1.cpp:275-280`）——CUBIC 有对应逻辑，BBR 无，可能不一致。
- `BbrV1::onExitStartup()` 注释明确“仅用于统计，尚未实现”（`bbr_v1.cpp:757-762`）。
- BBR 未覆盖 `onTimeout()`；CUBIC 未覆盖 `onBeginAck/onEndAck/onLoss` 实质逻辑——两算法对同一事件集响应不对称（`cubic.cpp:62-66,122-125`；`congestion.h:45-46`）。
- MinMax 值 `clampToU32`：带宽（bit/s）与 ack 聚合字节数用 32 位存储，高带宽（>~4.29 Gbit/s 或聚合>4GB）会**饱和截断**（`minmax.cpp:17-23`）——高速链路精度风险，待确认是否可接受。
- `enterProbeBWMode` 每次进入都 `std::random_device + mt19937` 现场构造（`bbr_v1.cpp:781-783`）——每次 new 随机引擎，性能/熵源开销，待确认。
- `RttStats::minRTT` 无老化，仅单调下降（`rtt.h:49-51`）；BBR 依赖自身 min_rtt 过期，CUBIC 直接用 srtt。
- `updateRecoveryState` 中 `case Conservation` 后无 `break`，故意 fall-through 到 `Growth`（`bbr_v1.cpp:474-482`）——刻意为之但脆弱，待确认。

---

## 8. 与 doc/ 差异（以代码为准）

1. **`bbr_init_cwnd_mss` 默认值不一致**：`doc/默认值与调参指南.md:20` 写 **32**，`doc/设计实现文档`语境亦偏 32；但 `config.h:90` 实际默认 **16**。另外无 `Config` 时 BBR 成员默认为 `32*1460`（`bbr_v1.h:163`）——即“默认构造行为”与“默认 Config 值”本身也不一致（16 vs 32）。以代码为准：带 Config 时 16 MSS，不带 Config 时 32 MSS。
2. **“BBR 剩余关键常量配置化”缺口描述过时**：`doc/设计实现文档.md:269-271` 称 ProbeBW 增益循环、ProbeRTT BDP 倍率、相似 RTT 阈值“仍为固定/未参数化”；但代码已分别通过 `bbr_pacing_gains`、`bbr_probe_rtt_multiplier`、`bbr_similar_min_rtt_threshold` 配置化（`bbr_v1.cpp:142-150`，`config.h:98-100`）。真正未配置化的是 §7.2 所列项（maxCwnd、MinMax 窗口、burst_tokens、行为标志），doc 缺口清单需据此修订。
3. 其余 `bbr.md`/`cubic.md` 的算法性描述与代码方向一致（未逐条比对，待确认细节）。

---

## 9. 依赖

- **对内**：`RttStats`（`rtt.h`）被 BBR/CUBIC 依赖注入；`BandwidthSampler`+`MinMax` 被 BBR 依赖；`BandWidth` 宏（`bw_sampler.h:20-31`）。
- **对象池**：`util/malo.hpp`（`Malo<BWPacketState>`，`bw_sampler.h:14`）。
- **平台/类型**：`utp/platform.h`、`utp/types.h`（`utp_time_t`）、`utp/config.h`（`Config`）。
- **日志**：`logger/logger.h`（`UTP_LOGD/W/E`）。
- **协议**：`proto/packet_common.h`（`IsValidPackNo`，BBR 用于轮次/包号判定，`bbr_v1.cpp:14,253,297`）。
- **上层集成**：`context/send_ctl.{h,cpp}` 持有 `Congestion::SP m_congestion` 与 `Pacer m_pacer`，负责事件转发与算法选择（`send_ctl.cpp:235-242,300,356,377-378,455,477,1116,1352`）。
