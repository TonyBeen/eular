# TODO

## Stream 可写通知合并

当前发送缓存以已确认的连续前缀释放空间。一个 ACK 帧确认多份 STREAM 包时，逐包释放会对同一 Stream
重复同步调用 `on_writable`；应用在回调中立即填满发送 ring buffer，会形成许多不足一个 MTU 的小写入。

实施方向：

- 发送 ring buffer 容量为 32768 字节，空闲空间从低于 16384 字节跨越到不低于 16384 字节时才通知可写。
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
