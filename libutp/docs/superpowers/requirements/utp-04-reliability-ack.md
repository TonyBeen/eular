# UTP-04 可靠性：ACK / 丢包检测 / 重传 / PTO / 发送控制

> Ground truth：`cpp/` 源码。本文档由 C++ 实现反推得到；`doc/` 仅作交叉参考，凡与代码不一致以代码为准并在 §8 标注差异。
>
> 负责文件：
> - `cpp/src/context/send_ctl.{h,cpp}`（核心：`SendControl`）
> - `cpp/src/util/ack_info.{h,cpp}`（`AckInfo`）
> - `cpp/src/util/send_history.{h,cpp}`（`SendHistory`）
>
> 交叉参考：`cpp/src/proto/packet_common.h`（`UTP_FRAME_RETX_MASK`）、`cpp/src/proto/proto.h`（ACK 阈值宏）、`cpp/src/proto/packet_out.h`（帧 meta 标志）、`cpp/src/context/connection_impl.{h,cpp}`（RTT 统计、拥塞入口、ACK 生成）。

---

## 1. 职责与边界

`SendControl`（`send_ctl.h:97`）是连接级发送方可靠性核心，一个 `ConnectionImpl` 持有一个实例。职责：

- **发送记账**：记录已发送包、维护待确认队列、飞行字节/包数统计（`packetSent` `send_ctl.cpp:279`、`appendUnacked` `:1007`、`unackedRemove` `:1718`）。
- **ACK 处理**：解析后的 `AckInfo` 驱动确认回收、RTT 采样、拥塞事件、丢包检测（`onAckReceived` `:406`）。
- **丢包检测**：基于 FACK（重排序阈值）、early-retransmit、发送时间三类信号（`detectLosses` `:1198`）。
- **重传调度**：丢失包进入 `m_lostPackets`，在可写窗口批量重传（`retransmitLostBatch` `:758`、`retransmitLostPacket` `:1612`、`retransmitSplitStreamPacket` `:1426`）。
- **重传定时器 / PTO**：单一 `m_retransTimer`，按四种模式（Handshake / Loss / TLP / RTO）计算超时（`retransAlarm` `:1023`、`onRetransTimer` `:1088`）。
- **发送预算 / 准入**：`canSend` / `canTransmitPacket` / `canSchedulePacket` 结合 cwnd 与 Pacer（`:324`、`:343`、`:329`）。
- **自适应重排序阈值**：动态调整丢包检测的 reorder 阈值（`m_adaptiveReorder`，`:879`~`:972`）。
- **调度写出**：`m_scheduledPackets` FIFO 批量 `sendmmsg`（`schedulePacket` `:641`、`flushScheduledPackets` `:666`、`onCanWrite` `:550`）。

**不在本模块内（边界）**：
- **ACK 帧的生成与触发时机**（ack-eliciting 计数阈值、max_ack_delay、乱序快速 ACK）在接收侧/连接层（`receive_history.*`、`connection_impl.cpp`，见 §9）。本模块只**消费** `AckInfo`，不产生 ACK。
- **拥塞控制算法**（cwnd/pacing rate 的具体计算）在 `congestion/*`；本模块通过 `Congestion` 接口回调。
- **Packet number 分配、加解密、帧编码、MTU 探测算法本体**分别在 connection/crypto/proto/mtu 模块；本模块只调用。

`AckInfo`（`ack_info.h:18`）是纯数据载体：解析 ACK 帧的结果。`SendHistory`（`send_history.h:15`）只跟踪本端最大已发包号并做“发送间隙”告警。

---

## 2. 数据结构

### 2.1 AckInfo（`ack_info.h`）
| 字段 | 类型 | 语义 |
|---|---|---|
| `largest_ack_packno` | `utp_packno_t` | ACK 帧中最大被确认包号（`=0` 视为非法，见 §4） |
| `ack_delay` | `utp_time_t` | 对端上报的 ACK 延迟（us） |
| `range_size` | `uint32_t` | 有效 range 数量 |
| `ack_ranges` | `std::array<Range,256>` | ACK 范围数组，`Range{low,high}` 闭区间 |

