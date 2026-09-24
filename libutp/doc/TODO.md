# TODO

## ACK-of-ACK 接收历史回收

当前 Connection 的 `receive_history` 固定保留 1000 个范围，ACK 仅从最新范围开始按当前 PacketOut 可用空间
编码，且不拆分单个 ACK frame。该固定上限保证接收热路径不分配内存，但在极端乱序下仍会按既有淘汰逻辑
提高 `cutoff`。

后续可参考 lsquic，在“携带 ACK 的本端发送 attempt 已被对端确认”后回收已安全过期的接收历史。实现时应在
构造 ACK 时记录所确认的最大对端包号，并为每个发送 attempt 保留该快照。不能直接以 ACK 的最大确认号推进
`cutoff`：ACK range 可能存在间隙，且重传会剥离 transient ACK；须先定义等价于 STOP_WAITING 的安全回收
语义，再调用 `utp_receive_history_stop_wait()`。

## 延迟 ACK 的旧快照与乱序反馈

当前 ACK-only PacketOut 是 `receive_history` 的瞬态快照。若 UDP `WOULD_BLOCK` 将其重新排队，且此后收到新的
ack-eliciting 包，发送前会丢弃旧 ACK-only PacketOut，并从最新接收历史重新构造 ACK。这样不会先发送旧快照、再紧随
一份新 ACK。

仍需完善以下部分：

- ACK 与 STREAM 或可靠 control 合包后，若该 PacketOut 因 `WOULD_BLOCK` 留在发送队列，不能直接丢弃，因为其中包含
  业务数据或语义 control。需要设计可安全剥离、重建 transient ACK 前缀的机制，并保持 frame metadata、外部 STREAM
  数据视图、加密封装和重传语义正确；在设计完成前允许该类包发送旧 ACK，并由后续 ACK 覆盖新历史。
- 当前立即 ACK 仅覆盖收到的包号超过此前最大包号且中间缺口达到阈值的情形。对于较小包号的迟到包，需参考 lsquic 的
  `WM_SMALLER` / `ACK_HAD_MISS` 语义，先确定 UTP 的立即反馈条件，避免在乱序微分片下制造 ACK 小包风暴。
- 补充 Context/socket 层确定性测试：覆盖 `sendmmsg` 部分发送、真实 `WOULD_BLOCK`、加密包及 ACK-only 被重建后的
  发送顺序，确认旧 ACK 不会先于新快照写入 socket。

## 发送 ACK 区间资源控制

当前 Stream 的 `send_ack_ranges` 使用动态、按偏移合并的区间表。乱序 ACK 不能再因固定区间数被丢弃，
否则已确认的数据无法从发送缓存前缀退休，最终会错误地停止可写通知。

仍需设计资源预算和过载策略：区间数量应与 Stream 发送缓存、在途 packet 数或连接级预算关联；应提供
区间数和分配失败的可观测性，并补充极端乱序/丢包压测。该策略不能重新引入固定 16、32 等数目上限来
静默忽略 ACK；资源不足必须作为明确的连接级错误处理。

## Linux PMTU 缓存兼容性

评估 Linux `IP_PMTUDISC_PROBE` 是否适合 UTP 的 DPLPMTUD。该选项保留 DF 位，并使配置它的 UDP socket
发送时忽略内核 PMTU 缓存；它不能阻止无效 ICMP 到达内核，也不能阻止路由缓存被错误钳制至
`net.ipv4.route.min_pmtu`。因此它只能让当前 UTP socket 不受该缓存影响，不能消除系统级缓存污染。

它是 socket 级选项，同一 Context 的多个连接共用 UDP socket。还需验证异步 ICMP 错误不会被后续普通业务
发送误归因，并通过错误队列将其准确归属到 MTU probe；完成多连接和错误队列测试后才能决定是否作为默认行为。

## Peer-ID 借用与变长编码

当前 `peer_id` 和 `target_peer_id` 的公共 API、Context 内部存储及 Rendezvous 编解码都限制为 1～128 字节。
后续取消 UTP 协议层的固定最大长度：UTP 不复制、不拥有 peer-id，只借用调用方提供的地址和长度。线上不为
peer-id 预留固定容量，只编码 `length + value`，接收端先读取长度字段，再按实际长度读取 peer-id 内容。

这意味着外层必须保证 peer-id 的存储地址和内容在对应 Context 的整个生命周期内始终有效，包括 Context 注册、所有异步
连接尝试、Rendezvous/NTRS 重传以及相关回调。Context 销毁前不得修改或释放该存储；UTP 不应在内部保存悬空指针，也不应
通过隐式复制来延长或缩短其生命周期。

实施时需要同步调整：

- 公共 API 改为显式的指针+长度输入，明确 0 长度仍表示空值/非法，而不是默认值；
- Context、连接尝试和 Rendezvous 临时对象只保存借用视图，避免固定 129 字节数组导致截断或复制；
- REGISTER、REQUEST、FORWARD 等消息的长度字段宽度及最大包长检查；
- NTRS 的 peer-id 哈希/索引和日志输出，避免以固定 128 字节比较或复制；
- 超长 peer-id 在 MTU floor、分片/重传和内存预算下的行为测试；必要时由上层协议决定是否拒绝，UTP 不增加固定长度上限。

当前实现仍以 128 字节为生效上限；本项只记录协议/API 扩展方向，未改变现有代码或文档中的当前限制。
