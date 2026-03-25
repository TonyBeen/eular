# Stream 优先级调度设计草案（评审修订版）

> 文档状态：Draft / 待复审  
> 版本：v0.3  
> 日期：2026-03-25  
> 目标：按评审意见完成修订，复审通过后再编码

---

## 1. 背景与问题

当前 libutp 已支持多 stream 并发收发，但发送侧缺少显式的 stream 级优先级调度。
在高负载场景下，大吞吐流可能长期占用发送机会，导致交互型小流尾延迟抬升。

当前已知症状：
- 多流并发时，发送机会主要由可发送性与队列顺序决定。
- 缺乏面向业务语义的优先级表达能力。
- 缺乏按优先级分桶的可观测指标与公平性指标。

---

## 2. 设计目标

1. 为 stream 增加可配置优先级（0~7，数值越小优先级越高）。
2. 在不破坏 ACK、重传、拥塞控制闭环的前提下，降低高优先级流时延。
3. 提供饥饿保护，避免低优先级流持续饿死。
4. 提供性能与公平性可观测指标，支持压测和回归验收。

非目标：
1. 本次不引入复杂多路径调度。
2. 本次不改协议包头或帧格式。
3. 本次不做应用层 QoS 自动学习。
4. 本次不支持 Incremental 属性与相关 API；同优先级流是否交织由调度策略实现，应用如需更细粒度控制应在应用层分片和发送节奏中处理。

---

## 3. 方案候选

### 3.1 方案 A：严格优先级 + Aging（首版推荐）

规则：
- 发送新数据时，优先选择更高优先级 stream。
- 若低优先级 stream 等待时间超过 aging 阈值，则触发保底服务机会。

优点：
- 实现简单，落地快，收益直接。

风险：
- 参数不当会导致低优先级流体验不稳定。

### 3.2 方案 B：DRR

规则：
- 按 deficit/quantum 循环服务各优先级队列。

优点：
- 公平性更强，可预测性更好。

风险：
- 相比严格优先级，运行时状态更多，调参与验证成本上升。

### 3.3 结论

首版直接采用“严格优先级 + Aging + DRR + 运行时策略热切换”的完整方案，默认策略为严格优先级 + Aging。

说明：
- 当前 uTP 仍处于首版设计阶段，尚未发布，不以“为未来演进预留”为目标。
- 本版直接冻结首发可交付方案，避免引入过多保留态和兼容负担。

---

## 4. 包内帧填充顺序（替代原“仲裁顺序”）

本设计采用“单包内帧聚合填充顺序”，而不是“分包仲裁顺序”。

```cpp
void buildPacket(Packet* pkt, size_t space) {
	// 1) 必要控制帧优先
	if (needSendAck()) space -= addAckFrame(pkt, space);
	if (needSendPathChallenge()) space -= addPathChallengeFrame(pkt, space);
	if (needSendPathResponse()) space -= addPathResponseFrame(pkt, space);

	// 2) 重传帧优先于新数据
	if (hasLossRecovery() && space > 0) {
		space -= addRetransmitFrames(pkt, space);
	}

	// 3) 新数据按 stream 优先级填充
	while (space > kMinFrameSize) {
		Stream* s = selectStreamByPriority();
		if (!s) break;
		space -= addStreamFrame(pkt, s, space);
	}

	// 4) 需要时补齐 padding
	if (needPadding(pkt) && space > 0) {
		addPaddingFrame(pkt, space);
	}
}
```

约束：
- 控制帧和 STREAM 帧可共存于同一包。
- 优先级只影响“新数据选流”，不影响控制帧和重传帧优先级。

---

## 5. 选流条件与流控交互

`selectStreamByPriority()` 的候选条件必须同时满足：

1. stream 有待发送数据。
2. stream 级流控允许发送（未触达 stream limit）。
3. connection 级流控允许发送（未触达 connection limit）。
4. 拥塞控制与 pacer 允许发送。
5. 在满足 1~4 的候选中，选择优先级最高者。

示例伪代码：

```cpp
Stream* selectStreamByPriority() {
	for (uint8_t prio = 0; prio <= 7; ++prio) {
		for (Stream* s : buckets[prio]) {
			if (!s->hasDataToSend()) continue;
			if (!s->canSendUnderStreamFlowControl()) continue;
			if (!connCanSendUnderConnectionFlowControl()) continue;
			if (!sendAllowedByCwndAndPacer()) continue;
			return s;
		}
	}
	return nullptr;
}
```

---

## 6. 优先级模型与默认值

### 6.1 取值范围与语义

- priority: 0~7
- 默认值：4（中位数向上取整，代表普通流）

建议语义：
- 0：CRITICAL（极少使用，关键实时）
- 1：REALTIME（实时交互）
- 2：HIGH（高优先）
- 3：NORMAL_HIGH（普通偏高）
- 4：NORMAL（默认）
- 5：NORMAL_LOW（普通偏低）
- 6：LOW（低优先）
- 7：BACKGROUND（后台批量）

### 6.2 Aging（基于 RTT/CWND 动态）

