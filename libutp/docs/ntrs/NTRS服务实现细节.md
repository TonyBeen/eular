# NTRS 服务实现细节

> 更新时间：2026-09-15
> `ntrs` 是 UDP Rendezvous 服务；`natd_hub`/`natd_node` 是独立的 NAT 探测控制服务。

## 1. 服务和数据对象

- `ntrs` 默认监听 UDP `0.0.0.0:6600`；使用 `-6` 时监听 IPv6 `[::]:6600`。IPv4 和 IPv6 使用独立实例，可在同一主机复用端口。服务只处理 Rendezvous/保活/候选协调，不转发业务数据。
- `natd_hub` 默认监听 TCP `0.0.0.0:7700`，可通过 cert/key 为控制连接启用 TLS。
- `natd_node` 默认提供 UDP probe `7800`、UDP change-port `7801`、TCP control `7900`。
- `ntrs_natc` 是 NAT 探测客户端；`nat_punch` 是联调示例。`nat_punch -r` 注册成功后持续运行，逐连接保存被动传输状态；一条连接完成或失败不会反注册或停止接收后续连接。进程收到退出信号后才反注册。主动传输结束时输出当前 PMTU、RTT、带宽估计、UDP 收发字节和重传率。

注册对象按 peer-id 建立，包含当前 UDP endpoint、8 字节 registration token、NAT 分类、本地候选、公网映射、地址样本和保活状态。一次 Rendezvous 使用 16 字节 `attempt_id` 和 8 字节 punch token；CALIBRATE 使用 8 字节 calibration token 和 64 位 calibration id。

## 2. REGISTER / 保活 / 反注册

1. Context 向 NTRS 发送 `REGISTER`，携带 peer-id、绑定端口、本地候选、已探测公网 endpoint 和 NAT 分类。
2. NTRS 记录发送端观察到的 endpoint，生成 registration token，返回 `REGISTERED`；对称 NAT 场景还可下发 calibration endpoint。
3. 客户端默认每 15 秒发送 `PING`。NTRS 回复带 token 和 packet number 的 `PONG`。
4. PONG 丢失后，NTRS 每 1 秒重试，最多 3 次；仍无响应则删除注册。注册租约默认 30 秒，旧 endpoint 不会无限保留。
5. `UNREGISTER` 携带当前 token；成功或已经注销都返回 `UNREGISTERED`，无效 token 返回 `REJECTED`。Context 销毁不自动发送 UNREGISTER，应用应显式调用 API。

重复 REGISTER、PING、UNREGISTER 使用 token/request id 做幂等处理；注册失败通过 `REJECTED` 的消息类型和原因码交付给客户端注册回调。

### 消息身份与地址来源

所有半连接控制均承载在 `UTP_PACKET_TYPE_RENDEZVOUS` 的 `FrameRendezvous` 内；它们不创建 UTP Connection，也不参与普通 ACK、拥塞控制或业务流解复用。REGISTER 携带 peer-id、当前 registration token、NAT 类型、绑定端口、本地候选和 NAT 探测得到的公网 endpoint。NTRS 还记录 UDP 源地址观测值；两者是面向不同目的地得出的映射证据，不能互相覆盖。

`REGISTERED` 回显 request id 并下发新的 8 字节 registration token；仅在需要对称 NAT 校准时附带 calibration id 和副端口。PING/PONG 通过 `(registration_token, acknowledged_packet_number)` 匹配：任一有效关联入站报文都会刷新活跃时间，只有本地 send 成功不能延长对端的存活判定。

反注册删除记录后保留短期 token tombstone。重复 UNREGISTER 命中 tombstone 仍回复 UNREGISTERED，使响应丢失后的重试可收敛；新注册始终分配新 token。

## 3. 地址更新和校准

`ADDRESS_UPDATE` 批量上报对端观察到的公网地址样本，带 update id；NTRS 返回 `ADDRESS_UPDATED`。Context 会合并样本并按容量或防抖超时发送，不要求每个样本单独请求。

对称 NAT 请求建立后，NTRS 可先返回 `CALIBRATE`，要求客户端向指定副端口发送带 calibration id 的 PING。NTRS 用固定 calibration IP 检查路径，记录不同副端口看到的映射端口，随后以 PONG 返回 `(token, packet_number)`，Control 线程再把结果交回产生请求的 Worker。IP 不一致时保留样本并标记多线路，不伪造单一端口预测。

### 地址样本

已建立连接的接收端从当前路径观察到对端公网 endpoint 后，以可靠 `OBSERVED_ADDRESS` 帧返回。注册 Context 将样本去重并批量发送 ADDRESS_UPDATE：累计四条立即发送，未满则在首条样本后最多聚合五秒。每一批有独立 `update_id`；确认前保留不可变快照，期间的新样本进入下一批。NTRS 回复 ADDRESS_UPDATED 仅表示已处理该批，不公开每条样本是否被预测模型采用。