`reset()`（`ack_info.cpp:11`）只清 `largest_ack_packno/ack_delay/range_size`，**不清空 `ack_ranges` 数组内容**（依赖 `range_size` 界定有效性）。

### 2.2 SendHistory（`send_history.h`）
| 字段 | 语义 |
|---|---|
| `_last_sent` | 最大已发包号（`largestPackNo()`） |
| `_warn_thresh` | 间隙告警阈值 |
| `_flags` | `SH_WARNED(1)` / `SH_GAP_OK(2)` |

`update(packno)`（`send_history.cpp:11`）：若 `_last_sent != packno-1` 且未告警且 `packno > _warn_thresh` 则打一次 WARN；随后仅在 `packno > _last_sent` 时前移 `_last_sent`（不回退）。

### 2.3 SendControl 关键成员（`send_ctl.h:162`~`:238`）
- 队列（`queue.h` TAILQ）：`m_unackedPackets`（待确认）、`m_scheduledPackets`（待发送 FIFO）、`m_lostPackets`（待重传）、`m_bufferedPackets[PriorityCount]`（按优先级缓冲）。
- 飞行统计：`m_bytesUnackedAll`、`m_nInflightAll`（含 ACK-only 等不可重传包）、`m_bytesUnackedRetrans`、`m_nInflightRetrans`、`m_bytesScheduled`、`m_bytesRetransTotal`。
- 包号/确认：`m_currentPackNo`、`m_largestAckedPackNo`、`m_largestAckedSentTime`、`m_largestSentAtCutback`、`m_maxRttPackNo`、`m_largestAck2ed`。
- 重传计数：`m_nConsecRtos`（RTO 退避指数）、`m_nHandshake`、`m_nTlp`。
- 定时/丢包：`m_retransTimer`、`m_lastSentTime`、`m_lastRtoTime`、`m_lossTo`（>0 触发 kLoss 模式）、`m_lossSignalsUs`（丢包信号时间戳，滑窗）。
- 重排序阈值：`m_reorderThresh` 与 `m_adaptiveReorder{baseThresh,currentThresh,consecutiveLossEvents,ackedSinceLoss,lastExpandUs}`。
- 可重传掩码：`m_retxFrames`（初始化为 `UTP_FRAME_RETX_MASK`）。
- 其他：`m_congestion`（`Congestion::SP`）、`m_pacer`、`m_flags`（`SendCtlFlags`）。

### 2.4 枚举
- `SendCtlFlags`（`send_ctl.h:26`）：位掩码。`Pace`、`WasQuiet`、`AppLimited`、`EcnEnabled`、`LostAckInit/Hsk/App`、`OneRttAcked` 等。
- `RetransmissionMode`（`send_ctl.h:59`）：`kHandshake` / `kLoss` / `kTlp` / `kRto`。
- `ExpireFilter`（`send_ctl.h:74`）：`kAll` / `kHandshakeOnly` / `kLastOnly`。

---

## 3. 流程

### 3.1 发送记账 `packetSent`（`:279`）
1. 计算包尺寸（`PacketSentSize`：加密时用 `encrypt_data_size`，否则 `data_size`，`:130`）。
2. `pkt->addSendAttempt(...)` 记录发送尝试。
3. `notePacketSent` → `m_sendHistory.update`（`packno==0` 跳过，`:317`）。
4. `m_congestion->onPacketSent(...)`。
5. `appendUnacked`：入队、置 `kPoUnAcked`、累加飞行统计；若含可重传帧则累加 retrans 统计。
6. 若 `pkt->frame_types & m_retxFrames`：`retransAlarm(sent_time)`；当 `m_nInflightRetrans==1` 置 `WasQuiet`。

### 3.2 调度写出 `onCanWrite`（`:550`）
1. `m_pacer.tickIn/tickOut`（RAII guard）。
2. `flushScheduledPackets(nowUs, kSendBatchCap=32, ...)` 发送已调度队列。
3. 若调度队列非空且下一个包不能发 → `scheduleWrite()` 并返回。
4. 预算 `sentBudget = m_nextLimit>0 ? m_nextLimit : UINT32_MAX`；循环 `retransmitLostBatch` 重传丢失队列直到预算/`canSend()` 耗尽。
5. 更新/递减 `m_nextLimit`；若还有丢失包但本轮没发出，`nextScheduleTime(1)`。
6. 仅当 `state()==kStateConnected` 才尝试 MTU 探测（`shouldProbe`/`nextProbeMtu`/`sendPacket(kPoMtuProbe)`，`:601`~`:638`）。

