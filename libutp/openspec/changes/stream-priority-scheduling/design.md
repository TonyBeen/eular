## Context

本设计用于补齐 libutp 在多流并发发送场景下缺失的 stream 级优先级调度能力。目标是在保持现有 ACK/重传/拥塞控制闭环稳定性的前提下，优先保障高优先级流的排队时延，并避免低优先级流长期饥饿。

本功能尚未发布，因此本设计遵循项目级 OpenSpec 规则：直接冻结首版可交付方案，不为未来演进预留未批准的保留策略或占位接口。

## Goals / Non-Goals

**Goals:**

- 提供 stream 优先级语义与默认值。
- 定义调度策略并保证与现有发送主循环兼容。
- 给出饥饿保护与公平性底线。
- 增加可观测指标与测试约束。

**Non-Goals:**

- 不在本次引入应用层复杂 QoS 策略。
- 不改变 stream ID、ACK 帧或路径验证协议格式。
- 不与多路径调度同时耦合推进。
- 不支持 Incremental 属性或相关 API；应用若需要该语义，应在应用层通过分片和发送节奏控制实现。

## Decisions

### 1. 优先级模型

每个 stream 维护 `priority`，优先级范围为 `0~7`，默认值为 `4`。

- 0: critical
- 1: realtime
- 2: high
- 3: normal_high
- 4: normal
- 5: normal_low
- 6: low
- 7: background

### 2. 首版调度策略直接定型

首版固定采用两种正式策略并存：
- `Strict Priority + Aging`（默认）
- `DRR`

说明：
- DRR 是首版正式能力，不是未来占位。
- 不为首版范围之外策略预埋占位接口。

### 3. 包内帧填充顺序

发送侧按以下顺序在单个包内填充帧：
1. 必要控制帧（ACK/Path/Close）
2. 重传帧
3. Stream 新数据（按 `priority` 与当前调度策略选流）
4. Padding

### 4. 流控与发送约束优先于优先级

选流必须同时满足：
1. stream 有待发送新数据
2. stream 级流控允许发送
3. connection 级流控允许发送
4. cwnd/pacer 允许发送
5. 在上述约束下优先级最高

### 5. 优先级继承
本版不引入优先级继承接口与语义。

说明：
- 首版先聚焦可验证的两种调度策略（Strict+Aging 与 DRR）。
- 优先级继承如需引入，应单独立项并补齐依赖语义证明和测试闭环。

### 6. 热切换范围

首版支持 `POLICY_DISABLED / POLICY_STRICT_PRIORITY / POLICY_DRR` 三态热切换。

### 7. 可观测性

至少新增以下指标：
- 每优先级排队时延分位数（P50/P95/P99）
- 每优先级发送字节与包数
- 每优先级重传占比
- 饥饿保护触发计数与有效性
- 最大饥饿时间
- 低优先级吞吐占比
- 策略切换计数
- DRR deficit 消耗计数

## Risks / Trade-offs

- 优先级策略可能放大应用配置错误带来的不公平。
- Strict Priority 在高负载下对低优先级流不友好，需要明确保底策略。
- 指标埋点若过细可能增加 CPU 开销。
- 双策略并存会增加参数理解和配置复杂度。

## Open Questions (For Review)

- 默认是否启用优先级调度，还是通过配置开关逐步放量？
- aging 阈值和 burst 约束是否接受 RTT/CWND 动态计算？
- 是否接受 DRR 作为首版正式策略，默认仍为 Strict+Aging？
- 是否接受三态热切换 `DISABLED / STRICT_PRIORITY / DRR`？

## Rollout Plan

1. Review Gate: 仅评审文档与设计，不提交运行时代码变更。
2. Implementation Gate: 评审通过后再进入编码。
3. Verification Gate: 单测 + 集成压测 + 回归对比通过后启用。