NTRS 只接受与当前有效保活源 IP 一致的样本；公网 IP 变化时清空历史样本和过期预测。ADDRESS_UPDATED 丢失时，Context 按注册选项指数退避重试，耗尽后只记录警告，不影响已建立的 UTP Connection。

## 4. Rendezvous 顺序

主动端 A 向 NTRS 发送包含 `attempt_id`、source/target peer-id、NAT 信息和候选地址的 `REQUEST`。

1. NTRS 先向 A 返回 `REDIRECT`，携带 B 的候选计划和 punch token。
2. NTRS 再向已注册的 B 发送 `FORWARD`，携带 A 的 peer-id、候选计划、attempt_id 和同一 punch token。
3. B 向 A 的候选地址发送 `PUNCH`；A 收到合法 token 后只向对应来源 endpoint 重发保留的 Initial。
4. 后续由普通 UTP Initial/Handshake/HandshakeDone 建立 Connection；NTRS 不转发业务数据。

候选按完整 endpoint 去重，IPv6 还比较 scope；不同端口仍是不同候选。REQUEST/FORWARD 及重试在 pending 生命周期内幂等，旧的 pending、punch token 和 forward owner 超时清理，默认约 30 秒。目标未注册、已失效、地址族不匹配或没有可行候选时，NTRS 静默丢弃 REQUEST，不回复 `REJECTED`，避免通过响应枚举在线 peer-id；请求方由自身的连接超时处理失败。REGISTER/UNREGISTER 的 token 或参数错误仍可返回 `REJECTED`。

### CandidatePlan 和 PUNCH 边界

CandidatePlan 包含同一地址族的本地候选与公网候选。每个公网候选为精确 `IP:port`，不能拆开或交叉组合；本地和公网候选各至多四项。NTRS 依据注册源观测、NAT 探测上报、校准样本和端口模型构造目标候选。请求方的候选只用其 REQUEST 上报与 NTRS 本次观察，不对请求方预测端口。

PUNCH 只负责建立 NAT 映射、放宽过滤并反馈可达路径，不是 PATH_CHALLENGE，也不直接选定已建立连接的迁移路径。局域网、hairpin 和公网候选使用同一接口并可并行尝试。对称 NAT 与端口限制 NAT 的组合仍取决于端口预测是否命中；token 能验证协调归属，不能突破 NAT 的过滤规则。

## 5. 线程模型和队列

每个 UDP Worker 负责收包、协议头/FrameRendezvous 解码和一次性请求处理；相同 attempt_id 通过哈希稳定分派到同一 Worker。Control 线程独占注册、反注册、保活、地址更新和 CalibrationSession 全局表；Worker 通过 MPSC 队列和一对共享 socketpair 通知 Control，Control 的结果再投递到拥有请求的 Worker。Control 在保活扫描前优先排空 PING/PONG 队列，已到达 Worker 的保活包先刷新注册活跃时间，避免双方同时到期时发送冗余 PING。Worker 的 socket 发送使用本地输出队列，其他线程不直接改动其事件循环。

队列容量是启动参数。满时丢弃新的可重试请求并记录日志；注册、反注册和保活任务不能静默丢弃，应用层应通过重试/超时观察结果。socketpair 通知写失败、socket 非 WOULD_BLOCK 致命错误和 event loop 启动失败记录错误码后退出进程，由外部服务管理器重启。

### NAT 探测服务与控制面

`natd_hub` 和 `natd_node` 与 UDP Rendezvous 服务解耦。Node 提供主探测、change-port 探测和到 Hub 的控制连接；Hub 根据 Node 的地址族、角色和健康状态分配 primary/backup 协作节点。Node 控制连接断开使用指数退避重连，达到上限后按固定间隔重试。

NAT 探测 UDP 请求使用独立的 `UTP_PACKET_TYPE_NAT_PROBE`，固定为 128 字节，响应不大于请求。它使用零 CID、独立包号和随机 probe token，不占 Connection 的包号或 ACK 空间。主探测要求 `CHANGE_IP|CHANGE_PORT`，备用探测要求 `CHANGE_PORT`；响应同时校验包号、token、步骤、来源和地址族。该隔离保证 NAT 探测失败不会污染正常 UTP 或 Rendezvous 状态。

NTRS 的 Control/Worker 分离只存在于服务进程内部：Control 独占注册表、租约、tombstone、校准与保活状态；UDP Worker 负责收包、严格解码和本地输出队列。Worker 到 Control 的任务通过有界 MPSC 队列和共享 socketpair 唤醒；结果投递回拥有该事务的 Worker。多 Worker 部署可用 `SO_REUSEPORT` 共享同一 UDP endpoint，但应用侧普通 Context 不因此获得跨线程安全。

## 6. 部署

Linux 构建：

```sh
cd ntrs
cmake --preset ntrs-linux
cmake --build --preset ntrs-linux
ctest --preset ntrs-linux
```

musl 使用 `ntrs-musl` preset。IPv4/IPv6 使用独立实例和同族 endpoint；网卡通过 `--interface` 绑定。
