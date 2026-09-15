# NTRS 服务实现细节

> 更新时间：2026-09-15
> `ntrs` 是 UDP Rendezvous 服务；`natd_hub`/`natd_node` 是独立的 NAT 探测控制服务。

## 1. 服务和数据对象

- `ntrs` 默认监听 `0.0.0.0:24000`，只处理 UDP Rendezvous/保活/候选协调，不转发业务数据。
- `natd_hub` 默认监听 TCP `0.0.0.0:24000`，可通过 cert/key 为控制连接启用 TLS。
- `natd_node` 默认提供 TCP control `24003`、UDP probe `24001`、UDP change-port `24002`。
- `ntrs_natc` 是 NAT 探测客户端；`nat_punch` 是联调示例。

注册对象按 peer-id 建立，包含当前 UDP endpoint、8 字节 registration token、NAT 分类、本地候选、公网映射、地址样本和保活状态。一次 Rendezvous 使用 16 字节 `attempt_id` 和 8 字节 punch token；CALIBRATE 使用 8 字节 calibration token 和 64 位 calibration id。

## 2. REGISTER / 保活 / 反注册

1. Context 向 NTRS 发送 `REGISTER`，携带 peer-id、绑定端口、本地候选、已探测公网 endpoint 和 NAT 分类。
2. NTRS 记录发送端观察到的 endpoint，生成 registration token，返回 `REGISTERED`；对称 NAT 场景还可下发 calibration endpoint。
3. 客户端默认每 15 秒发送 `PING`。NTRS 回复带 token 和 packet number 的 `PONG`。
4. PONG 丢失后，NTRS 每 1 秒重试，最多 3 次；仍无响应则删除注册。注册租约默认 30 秒，旧 endpoint 不会无限保留。
5. `UNREGISTER` 携带当前 token；成功或已经注销都返回 `UNREGISTERED`，无效 token 返回 `REJECTED`。Context 销毁不自动发送 UNREGISTER，应用应显式调用 API。

重复 REGISTER、PING、UNREGISTER 使用 token/request id 做幂等处理；注册失败通过 `REJECTED` 的消息类型和原因码交付给客户端注册回调。

## 3. 地址更新和校准

`ADDRESS_UPDATE` 批量上报对端观察到的公网地址样本，带 update id；NTRS 返回 `ADDRESS_UPDATED`。Context 会合并样本并按容量或防抖超时发送，不要求每个样本单独请求。

对称 NAT 请求建立后，NTRS 可先返回 `CALIBRATE`，要求客户端向指定副端口发送带 calibration id 的 PING。NTRS 用固定 calibration IP 检查路径，记录不同副端口看到的映射端口，随后以 PONG 返回 `(token, packet_number)`，Control 线程再把结果交回产生请求的 Worker。IP 不一致时保留样本并标记多线路，不伪造单一端口预测。

## 4. Rendezvous 顺序

主动端 A 向 NTRS 发送包含 `attempt_id`、source/target peer-id、NAT 信息和候选地址的 `REQUEST`。

1. NTRS 先向 A 返回 `REDIRECT`，携带 B 的候选计划和 punch token。
2. NTRS 再向已注册的 B 发送 `FORWARD`，携带 A 的 peer-id、候选计划、attempt_id 和同一 punch token。
3. B 向 A 的候选地址发送 `PUNCH`；A 收到合法 token 后只向对应来源 endpoint 重发保留的 Initial。
4. 后续由普通 UTP Initial/Handshake/HandshakeDone 建立 Connection；NTRS 不转发业务数据。

候选按完整 endpoint 去重，IPv6 还比较 scope；不同端口仍是不同候选。REQUEST/FORWARD 及重试在 pending 生命周期内幂等，旧的 pending、punch token 和 forward owner 超时清理，默认约 30 秒。目标注册失效或双对称 NAT 不具备可行候选时，NTRS 返回 `REJECTED`。

## 5. 线程模型和队列

每个 UDP Worker 负责收包、协议头/FrameRendezvous 解码和一次性请求处理；相同 attempt_id 通过哈希稳定分派到同一 Worker。Control 线程独占注册、反注册、保活、地址更新和 CalibrationSession 全局表；Worker 通过 MPSC 队列和 socketpair 通知 Control，Control 的结果再投递到拥有请求的 Worker。Worker 的 socket 发送使用本地输出队列，其他线程不直接改动其事件循环。

队列容量是启动参数。满时丢弃新的可重试请求并记录日志；注册、反注册和保活任务不能静默丢弃，应用层应通过重试/超时观察结果。socketpair 通知写失败、socket 非 WOULD_BLOCK 致命错误和 event loop 启动失败记录错误码后退出进程，由外部服务管理器重启。

## 6. 部署

Linux 构建：

```sh
cd ntrs
cmake --preset ntrs-linux
cmake --build --preset ntrs-linux
ctest --preset ntrs-linux
```

musl 使用 `ntrs-musl` preset。IPv4/IPv6 使用独立实例和同族 endpoint；网卡通过 `--interface` 绑定。
