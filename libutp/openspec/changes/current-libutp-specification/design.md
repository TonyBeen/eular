## Context

libutp 已有一版 OpenSpec 基线，但目前 capability 粒度主要停留在五个大块。随着协议细节持续演进，单个 capability 内聚合了多类行为，导致后续变更在评审时难以精准回答“究竟改了哪一条契约”。本轮设计目标是在不改变任何运行时语义的前提下，对 capability 进行二级拆分，让规范和代码责任面更一一对应。

该变更仍是文档层变更，但会横跨 API、状态机、帧处理、回调可观测性与安全门禁。核心设计问题是：如何把既有 requirement 重新分桶为更小 capability，同时避免把内部实现结构当作规范边界。

## Goals / Non-Goals

**Goals:**

- 将 5 个大块 capability 拆为细粒度 capability，且不损失现有 requirement 覆盖。
- 让每个 capability 对应单一职责域，便于后续按最小影响面提案。
- 保持 requirement 面向外部可观测行为，维持与现有测试证据链一致。
- 继续为内部重构保留空间，避免绑定私有数据结构与微观调度细节。

**Non-Goals:**

- 修改 libutp 运行时行为、报文格式或公共 API。
- 在本轮拆分中补充尚未被实现与测试稳定支撑的“未来行为”。
- 将 capability 拆分演化为实现目录镜像，造成规范与代码耦合。

## Decisions

### 1. 采用“二级 capability”而非维持五大聚合块

规范从五大块拆分为 15 个小 capability：
- 连接域：`handshake-establishment`、`passive-accept-control`、`connection-outcome-events`、`connection-close-convergence`
- 流域：`stream-discovery-and-lookup`、`stream-id-admission`、`stream-read-fin-reset`
- ACK 与恢复域：`ack-trigger-policy`、`ack-frequency-synchronization`、`loss-retransmission-cycle`
- 路径域：`path-challenge-lifecycle`、`path-amplification-limits`
- 0-RTT 域：`zero-rtt-token-gating`、`zero-rtt-replay-defense`、`zero-rtt-decision-signaling`

Rationale: 变更将更接近“单一能力单文件”，后续 proposal 可最小化影响面并提升审阅定位速度。

Alternative considered: 保留五大 capability 仅在文件内部加小节。Rejected，因为 capability 仍然过粗，跨主题改动仍会混在同一文件中。

### 2. 规范边界仍以外部可观测结果为准

拆分后每个 capability 依然只规定 SHALL/MUST 可观测结果，例如回调触发、stream_id 准入、ACK 时机、路径校验结果与 0-RTT 拒绝语义。

Rationale: 内部类和调参策略仍可能调整，规范应稳定描述结果，不锁死实现路径。

Alternative considered: 在二级 capability 中写入更细内部时序与容器行为。Rejected，因为会把实现细节误当协议契约。

### 3. requirement 迁移遵循“原 requirement 无语义漂移”原则

五大 capability 的既有 requirement 被按语义切分迁移到 15 个 capability，不引入超出现有实现证据的新契约。

Rationale: 本轮目标是结构重构，不是行为升级，避免将结构调整与协议行为变化耦合在同一次变更中。

Alternative considered: 拆分时顺带增强 requirement 强度。Rejected，因为会让评审无法区分“结构变化”与“行为变化”。

### 4. 未稳定区域继续保留为开放问题

加密握手细节、拥塞策略细化等仍按开放问题处理，不因 capability 变细而提前固化。

Rationale: capability 颗粒度提升不等于冻结更多未来设计。

## Risks / Trade-offs

- [Risk] capability 数量增加后，维护者可能面临“文件变多”的导航成本。→ Mitigation: 通过一致命名与 traceability 建立稳定映射。
- [Risk] 拆分不当会产生 requirement 重复。→ Mitigation: 删除原五大 capability 的重复 spec 内容，保留单一规范来源。
- [Risk] 后续提案跨多个小 capability 时，审阅者需处理多文件变更。→ Mitigation: 在 proposal 中明确列出受影响 capability 列表。

## Migration Plan

无需运行时迁移。合并后，后续 OpenSpec 变更需直接定位到对应二级 capability，避免回到“大块 capability 内追加小节”的方式。

## Open Questions

- 是否在后续将握手安全属性进一步拆成独立 security capability（例如 key update、anti-amplification 细化）？
- ACK 自适应策略是否需要从“结果导向”进一步提升到“状态机级别”规范，还是保持调参自由度？