`flushScheduledPackets`（`:666`）注意：`batchCap = min(maxPackets, 1)` — **实际每次只准备 1 个包**（尽管数组容量 `kSendBatchCap=32`）。带 `kPOLTrackOnSend` 的包发送后才纳入 unacked+拥塞记账；否则回收（`putPacketOut`）。

### 3.3 ACK 处理 `onAckReceived`（`:406`）
1. 合法性校验 `IsAckInfoValid`（见 §4）；不合法返回 `UTP_ERR_FRAME_FORMAT_ERROR`。
2. 前移 `m_largestAckedPackNo`；计算 `ackProgress`。
3. 处理 `WasQuiet` → `m_congestion->wasQuiet`；`onBeginAck`。
4. 遍历 `m_unackedPackets`，对 `IsPackNoAcked` 命中的包：
   - 计数、记录本轮最大 acked 及其 sent_time；`m_congestion->onAck`；
   - MTU 探测/数据包 ACK 回调（`onProbeAck`/`onDataPacketAck`）；
   - `kFrameHandshakeDone` → `onHandshakeDoneFrameAcked`；`stream_data_size>0` → `onStreamPacketAcked`；
   - `unackedRemove` + `destroyPacket`。
5. `onEndAck`。
6. **RTT 采样**（`:480`~`:511`）：`sampleRtt = nowUs - largestAckedSentTime`；`ackDelay = min(ackInfo.ack_delay, peerMaxAckDelayMs*1000)`；`ackDelay>0 && sample>ackDelay` 时扣减；更新 `m_conn->m_obsRttUs/VarUs`（EWMA：var 权重 3/4、srtt 权重 7/8）与 `m_conn->m_rttStats.update`。
7. 若本轮有 ACK：清零 `m_nConsecRtos/m_nHandshake/m_nTlp/m_nextLimit`。
8. 若 unacked 非空且 (`hasAcked || ackProgress`)：`detectLosses`；无丢包且有 ACK 则 `updateReorderThresholdOnAck`；`retransAlarm`；有丢包则 `nextScheduleTime(1)`。
9. 若 unacked 空：`m_lossTo=0`、停表。
10. 若 `m_nInflightRetrans==0` 置 `WasQuiet`。

### 3.4 丢包检测 `detectLosses`（`:1198`）
遍历 `m_unackedPackets` 中 `packno <= m_largestAckedPackNo` 的包（跳过已 `kPoLossRecorded`），依次判定三类丢失信号：
1. **FACK / 重排序阈值**（`:1219`）：`pkt.packno + m_reorderThresh < m_largestAckedPackNo` → 丢失。MTU 探测包走 `handleLostMtuProbe`，其余走 `handleRegularLostPacket` 并置 `kPOLFacked`。
2. **early retransmit**（`:1239`）：存在 `largestRetxPacketNo` 且当前包可重传、非 MTU 探测、且 `largestRetxPackNo <= m_largestAckedPackNo` → 丢失；若 `srtt>0` 设 `m_lossTo = srtt/4`（触发后续 kLoss 模式）。
3. **按发送时间**（`:1256`）：`srtt>0 && m_largestAckedSentTime > pkt.sent_time + srtt` → 丢失。
- 若 `largestLostPackNo > m_largestSentAtCutback` → `onLossEvent`（拥塞减窗 + pacer.lossEvent + 更新 cutback 基线）。
- 若有丢包：`recordLossSignal` + `updateReorderThresholdOnLoss`。

`handleRegularLostPacket`（`:1374`）：命中 ACK 帧时置 `LostAckApp`；`onLost`；从 unacked 移除；置 `kPoLost|kPoLossRecorded|kPoResetPackNo`；`loss_chain=self`；入 `m_lostPackets`。

