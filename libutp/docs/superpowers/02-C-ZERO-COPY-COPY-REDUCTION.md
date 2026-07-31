# libutp C 零拷贝与数据拷贝收敛记录

本文记录 C 实现中 `memcpy` / `memset` 的使用边界和后续实现顺序。`doc/` 中的零拷贝文档可作背景参考，但实现决策以 `docs/`、`cpp/` 代码和当前 C 结构为准。

## 原则

1. 热路径优先减少大块数据搬移，初始化类清零排在后面。
2. 不能为了删除 `memcpy` 破坏可靠传输的生命周期约束：应用 `write(data,len)` 返回后，协议栈必须持有可重传的数据。
3. 接收侧要实现真正零拷贝，必须先有 `PacketIn` 池和引用计数；否则 UDP read buffer 会被下一次收包覆盖，STREAM 乱序缓存不能直接指向它。
4. 发送侧 STREAM payload copy 已通过 `PacketOut` slice + socket scatter/gather 发送移除；后续发送侧重点转为应用写入 acquire/commit 和加密路径。
5. `memset` 只在需要系统 ABI 清零、敏感数据清理、或结构字段过多且安全性优先时保留。可明确初始化的内部结构优先逐字段初始化，避免清大对象里的 payload/storage 区。

## 当前保留的必要拷贝

| 位置 | 原因 | 后续替代 |
|---|---|---|
| `utp_stream_write()` 将应用数据写入 stream send buffer | 传统 write API 必须解耦应用缓冲生命周期，并保留明文用于 ACK 前重传 | 增加 acquire/commit write view，允许应用直接写入 ring buffer |
| `utp_stream_on_frame()` 将 STREAM 数据存入 recv fragment | 当前收包缓冲属于 context 临时 buffer，下一次 recv 会覆盖 | 引入 `PacketIn` 池 + refcnt，fragment 保存 packet 引用和 offset/len |
| `utp_stream_read()` 拷贝到应用 buffer | 传统 read API 的语义就是复制到调用方 buffer | 增加 acquire/commit read views，应用直接消费协议栈 buffer |
| 控制帧 payload 复制到 `PacketOut` | 控制帧很小，当前收益低，且重传依赖完整 packet | 后续 frame builder 直接写 packet 或小帧固定内联 |
| pending incoming 缓存完整 wire packet | pending 阶段尚未有正式 connection/PacketIn 生命周期 | pending 阶段接入 PacketIn 引用池后再移除 |
| crypto key/nonce/transcript 小块复制 | 加密材料构造需要独立存储，且非数据面大块 payload | 保留；敏感材料后续按 crypto 规范显式清理 |
| socket/address/system struct 清零或小块复制 | 平台 ABI 或地址表示转换要求明确布局 | 保留，除非有等价且更清晰的字段初始化 |

## P0 实现顺序

### 1. 发送侧 scatter/gather（已完成）

- C socket 内部发送已支持 slice/iovec：
  - POSIX: `sendmsg`
  - Windows: `WSASendTo`
  - 保留单块 `sendto` 快路径
- `utp_packet_out_t.slices[]` 已成为真实发送路径：
  - slice 0 指向 packet header / 小控制帧区域
  - STREAM data slice 指向 stream send buffer 视图
- `utp_context_flush_connection()` 根据 `slice_count` 选择单块或分片发送。
- `utp_packet_out_t.packet_type` 保存包类型，重传和 sent 回调不再依赖完整 raw payload decode。
- `utp_packet_out_flatten()` 仅作为测试/模拟传输辅助，用于把 slice 包拼成 wire buffer。

STREAM 组包处的大块 payload copy 已删除；仍保留 packet header / stream frame header 的小块写入。

### 2. Stream send buffer 改为 ring/view（已完成内部稳定生命周期）

- `send_buffer` 已改成 ring/offset 模型。
- 已发送未 ACK 的数据保留在 stream buffer 中，PacketOut external slice 可安全指向它。
- ACK 按 stream offset/length 记录并释放连续已 ACK 前缀。
- 发送后的 `memmove` 已删除。

后续仍需补充更完整的乱序 ACK / 重传压力回归，以及应用写入 acquire/commit view。

### 3. 接收侧 PacketIn + refcnt

- UDP 收包写入 PacketIn 池，不再写入 context 单个临时 read buffer。
- `FrameStream` decode 后 data 指针指向 PacketIn 内部 payload。
- recv fragment 保存：
  - packet 引用
  - data offset
  - length
  - stream offset
  - consumed
  - FIN
- fragment 插入时处理重叠裁剪，只保留未接收区间。
- `commitReadViews` 释放 fragment 引用，PacketIn refcnt 到 0 后回池。

完成后删除 STREAM 接收缓存的 payload copy。

### 4. Public/API 层零拷贝视图

- 传统 `write/read` 继续保留，语义上允许复制。
- 新增 API 前必须先稳定内部 view 模型；非必要不扩 API。
- 如果需要暴露，应与 cpp 对齐：
  - acquire write views / commit write
  - acquire read views / commit read

## 当前已完成的低风险收敛

- STREAM 发送路径已使用 PacketOut external slice，去掉 stream payload 到 PacketOut raw buffer 的大块 copy。
- UDP socket 已支持分片发送；POSIX 走 `sendmsg`，Windows 走 `WSASendTo`。
- stream send buffer 已改为 ring/offset 生命周期，去掉发送后的 `memmove`。
- 去掉 stream 初始化时对整个 `utp_stream_t` 的清零，避免清 `send_buffer` 和 recv fragment payload 区。
- 去掉 stream recv fragment 插入时对整个 fragment 的清零，只初始化元数据。
- 去掉 context 创建和 slot 分配/释放中的大对象清零，避免清 `udp_read_buffer`、pending storage、connection/stream payload 区。
- 去掉部分 packet out 池 acquire/release 的整结构清零，改为显式重置有效元数据。
- 去掉 ACK/send history/send ledger/bw sampler/minmax/pacer 等小结构中可明确替代的 `memset`。

## 不应提前做的事

- 不在没有 PacketIn 生命周期的情况下让 recv fragment 指向 UDP 临时缓冲。
- 不在没有 ring/view 生命周期的情况下让 PacketOut slice 指向会被移动或覆盖的 stream buffer。
- 不为减少 API 数量而隐藏必要的 acquire/commit 语义；零拷贝视图必须有明确提交点。
- 不把控制帧和地址转换的小块复制作为优先优化目标。
