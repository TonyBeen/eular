# TODO

## Stream 接收分片上限

当前每个 Stream 使用固定的 `UTP_STREAM_RECV_FRAGMENT_LIMIT`（1024）个有序重组分片槽位，Connection
另有限制全部 Stream 重组分片数量的计数。该限制独立于 `MAX_STREAM_DATA` 的字节流控窗口；大量极小且乱序的
STREAM 帧可能先耗尽分片槽位。

后续评估改为由每流接收窗口控制可保留的分片数据。实施时需一并设计按需增长的分片索引、单帧多空洞插入的
原子回滚，以及 PacketIn 与分片元数据的内存计费上限，避免仅删除数量限制后被极小分片无限占用内存。

## Stream 可写通知合并

当前发送缓存以已确认的连续前缀释放空间。一个 ACK 帧确认多份 STREAM 包时，逐包释放会对同一 Stream
重复同步调用 `on_writable`；应用在回调中立即填满发送 ring buffer，会形成许多不足一个 MTU 的小写入。

实施方向：

- 发送 ring buffer 的可写低水位为配置容量的一半；空闲空间从低于该值跨越到不低于该值时才通知可写。
- Stream 被应用写到空闲空间低于该低水位后，才重新允许下一次通知。
- 一个 ACK 帧完成所有确认、释放和丢包处理后，对每个受影响 Stream 最多通知一次。
- 设置回调时，若当前空闲空间已经达到低水位，仍立即通知一次。

该方案借鉴 `epoll` edge-triggered 的边沿通知，但不等同于 UDP socket 的 `EPOLLOUT`：Stream 可写性还受
发送缓存、拥塞控制、Pacer 和流控共同约束。应用有新的业务数据时仍可主动调用写入接口；可写回调不作为唯一
发送驱动。

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