### 3.5 重传定时器与 PTO
- `getRetransmissionMode`（`:1074`）优先级：**Handshake（有未确认握手包且未 connected）> Loss（`m_lossTo!=0`）> TLP（`m_nTlp<2`）> RTO**。
- `retransAlarm`（`:1023`）：unacked 空则停表；否则按模式取 delay，封顶 `MAX_RTO_DELAY=60s`，转 ms（`>=1`）后 `start`。
- `onRetransTimer`（`:1088`）按模式：
  - kHandshake → `expireUnacked(kHandshakeOnly)`。
  - kLoss → `detectLosses`。
  - kTlp → `m_lastRtoTime=now`、`++m_nTlp`、`expireUnacked(kLastOnly)`（只补发最后一个可重传包）。
  - kRto → 若 `(OneRttAcked) || now-m_lastRtoTime>=rto`：`++m_nConsecRtos`、`m_nextLimit=MAX_RESUBMITTED_ON_RTO(2)`、`m_congestion->onTimeout()`；再 `expireUnacked(kAll)`。
  - 末尾统一 `retransAlarm(now)` 重设。

### 3.6 延迟计算
- `calculateHandshakeDelay`（`:1129`）：`srtt==0→150ms`，否则 `srtt*1.5`（下限 10ms）；再 `<< min(m_nHandshake,8)`（指数退避），`m_nHandshake++`。
- `calculateTlpDelay`（`:1147`）：`srtt(==0→INITIAL_RTT=333.333ms)`；`m_nInflightAll>1→10ms`，否则 `srtt*1.5 + peerAckMaxDelayMs*1000`；下限 `2*srtt`。
- `calculatePacketRto`（`:1168`）：`srtt==0→DEFAULT_RETX_DELAY=500ms`，否则 `srtt + 4*rttvar`（下限 `MIN_RTO_DELAY=200ms`）；再 `*(1<<min(m_nConsecRtos,MAX_RTO_BACKOFFS=10))`。

---

## 4. 不变量与规则（MUST / MUST NOT）

- **MUST** ACK 合法性（`IsAckInfoValid` `:65`）：`range_size∈[1,256]`；`largest_ack_packno!=0` 且 `<= largestSent`；每个 range `low!=0 && low<=high && high<=largestSent`；range 之间不得重叠；所有 range 的最大 `high` 必须等于 `largest_ack_packno`。任一不满足则拒绝整帧。
- **MUST** 重传包使用新 packet number：丢失包置 `kPoResetPackNo`，重传前 `RewritePacketNumber`（`:794`、`:1640`）；加密包在改号或密文丢弃后 **MUST** 重新加密（`:798`、`:1648`）。
- **MUST NOT** 重传 `UTP_FRAME_RETX_MASK` 之外的帧：**ACK、PADDING、PING 不重传**（掩码注释掉 `kFTBitAck/kFTBitPadding/kFTBitPing`，`packet_common.h:43`）。ACK 丢失改为置 `LostAckApp` 标志由上层重新生成新 ACK（`:1381`）；MTU 探测包丢失直接丢弃不重传（`handleLostMtuProbe` `:1405`）。
- **MUST** piggyback ACK / transient 帧在重传时剔除：`kFMTransientOnRetrans` 帧及 `transient_ack_size>0` 的 ACK 载荷重传前 `StripTransientAckPayload`（`:779`、`:1502`、`:1622`）。
- **MUST NOT** 同一包重复计丢：`kPoLossRecorded` 已置的包在 `detectLosses`/`expireUnacked` 中跳过（`:1214`、`:1327`）。
- **MUST** `m_reorderThresh >= 1`（`setReorderThreshold` 归一化 `max(1,threshold)`，`:881`）。
- **MUST** 飞行统计守恒：`unackedRemove` `assert(m_bytesUnackedAll >= packetSize)`（`:1726`）。
- **MUST** `notePacketSent` 忽略 `packno==0`（`:319`）。
- **MUST** MTU 探测只在 `kStateConnected` 后启动（`:602`）。
- 重传模式优先级 **MUST** 为 Handshake > Loss > TLP > RTO（`:1074`）。
- RTO delay **MUST** 封顶 `MAX_RTO_DELAY`（`:1058`）；转 ms 后 **MUST** `>=1`（`:1063`）。

---

## 5. 参数与默认值（确切值 + 变量名）

