## 1. Documentation and Review (Current Stage)

- [x] 1.1 在用户文档记录“stream 级优先级缺失”为最高优先能力缺口。
- [x] 1.2 输出设计草案并列出待评审问题。
- [x] 1.3 按“首版直接定型”原则冻结第一版方案：Strict Priority + Aging 与 DRR（默认 Strict）+ 三态热切换。
- [ ] 1.4 完成架构评审与参数评审，确认冻结方案通过复审。

## 2. Implementation (After Review Approval)

- [ ] 2.1 在连接发送路径引入 stream 优先级调度器。
- [ ] 2.2 实现 Strict Priority + Aging 与 DRR 两种调度策略（默认 Strict）。
- [ ] 2.3 增加配置项（默认优先级、开关、Aging 参数、DRR 参数、三态热切换）。
- [ ] 2.4 打通指标采集（排队时延、公平性、DRR deficit、策略切换）。

## 3. Validation

- [ ] 3.1 新增单测：不同优先级流的调度顺序与饥饿保护。
- [ ] 3.2 新增单测：流控约束选流、DRR 调度公平性、策略热切换。
- [ ] 3.3 新增集成测试：高负载下交互流尾延迟收益验证。
- [ ] 3.4 回归现有 ACK/重传/拥塞控制行为，确认无功能回退。