首版采用“动态计算 + 边界钳制”：

```cpp
aging_threshold_ms = clamp(srtt_ms * 0.5, 5, 100);
max_high_prio_burst_packets = clamp((cwnd / mss) * 0.2, 4, 64);
```

保留固定参数回退开关（用于应急与诊断）：
- fixed_aging_threshold_ms（默认关闭）
- fixed_max_high_prio_burst_packets（默认关闭）

### 6.3 DRR 调度参数（首版）

首版引入 DRR（Deficit Round Robin）作为第二种正式可用调度策略。

建议默认参数：
- drr_quantum_bytes: 1200
- drr_deficit_cap_bytes: 64 * 1200

DRR 规则：
1. 每个优先级桶内按 round-robin 轮转。
2. 每轮为候选 stream 累加 `quantum` 到 `deficit`。
3. 当 `deficit` 足够发送当前帧时出队发送，并扣减对应字节。
4. `deficit` 不得无限增长，受 `drr_deficit_cap_bytes` 上限约束。

说明：
- DRR 与严格优先级策略共存于首版，由运行时策略选择。
- DRR 主要用于改善同优先级和混合流量下的公平性。

---

## 7. API 与配置草案

### 7.1 策略与配置

```cpp
enum SchedulerPolicy {
	POLICY_DISABLED = 0,
	POLICY_STRICT_PRIORITY = 1,
	POLICY_DRR = 2
};
```

配置项：
- enable_stream_priority_scheduler（默认 false）
- stream_scheduler_policy（默认 POLICY_STRICT_PRIORITY）
- stream_priority_default（默认 4）
- stream_priority_aging_rtt_multiplier（默认 0.5）
- stream_priority_burst_cwnd_ratio（默认 0.2）
- stream_drr_quantum_bytes（默认 1200）
- stream_drr_deficit_cap_bytes（默认 76800）

### 7.2 Stream 接口草案

- setPriority(uint8_t priority)
- priority() -> uint8_t

约束：
- 仅支持在连接所属线程内调用，不支持跨线程调用。
- 运行时改优先级仅影响后续待发送新数据。
- 已进入重传队列的数据不做重排。

### 7.3 调度策略热切换设计

本版热切换支持以下三种策略状态：

1. `POLICY_DISABLED`
   - 关闭 stream 优先级调度，退回当前普通发送路径
2. `POLICY_STRICT_PRIORITY`
	- 开启严格优先级 + Aging
3. `POLICY_DRR`
	- 开启 DRR 调度

切换接口：
- `setSchedulerPolicy(SchedulerPolicy policy)`
- `schedulerPolicy() -> SchedulerPolicy`

切换规则：
1. 热切换仅影响后续待发送新数据。
2. 已组包但未发出的数据包不做拆包重排。
3. 已进入重传队列的数据继续按原重传路径处理。
4. 从 `DISABLED -> STRICT_PRIORITY/DRR`：
   - 立即重建优先级桶
	- 按各 stream 当前 priority 重新入桶
   - 下一次新数据选流即按优先级执行
5. 从 `STRICT_PRIORITY/DRR -> DISABLED`：
   - 清空调度桶状态
	- 停止策略特定的运行时状态（aging 或 deficit）
   - 后续退回当前普通选流路径
6. 从 `STRICT_PRIORITY <-> DRR`：
	- 保留优先级桶成员关系
	- 清空旧策略运行时状态并初始化新策略状态
	- 不重排已组包未发数据，不影响重传队列

切换伪代码：

```cpp
void setSchedulerPolicy(SchedulerPolicy policy) {
	if (policy == current_policy) return;

	if (policy == POLICY_STRICT_PRIORITY || policy == POLICY_DRR) {
		rebuildPriorityBucketsFromStreams();
		if (policy == POLICY_STRICT_PRIORITY) resetAgingWindow();
		if (policy == POLICY_DRR) resetDrrDeficitState();
		current_policy = policy;
		return;
	}

	clearPriorityBuckets();
		clearAgingRuntimeState();
	current_policy = POLICY_DISABLED;
}
```

可观测性要求：
1. 记录策略切换计数：`stream_scheduler_policy_switch_total`
2. 记录最后一次切换时间：`stream_scheduler_last_switch_ts`
3. 切换前后保留基础统计，但不强制保留 aging 窗口内瞬时状态

边界说明：
- 本版策略集合固定为 `DISABLED / STRICT_PRIORITY / DRR`。

---

## 8. 线程模型约束

首版明确采用单线程模型：

1. 连接发送路径、优先级调度器和 stream 优先级修改均发生在同一线程。
2. 不支持跨线程直接调用 `setPriority()` 或其它调度相关接口。
3. 本设计不引入锁、原子变量、无锁队列或任务投递机制。

说明：
- 当前 priority scheduler 仅服务于单线程连接上下文。
- 若未来需要跨线程使用，应作为单独设计议题重新评审，而不是在本版中预埋并发语义。

---

## 9. 性能影响分析（预估）

### 9.1 CPU 开销