### 5.1 send_ctl.cpp 编译期常量（`:34`~`:44`）
| 宏/常量 | 值 | 说明 |
|---|---|---|
| `N_NACKS_BEFORE_RETX` | `3` | 默认重排序阈值（丢包检测），初始化 `m_reorderThresh` |
| `MAX_RTO_DELAY` | `60000000` us (60s) | RTO 延迟上限 |
| `DEFAULT_RETX_DELAY` | `500000` us (500ms) | srtt=0 时 RTO 基值 |
| `MIN_RTO_DELAY` | `200000` us (200ms) | RTO 下限 |
| `INITIAL_RTT` | `333333` us (~333ms) | TLP 计算中 srtt=0 的回退值 |
| `MAX_RTO_BACKOFFS` | `10` | RTO 指数退避封顶 |
| `MAX_RESUBMITTED_ON_RTO` | `2` | RTO 触发时 `m_nextLimit` |
| `kSendBatchCap` | `32` | 批量发送数组容量 |

### 5.2 丢包检测/自适应阈值（`:890`~`:972`、`:995`~`:1005`）
- `dynamicReorderThresholdCap`：`srtt==0 → base+4`；否则 `base + min(12, 2 + (min(400, rttvar*100/srtt)/50))`。
- `updateReorderThresholdOnLoss`：连续丢包事件 `>=2` 且距上次扩张 `>= max(1000us, srtt/4)` 才 `currentThresh++`（不超过 cap）。
- `updateReorderThresholdOnAck`：收缩阈值 `shrinkAckThreshold = max(8, currentThresh*2)`；累计 acked 达到后 `currentThresh--`（不低于 base）。
- `AdaptiveReorderState.baseThresh/currentThresh` 初值 `3`（`send_ctl.h:234`）。
- 丢包信号保留窗 `kKeepWindowUs = 10000000` us (10s)（`:999`）。

### 5.3 RTT EWMA（`onAckReceived`，用于统计导出）
- 首样本：`obsRttVar = sample/2`。
- 后续：`obsRttVar = (obsRttVar*3 + delta)/4`；`obsRtt = (obsRtt*7 + sample)/8`。

### 5.4 ACK 触发相关（协商参数，**定义在其它模块**，此处为交叉参考）
| 宏 | 值 | 定义处 |
|---|---|---|
| `UTP_DEFAULT_ACK_THRESHOLD` | `5`（每 5 个 ack-eliciting 包触发一次 ACK） | `proto/proto.h:27` |
| `UTP_DEFAULT_MAX_ACK_DELAY_MS` | `25`（最大 ACK 延迟 ms） | `proto/proto.h:28`；`connection_impl.h:373 m_peerAckMaxDelayMs` 默认此值 |
| `UTP_DEFAULT_REORDER_THRESHOLD` | `3`（乱序超过 3 包立即 ACK） | `proto/proto.h:29` |
| `FrameAckFrequency::kDefault*` | 分别镜像上述三值 | `proto/frame/ack_frequency.h:21-23` |

> 注意区分两个“reorder 阈值”：接收侧 ACK-eliciting 的 `reordering_threshold`（默认 3，用于决定是否立即回 ACK，`connection_impl.cpp:659`）与本模块**丢包检测**的 `m_reorderThresh`（默认 `N_NACKS_BEFORE_RETX=3`）。二者数值相同但语义不同；`connection_impl.cpp:379/1308` 用协商到的 `m_ackReorderingThreshold` 调用 `setReorderThreshold` 把接收侧值**复用**为发送侧丢包检测阈值（见 §7 边界与风险）。

---

## 6. 接口

