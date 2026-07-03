# 时间不同步的情况下计算的RTT不准确
在不使用NTP同步时间的情况下只能计算相对时间. (参考TCP)
1、A记录发送的时间t0
2、B收到探测包后记录到达时间t1, 并回复响应, 携带着到达时间t1及响应时间t2
3、A收到响应包记录收到包的时间为t3, RTT = t3 - t0 - (t2 -t1)

### NOTE 重传包不参与RTT计算, 由于无法确定重传包的ACK是哪次传输的, 故计算不准确

然而, 在存在丢包或乱序的情况下, 则可能引入较大的误差. 需要引入加权移动平均法(TCP采用的算法)更新RTT估算值
[指数加权移动平均详解](https://blog.csdn.net/qq_42363032/article/details/117352591)
[[通俗易懂]深入理解TCP协议（下）：RTT、滑动窗口、拥塞处理](https://www.imooc.com/article/29368)

linux/net/ipv4/tcp_input.c:tcp_rtt_estimator L820

## NTRS / P2P 接入清单

当前已完成：

- `kcp_connect_candidates()` 支持主叫端多候选并发 SYN。
- `kcp_connect()` 保持单地址兼容语义。
- 首个完成握手的候选会锁定为最终 `remote_host`。
- 新增 `kcp_ntrs_configure()` / `kcp_ntrs_start()` / `kcp_ntrs_create_session()` API 合约。
- 默认构建提供 NTRS stub，未启用 stun bridge 时返回 `NOT_SUPPORT`，不影响核心 KCP。
- `KCPP_ENABLE_NTRS=ON` 时可直接源码引入 `../stun`。
- 新增 `kcp_peer.out` 示例，复用 KCP UDP socket 完成 NTRS NAT 探测、UDP punch 和 KCP 候选连接。
- `build-musl-ntrs/examples/kcp_peer.out` 已可静态 musl 构建。

下一步：

- `kcp_ntrs_start()` 完成 connect/auth/request_probe/detect/register/wait_signal 状态机。
- `kcp_ntrs_create_session()` 主叫端从 Node 获取候选后调用 `kcp_connect_candidates()`。
- 在 `kcp_read_cb()` 增加 STUN/KCP 分流。
- 将 `kcp_peer.out` 验证通过的流程沉淀回 `kcp_ntrs_*` 库 API。
- 删除或重写旧 `examples/ntrs_kcp_client.cc`，移除旧文本协议和伪造候选。

后续硬化：

- 增加 `attempt_id`，合并被动端同一逻辑连接的多个候选 SYN。
- 增加候选级日志和测试。