调度复杂度：
- 按 8 个优先级桶扫描，桶内 round-robin。
- 典型复杂度约 O(8M)，M 为桶内活跃 stream 数。

优化目标：
- 相比现有发送热路径，CPU 增幅 < 5%。

### 9.2 内存开销

新增字段（预估）：
- 每 stream: `priority`（1B）+ `last_service_ts`（8B）
- 调度器桶结构：常量级元数据

对 1000 stream 的量级，增量约 10KB 级别，可接受。

### 9.3 基准测试目标

1. 吞吐回退不超过 5%。
2. 高优先级流 p95/p99 排队时延显著下降。
3. 低优先级流吞吐维持非 0 且有保底。

---

## 10. 可观测性与验收指标

核心指标：
1. stream_priority_queue_delay_ms_p50/p95/p99
2. stream_priority_tx_bytes_total
3. stream_priority_tx_packets_total
4. stream_priority_retransmit_ratio
5. stream_priority_aging_trigger_total

公平性补充指标：
1. stream_priority_max_starvation_ms
2. stream_priority_aging_effectiveness_ratio
3. stream_priority_low_prio_throughput_ratio
4. stream_drr_deficit_exhaust_total
5. stream_scheduler_policy_switch_total

---

## 11. 与现有机制交互说明

### 11.1 与流控交互

- 优先级调度受 stream/connection 双层流控约束。
- 高优先级流流控耗尽时，会自动选择可发送的低优先级流。

### 11.2 与拥塞控制交互

- 优先级不改变 cwnd 计算。
- 优先级只影响可发送预算内的新数据分配。

### 11.3 与 ACK 交互

- ACK 生成与处理不受 stream 优先级影响。

### 11.4 与重传交互

- 重传帧优先于新数据帧。
- 低优先级流重传不应被高优先级新数据阻塞。

### 11.5 与路径迁移交互

- 路径验证控制帧优先于业务新数据。
- 迁移期间优先级规则保持不变。

---

## 12. 测试计划

### 12.1 单元测试

1. 优先级比较与同优先级 round-robin 公平性。
2. 流控约束下的选流（高优先级流控满时应选低优先级可发送流）。
3. 拥塞窗口为 0 时返回不可发送。
4. aging 触发与触发后发送有效性。
5. 运行时动态改优先级生效。

### 12.2 集成测试

1. 高优先级小流 + 低优先级大流并发。
2. 多流混合（0/4/7）并发。
3. 丢包 + 重传场景下优先级正确性。
4. 路径迁移期间优先级保持。

### 12.3 回归测试

1. ACK 行为不回退。
2. 拥塞控制状态迁移不回退。
3. 连接关闭与路径验证行为不回退。

---

## 13. 风险与回滚

风险：
1. 参数不当导致低优先级体验下降。
2. 调度和指标引入热路径开销。
3. 若调用方违反单线程约束，可能导致调度状态与发送状态不一致。
4. 双策略并存增加参数理解与配置复杂度。

缓解：
1. 首版开关可控，默认关闭。
2. 先最小指标集，后扩展。
3. 在接口与文档中明确“仅支持单线程连接上下文”，实现中不承担跨线程兼容责任。
4. 默认策略固定为 `POLICY_STRICT_PRIORITY`，DRR 需显式启用并受同一测试门槛约束。

回滚：
- 设置 `enable_stream_priority_scheduler=false` 或 `POLICY_DISABLED`。

---

## 14. 参数调优指南（初版）

1. 低 RTT 场景：可降低 `aging_rtt_multiplier`。
2. 高 RTT 场景：可提高 `aging_rtt_multiplier` 或放宽上限。
3. 高带宽场景：可提高 `burst_cwnd_ratio`。
4. 交互优先场景：可降低 `burst_cwnd_ratio`。

---

## 15. 故障排查（初版）

1. 低优先级吞吐为 0：检查 aging 指标与流控阻塞指标。
2. 高优先级时延无改善：检查重传比、cwnd 与开关是否开启。
3. 策略未生效：检查 policy 与 stream 默认优先级配置。
4. DRR 公平性异常：检查 `stream_drr_deficit_exhaust_total` 与 quantum/deficit_cap 配置。

---

## 16. 复审清单（待你确认）

1. 是否同意“包内帧填充顺序”作为规范描述。
2. 是否同意“流控约束先于优先级选择”的强约束。
3. 是否同意默认优先级改为 4。
4. 是否同意 Aging 使用 RTT/CWND 动态计算。
5. 是否同意首版默认关闭调度器开关。
6. 是否同意首版正式支持 DRR，并默认仍为 `POLICY_STRICT_PRIORITY`。
7. 是否同意热切换支持 `DISABLED / STRICT_PRIORITY / DRR` 三态切换。
8. 是否同意复审通过后再进入代码实现。

---

## 17. 分阶段计划

1. 阶段 A：文档冻结（当前）
- 完成本版修订并复审签字。

2. 阶段 B：代码实现
- 实现调度器、配置项、指标与单测。

3. 阶段 C：验证与灰度
- 集成压测、回归验证、对比指标、逐步放量。