### 6.1 SendControl 公有（`send_ctl.h:99`~`:126`）
| 方法 | 语义 |
|---|---|
| `SendControl(conn, ctx)` / `init()` | 构造 + 初始化拥塞算法（`cfg->cc_algorithm==2 → Cubic`，否则 `BbrV1`，`:235`）、开启 `Pace` |
| `Status packetSent(PacketOut*)` | 发送后记账（§3.1） |
| `void notePacketSent(packno)` | 更新发送历史 |
| `bool canSend()` | `= canTransmitPacket(1)` |
| `bool canSchedulePacket(size) const` | 基于 `m_bytesUnackedAll + m_bytesScheduled` 与 cwnd（`:329`） |
| `bool canTransmitPacket(size)` | 基于 `m_bytesUnackedAll` 与 cwnd + Pacer（`:343`）；受阻时排下次调度 |
| `uint64_t bytesOutTotal()/bandwidthEstimate()/retransmittedBytes()` | 统计导出 |
| `utp_packno_t largestSentPacketNo()` | `m_sendHistory.largestPackNo()` |
| `Status onAckReceived(const AckInfo&, nowUs)` | ACK 处理（§3.3） |
| `void onCanWrite(nowUs)` | 可写窗口驱动发送/重传/MTU 探测（§3.2） |
| `Status schedulePacket(PacketOut*, trackOnSend)` | 入调度 FIFO |
| `uint32_t scheduledCount()` | `m_nScheduled` |
| `void setReorderThreshold(threshold)` | 设置/重置自适应重排序阈值 |
| `bool isLossFrequent(now, windowUs, threshold) const` | 滑窗内丢包信号计数是否达阈（`:974`） |

### 6.2 关键私有
`expireUnacked`、`detectLosses`、`onLossEvent`、`recordLossSignal`、`retransAlarm`、`getRetransmissionMode`、`onRetransTimer`、`calculate{Handshake,Tlp,PacketRto}Delay`、`retransmitLost{Batch,Packet}`、`retransmitSplitStreamPacket`、`handle{RegularLostPacket,LostMtuProbe}`、`appendUnacked`/`unackedRemove`/`destroyPacket`。

### 6.3 AckInfo / SendHistory
`AckInfo::reset()`；`SendHistory::update(packno)` / `largestPackNo()`。

---

## 7. 当前实现边界

- **`flushScheduledPackets` 实际批量为 1**：`batchCap = min(maxPackets, 1u)`（`:672`），尽管使用了 `kSendBatchCap=32` 的数组与 `sendmmsg`。调度包每次只发 1 个（重传批量 `retransmitLostBatch` 才用满 32，`:764`）。待确认这是否为有意限制。
- **`OneRttAcked` 标志从未被置位**：仅在 `onRetransTimer` 的 RTO 分支被读取（`:1111`），全代码库未发现设置点 → 该条件恒为 false，RTO 实际只依赖 `now - m_lastRtoTime >= rto`。**待确认（疑似未接线）**。
- **`LostAckInit` / `LostAckHsk`**：定义但 `send_ctl.cpp` 中从不置位；仅 `LostAckApp` 在 `handleRegularLostPacket` 置位（`:1382`），且未见本模块消费者（应由连接层读取以触发新 ACK，未在本模块闭环）。**待确认消费方**。
- **`m_largestAck2ed`**：成员声明+init 清零，`send_ctl.cpp` 内无写入/读取；注释称用于裁剪接收历史，但本模块未实现该逻辑。**待确认（疑似预留/死字段）**。
- **`m_lastSentTime`**：init 清零，未见更新点（空闲检测未落地）。**待确认**。
- **`retransmitLostPacket`（单包版本，`:1612`）**：功能完整但 `onCanWrite` 走的是 `retransmitLostBatch`；单包版是否仍有调用方需在连接层确认。
- **`AckInfo::reset()` 不清 `ack_ranges`**：复用同一 `AckInfo` 时旧 range 残留，依赖 `range_size` 严格界定；若上层误用可能读到脏 range。
- **`setReorderThreshold` 语义耦合**：接收侧协商的 ACK reorder 阈值被直接用作发送侧丢包检测的 base 阈值（`connection_impl.cpp:379/1308`）。两者语义不同，耦合可能不符合预期。**风险**。
- **`kSendBatchCap` 数组按 `UdpSocket::kMaxMsgSlices` 逐包填充**，`slice_count` 超过上限时静默截断（`:177`、`:819`）。

---

## 8. 与 doc/ 差异

