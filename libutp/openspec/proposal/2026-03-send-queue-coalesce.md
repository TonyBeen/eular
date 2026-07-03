# libutp 发送队列与小包聚合优化设计草案（v1）

## 一、设计目标
1. 消除每次写入一个队列节点的元数据开销。
2. 消除头删搬移造成的 O(n) 成本。
3. 防止 tiny write 导致 tiny packet，默认尽量靠近路径可用负载上限发送。
4. 不破坏现有流调度语义和回调语义。

## 二、v1 方案
### 1. 发送队列从 Chunk 队列改为 Cursor 模型
- 删除 PendingSendChunk 队列语义，保留环形发送缓冲。
- 增加发送游标字段：next_send_offset、queued_bytes、fin_queued、fin_sent。
- 发送时直接从 sendBuffer 取连续可读区，按预算生成 Stream 帧。
- 好处是没有“每写一次一个节点”，也没有头删搬移。

### 2. 发送循环从按 chunk 次数改为按字节预算
- flushPendingSends 不再固定 1 chunk，而是按 byte budget 发送。
- 单次可发上限 = min(路径可用载荷, UINT16_MAX, 当前可读字节)。
- 直到 WOULD_BLOCK 或预算耗尽。
- 默认预算建议绑定 pmtu，避免单包只有 1KB 的报文化行为。

### 3. tiny write 合并策略
- 默认启用短暂聚合窗口（例如 1ms 内可合并）+ 最大聚合上限。
- 遇到 fin 或应用显式 flush 信号时立即发。
- 这样 1 字节多次写不会直接变成 1 字节一包。

### 4. 调度兼容策略
- Connection 侧仍按当前选流策略选 stream。
- Stream 一次被选中后按预算尽量发，提高吞吐，但受公平预算限制。
- 可配置单次 stream 最大发送字节，避免饿死其他 stream。

## 三、配置项（提案）
- stream_send_budget_bytes_per_pick
- stream_coalesce_delay_us
- stream_min_payload_before_immediate_send
- stream_enable_coalescing

## 四、验收标准
1. 连续 1 字节 write 负载场景下，平均每包业务载荷显著大于 1 字节。
2. CPU 和内存分配次数下降（重点看发送热路径）。
3. 不影响现有 session_resume、stream 调度、0-RTT 测试通过。
4. 在同等丢包条件下，吞吐不下降，重传率不劣化。

## 五、OpenSpec 落地方式（先评审后编码）
1. 先新增一条变更提案，包含 proposal/design/tasks 三件套。
2. tasks 里明确“设计评审通过”是编码前置 gate。
3. 编码阶段按 gate 执行，不再边问边改。
4. 这一流程与现有规则一致，规则位置: config.yaml
