# libutp 接收链路设计：有序分片与引用计数

## 1. 概述

当前 C 实现的接收路径使用固定上限的按 offset 排序分片数组，以及 PacketIn 引用计数管理内存生命周期。不使用红黑树，也不为每个分片单独分配节点。

## 2. 核心组件

### 2.1 有序接收分片 (`utp_stream_recv_fragment_t`)

`utp_stream_t` 使用固定容量的 `recv_fragments[]` 按 offset 保存 `utp_stream_recv_fragment_t`。

- 插入时按流 offset 定位，重叠范围被裁剪，只保留尚未接收的字节。
- 每个 Stream 和 Connection 都有分片/内存预算，超过预算时拒绝新的乱序片段。
- 分片记录 offset、length、consumed、FIN，以及所引用的 PacketIn 数据视图。

### 2.2 PacketIn 引用计数

- UDP 层接收的数据封装在池分配的 `utp_packet_in_t` 中。
- 分片引用 PacketIn 时增加 `ref_count`；应用通过 `utp_stream_commit_read_view()` 消费数据后释放分片引用。
- `ref_count` 归零后 PacketIn 返回所属池；只要仍有分片引用，底层缓冲就不能复用。

## 3. 数据流向

### 3.1 接收与重组

1. `recvmmsg`/`recvmsg` 将数据写入 PacketIn 池提供的缓冲区。
2. 协议层解析 STREAM 帧，计算帧数据在 PacketIn 中的偏移。
3. 重组层构造 `utp_stream_recv_fragment_t`，直接持有 PacketIn 数据地址并插入有序数组。
4. 新帧与已有分片重叠时裁剪重复范围，仅保留未接收部分。

### 3.2 应用层读取

1. `utp_stream_acquire_read_view()` 返回当前连续分片的数据视图。
2. 应用在回调或事件循环中处理该视图；视图只在对应 commit 前有效。
3. `utp_stream_commit_read_view()` 提交消费范围，释放完整分片并递减 PacketIn 引用计数。

## 4. 设计边界

- 该路径减少了协议层到应用层的复制，但不是“无限持有”的零成本视图；应用必须及时 commit，避免占满 PacketIn 和分片预算。
- 复制式 `utp_stream_read()` 仍然可用，适合应用不需要生命周期管理视图的场景。
- 乱序分片的数量和内存成本都受 Stream/Connection 上限约束；资源耗尽通过状态码反馈，不在热路径无限扩容。
