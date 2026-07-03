## Why

当前 OpenSpec 基线已经覆盖了连接、流、ACK、路径校验、0-RTT 五个大块，但 capability 粒度仍偏粗，后续任何改动都容易变成“大规格文件里的局部增删”，审阅和追踪成本较高。需要再拆一轮，把每个大块拆成职责单一、可单独演进的小 capability。

## What Changes

- 在现有基线上做二级 capability 拆分，不改变运行时行为。
- 将 5 个大块拆为 15 个细粒度 capability，分别聚焦握手、回调、流 ID 门禁、ACK 触发、重传、路径校验、0-RTT 安全门禁与可观测性。
- 保持 requirement 与现有代码/测试可观察行为对齐，避免将实现细节误固化为规范。

## Capabilities

### New Capabilities
- `handshake-establishment`: 主动侧握手成功后的 connected 晋升条件。
- `passive-accept-control`: 被动 pending、回调决策与 accept 门控。
- `connection-outcome-events`: connected / connect-error 结果事件语义。
- `connection-close-convergence`: close 路径向终止态收敛语义。
- `stream-discovery-and-lookup`: stream 创建后可检索与未知 ID 语义。
- `stream-id-admission`: 角色位、方向位、配额与入站 stream_id 门禁。
- `stream-read-fin-reset`: 可读性、乱序门控、FIN/RESET 终态语义。
- `ack-trigger-policy`: delayed ACK 的包阈值/时间阈值触发语义。
- `ack-frequency-synchronization`: AckFrequency 对收端 ACK 策略与发端重排阈值的联动。
- `loss-retransmission-cycle`: 丢包判定后以新发送尝试执行重传。
- `path-challenge-lifecycle`: 地址迁移后的 validating、挑战响应与重试耗尽失败。
- `path-amplification-limits`: validating 阶段放大限制与业务流量门控。
- `zero-rtt-token-gating`: token 的 peer 绑定、生命周期与元数据恢复门禁。
- `zero-rtt-replay-defense`: replay window 的重复早数据拒绝语义。
- `zero-rtt-decision-signaling`: 会话材料导出与 0-RTT 决策事件可观测性。

### Modified Capabilities

None.

## Impact

影响范围仍是规范层：`include/utp` 的 API 语义、`src/context` / `src/socket` 的连接编排、`src/proto` 的包帧行为、`src/stream.cpp` 的流行为、`src/congestion` 的恢复逻辑，以及 `test/` 中的回归观测点。该变更仅重构 OpenSpec capability 组织，不引入运行时行为改动。