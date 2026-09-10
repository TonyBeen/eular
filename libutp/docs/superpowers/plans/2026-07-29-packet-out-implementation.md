# packet_out 实施记录

- 日期: 2026-09-10
- 状态: 已实施
- 对应设计: [2026-07-29-packet-out-design.md](../specs/2026-07-29-packet-out-design.md)

## 已实施架构

`packet_out` 分为两个具有不同生命周期的池:

- Connection 私有 `utp_packet_out_pool_t` 仅管理 `utp_packet_out_t` 描述符。空闲链耗尽时，以 32 个描述符为一块惰性扩容；Connection 销毁时释放所有描述符块。
- Context 共享 `utp_packet_out_buffer_pool_t` 管理数据缓冲。固定桶为 `1280`、`1500`、`4096`、`9000` 和 `65535`；桶耗尽时以 32 块缓冲为一批扩容。所有 Context 内的 Connection 复用这些缓冲。

`utp_packet_out_pool_acquire()` 选择刚好可容纳请求的最小固定桶；描述符或缓冲扩容失败时回滚已借资源并返回 `UTP_INTERNAL_ERROR_NOMEM`。不存在 PacketOut 固定数量上限，Context 的发送控制队列使用 `SIZE_MAX` 策略，发送尝试索引则独立地按 32 项固定块增长。

每个共享缓冲桶在每 1024 次借还时更新一次峰值采样。长期峰值低于分配量四分之一时，最多释放一半完全空闲的 32 块批次；正在使用的批次不移动、不释放。

## 验证

- `c/test/test_packet_out.cc`: 初始化零分配、固定桶选择、32 项增长、跨 Connection 复用、收缩与状态重置。
- `c/test/test_send_control.cc`: `SIZE_MAX` 队列策略和 32 项发送尝试块。
- Context、Connection、Stream 与传输集成测试验证共享池接入、连接清理和 UDP 发送路径。
