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
| `utp_stream_write()` 将应用数据写入 stream send buffer | 传统 write API 必须解耦应用缓冲生命周期，并保留明文用于 ACK 前重传 | 内部已增加 acquire/commit write view，允许后续 API 层直接写入 ring buffer |
| `utp_stream_on_frame()` raw 数据帧 | raw packet 没有可借用生命周期，不能被 recv fragment 持有 | 有数据的 STREAM frame 必须走 `PacketIn`；无数据 FIN 可保留 raw 兼容 |
| `utp_stream_read()` 拷贝到应用 buffer | 传统 read API 的语义就是复制到调用方 buffer | 内部已增加 acquire/commit read view，后续 public/API 层可映射到该模型 |
| 控制帧 payload 复制到 `PacketOut` | 控制帧很小，当前收益低，且重传依赖完整 packet | 后续 frame builder 直接写 packet 或小帧固定内联 |
| 加密 0-RTT 客户端在 HandshakeDone 前收到乱序 CTRL | 已创建但尚未向应用暴露的 pending Connection 可持有 PacketIn 生命周期 | Connection 零拷贝保留至多一个 PacketIn；派生 1-RTT 密钥后送入正常收包路径。Context 只做 CID 路由，不持有该缓存 |
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

内部 write view 已完成：

- `utp_stream_acquire_write_views()` 借出 stream send ring 的可写片段，wrap 时最多返回两个 view。
- `utp_stream_commit_write_views()` 只提交调用方已写入的数据；普通写与零拷贝写都通过
  `utp_stream_close()` 单独关闭本地写方向，不在 write/commit 接口中传递 FIN。
- `utp_stream_close()` 将 FIN 排在已有数据之后；最后一段待发送数据可携带 FIN，无待发送数据时生成零长度
  STREAM + FIN。关闭本地写方向后仍可继续读取对端数据。
- 传统 `utp_stream_write()` 继续保留拷贝语义，作为兼容路径。

后续仍需补充更完整的乱序 ACK / 重传压力回归。

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

当前状态：正式 UDP 收包路径已接入 PacketIn 池和 refcnt；`utp_connection_on_packet_in_received()` 将 PacketIn 传入 STREAM 解码路径，recv fragment 只保存 packet 引用、data pointer 和 length，数据消费或 stream/connection cleanup 时释放。有数据的 raw STREAM frame 会被拒绝，避免保存无生命周期指针；pending replay 会先从 context PacketIn 池借包包装完整 wire image，再交给 connection。

PacketIn 池采用稳定分块分配：每次增长 64 项，拆为 8 个、每块 8 项的独立 allocation，因此扩容不会移动被 STREAM 重组等路径持有的 PacketIn。已借出项没有总数上限；`utp_context_options_t::packet_in_max_free` 仅限制归还后的空闲缓存，默认 256。超过该水位时只回收完整空闲块，不拆分仍有借用项的块；因此在碎片化借用时，空闲数可暂时略高于配置值。内部批量借用接口会先预留整批 PacketIn，供 Linux `recvmmsg` 接入时使用。

内部 read view 已完成：

- `utp_stream_acquire_read_view()` 借出当前连续首片的只读视图。
- `utp_stream_commit_read_view()` 按 offset/length 提交消费并释放 fragment / PacketIn 引用。
- FIN 在数据消费完后通过零长度 view 单独暴露。传统 `utp_stream_read()` 在暂时没有连续数据时返回
  `UTP_INTERNAL_ERROR_WOULD_BLOCK`，在对端 FIN 已到达且数据已读尽时返回 `UTP_INTERNAL_ERROR_CLOSED`，两者不得混淆。

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
- 去掉 stream recv fragment 的内嵌 payload storage，接收侧 payload 生命周期由 PacketIn 管理。
- 去掉 context 创建和 slot 分配/释放中的大对象清零，避免清 pending storage、connection/stream payload 区。
- 去掉部分 packet out 池 acquire/release 的整结构清零，改为显式重置有效元数据。
- 去掉 ACK/send history/send ledger/bw sampler/minmax/pacer 等小结构中可明确替代的 `memset`。

## 不应提前做的事

- 不在没有 PacketIn 生命周期的情况下让 recv fragment 指向 UDP 临时缓冲。
- 不在没有 ring/view 生命周期的情况下让 PacketOut slice 指向会被移动或覆盖的 stream buffer。
- 不为减少 API 数量而隐藏必要的 acquire/commit 语义；零拷贝视图必须有明确提交点。
- 不把控制帧和地址转换的小块复制作为优先优化目标。
