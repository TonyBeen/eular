## Why

当前 libutp 在多流并发发送时尚未提供 stream 级优先级调度。大吞吐流持续发送时，交互型小流可能出现尾延迟抬升，影响时延敏感业务体验。该缺口已被确认为当前协议能力的最高优先级改进项。

## What Changes

- 为连接内 stream 引入可配置优先级语义（0~7，数值越小优先级越高，默认 4）。
- 直接冻结首版发送调度方案：Strict Priority + Aging 与 DRR 双策略并存，默认 Strict Priority + Aging。
- 定义运行时三态策略热切换：`DISABLED / STRICT_PRIORITY / DRR`。
- 定义与现有 `cwnd`、pacer、ACK/重传队列的包内帧填充顺序和选流边界。
- 明确不支持 Incremental 属性与相关 API；该类发送形态由应用层控制数据切片与发送节奏实现。
- 增加优先级维度的公平性指标、策略切换指标、DRR 运行时指标与回归测试要求。

## First-Release Rule

该功能尚未发布，因此本提案明确按“首版直接定型”原则编写：

- 不为未来策略演进预留额外保留态。
- 不引入未纳入首版交付范围的占位策略。
- 以一个可交付、可测试、可回滚的 v1 方案作为唯一设计目标。

## Capabilities

### New Capabilities
- `stream-priority-scheduling`: Stream 级优先级定义、发送调度与可观测性约束。

### Modified Capabilities
- `stream-discovery-and-lookup`: 增加 stream 默认优先级与查询语义（如适用）。
- `stream-read-fin-reset`: 不改变读侧语义，但补充优先级不影响 FIN/RESET 正确性的约束。

## Impact

影响范围包括 `ConnectionImpl` 发送侧调度、`SendControl` 出队策略、配置接口（默认优先级/开关）、统计与测试基线。该提案阶段不直接改运行时代码。