| 项 | doc/ 描述 | 代码实现 | 结论 |
|---|---|---|---|
| 不重传帧类型 | `doc/frame/帧重传.md` 列 PADDING/ACK/PING 不重传，示例代码引用 lsquic 的 `SC_LOST_ACK_*` | `UTP_FRAME_RETX_MASK`(`packet_common.h:43`) 确实排除 ACK/PADDING/PING；本实现有 `LostAckApp` 但 `LostAckInit/Hsk` 未使用，且无“ACK-only 不计拥塞”的显式分支（拥塞记账对所有 unacked 一视同仁，`appendUnacked`） | 大方向一致；doc 的 PN-space 分离（Init/Hsk/App）在本实现中**未落地**，只有单一 App 语义 |
| ACK 触发策略 | `doc/协议设计文档.md §9.2` 计数阈值 + max_ack_delay + 乱序快速 ACK + piggyback | 本模块**不生成 ACK**，触发逻辑在接收侧/连接层；本模块只在重传时剔除 transient piggyback ACK | 职责在别处，doc §9.2 不属本模块 |
| 重传原则 | §9.7 重传必须用新 PN、不改流内偏移 | 一致：`kPoResetPackNo` + `RewritePacketNumber` + 重新加密（`:794/:1640`） | 一致 |
| 丢包检测 | §9.6 “ACK 前移 + 重排序阈值” | 实现了三重信号（FACK/early-retx/sent-time），比 doc 更细 | 代码更细，doc 为概括 |
| HandshakeDone pending | §9.7/§3 要求 HandshakeDone 在 pending 期可重复携带直到确认 | 代码有 `onHandshakeDoneFrameAcked` 回调（`:466`）与握手重传模式；pending 重复携带逻辑在连接层，本模块只提供 ACK 确认信号 | 需连接层交叉确认 |
| max_ack_delay 归属 | §9.3 只在 AckFrequency 定义 | `m_peerAckMaxDelayMs` 默认 `UTP_DEFAULT_MAX_ACK_DELAY_MS=25`，用于 RTT 扣减与 TLP 计算 | 一致 |

---

## 9. 依赖

- **`ConnectionImpl`**（`context/connection_impl.h`）：`m_rttStats`（`srtt()/rttVar()/update()`）、`m_obsRttUs/m_obsRttVarUs`、`m_peerAckMaxDelayMs`、`m_udpSocket`（`send`/`sendmmsg`）、`m_mtuDiscovery`、`m_txAesCtx`、`m_mm`（`putPacketOut`）、`state()`、`sendPacket/sendStreamFrame`、`canSendStreamUnackedBytes`、`canSendOnCurrentPath`、`streamPayloadBudgetHint`、`scheduleWrite`/`nextScheduleTime`、`onStreamPacketAcked/Unacked*`、`onHandshakeDoneFrameAcked`、`m_bytesOut/m_bytesRetrans`、`m_ackReorderingThreshold`。
- **`ContextImpl`**：`loop()`（定时器）、`config()`（`cc_algorithm`、`clock_granularity_us`）。
- **`Congestion`**（`congestion/congestion.h`，实现 `Cubic`/`BbrV1`）：`onInit/onPacketSent/onBeginAck/onAck/onEndAck/onLost/onLoss/onTimeout/wasQuiet/getCwnd/getPacingRate`。
- **`Pacer`**（`congestion/pacer.h`）：`init/canSchedule/packetScheduled/nextSched/tickIn/tickOut/lossEvent`。
- **`PacketOut`**（`proto/packet_out.h`）：`po_flags`（`kPoUnAcked/kPoSched/kPoLost/kPoLossRecorded/kPoResetPackNo/kPoMtuProbe/kPoEncrypted/kPoHello/kPoKeepPlaintext` 等）、`local_flags`（`kPOLTrackOnSend/kPOLFacked`）、`frame_types`、`frame_meta[]`（`kFMTransientOnRetrans/kFMDroppableOnMtu`）、`addSendAttempt`、`bw_state`。
- **`detail::PacketEditor`**：`RewritePacketNumber`、`StripTransientAckPayload`。
- **`AesGcmContext`**：`encrypt`、`ReleaseEncryptBuffer`。
- **`proto/*`**：`UTP_FRAME_RETX_MASK`、`FrameType`/`kFrameMax`、`UTP_HEADER_SIZE`、`FramePadding`/`FrameStream`、ACK 阈值宏。
- **`event/timer.h`**（`ev::EventTimer`）、**`util/time.h`**（`MonotonicUs/Ms`）、**`logger`**。
