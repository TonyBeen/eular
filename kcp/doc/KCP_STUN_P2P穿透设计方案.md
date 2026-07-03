# KCP 原生集成 STUN/P2P 穿透设计方案

## 当前实现状态

已完成：

- 新增 `kcp_p2p_candidate_t` 与 `kcp_connect_candidates()`。
- `kcp_connect()` 保持原有单地址语义，并作为单候选包装。
- 主叫端同一个 `KcpConnection` 可以向多个候选地址并发发送 `SYN`。
- 收到任一候选地址的有效握手响应后，连接会锁定该地址为最终 `remote_host`。
- 后续 KCP 数据继续走选中的单一路径。
- 新增 `kcp_ntrs_configure()` / `kcp_ntrs_start()` / `kcp_ntrs_create_session()` API 合约。
- 默认构建提供 NTRS stub，未启用 stun bridge 时返回 `NOT_SUPPORT`。
- `KCPP_ENABLE_NTRS=ON` 时可源码引入 `../stun`。
- 新增 `kcp_peer.out` 示例，使用 KCP UDP socket 执行 NTRS NAT 探测、UDP punch 和 KCP 候选连接。

未完成：

- `KcpContext` 真实内嵌 NTRS 控制面。
- `KcpContext` 内嵌完整异步 NAT 探测。
- `kcp_ntrs_start()` / `kcp_ntrs_create_session()` 的真实异步 bridge 实现。
- 被动端按 session/attempt 合并多候选入站 `SYN`。

当前开发顺序：

1. 先完成 KCP 多候选握手和路径收敛。
2. 再把 NTRS 控制面作为可选模块接入 `KcpContext`。
3. 最后把 `../stun` 的异步 NAT 探测迁入 KCP UDP socket 分流路径。

## 1. 背景与目标

当前 KCP 已具备完整的 UDP 传输、握手、收发和连接管理能力，但 P2P 穿透仍停留在示例层，尚未纳入原生连接流程。目标是将 STUN/NTRS 候选获取、多地址并发握手和最终路径收敛整合进现有 KCP 协议与 `kcp_connect()` 调用链，尽量缩减用户调用手续。

本方案遵循以下目标：

- 允许修改现有 KCP 协议，增加穿透相关握手语义
- 继续复用 `kcp_bind()` 创建并持有唯一 UDP socket
- 继续复用 `kcp_connect()` 作为统一连接入口
- 支持多候选地址并发发起 `SYN`
- 当某个地址率先完成握手后，立即收敛到单一路径并停止其他候选
- 用户侧尽量保持“`kcp_bind()` + `kcp_connect()`”的调用方式

本方案优先解决以下问题：

- 多网卡局域网地址与 `srflx` 地址并发尝试时的状态一致性
- 被动端收到同一次连接尝试的多个源地址 `SYN` 时的归并问题
- 主动端选中最终路径后，其余候选的取消与迟到包处理问题

## 2. 总体方案

### 2.1 用户接口语义

用户侧调用流程保持为：

1. `kcp_context_create()`
2. `kcp_bind()`
3. `kcp_connect()`

其中：

- 单地址输入时，行为保持与当前版本一致
- 多候选地址输入时，`kcp_connect()` 自动进入“多候选并发握手”模式
- 候选地址可来自：
  - 多个局域网地址
  - 对端 `srflx` 地址
  - 对端第二 `srflx` 地址

用户不需要显式调用额外的打洞接口。当前第一阶段由上层在调用前收集候选并传给 `kcp_connect_candidates()`；后续阶段再由 `KcpContext` 内部完成 NTRS/NAT 流程。

### 2.2 Socket 复用原则

全流程继续复用 `kcp_bind()` 创建的 UDP socket，不引入 attach-fd 类接口：

- STUN 响应接收
- UDP 打洞相关控制报文
- KCP 握手报文
- 后续 KCP 数据报文

都走同一个本地 UDP socket，以保证 NAT 映射连续。

### 2.3 与 `../stun` 的关系

`../stun` 目录已具备 STUN 编解码、NTRS 信令和 NAT 探测样例能力。本方案不重写 STUN 协议，而是复用以下组件：

- `stun.h` / `stun.cpp`
- `ntrs_codec.h` / `ntrs_codec.cpp`
- 已验证字段：
  - `srflx_ip`
  - `srflx_port`
  - `srflx_ip_2`
  - `srflx_port_2`
  - `mapping_stable`
  - `nat_type`
  - `nat_risk`
  - `probe1_*`
  - `probe2_*`
  - `filter_*`

KCP 核心不直接承载复杂的 NTRS TCP 控制逻辑，但需要能接收候选地址集合，并在原生连接状态机中完成多候选握手和收敛。

## 3. 协议与接口设计

### 3.1 `kcp_connect()` 扩展方向

为兼容现有调用方式，同时支持多候选连接，当前采用新增 API：

- `kcp_connect()` 保留现有单地址语义
- `kcp_connect_candidates()` 支持多候选并发连接

候选结构至少包含：

- 目标地址
- 候选类型
  - `host_local`
  - `srflx_primary`
  - `srflx_secondary`
- 优先级

连接接口扩展后仍满足以下约束：

- 成功回调只触发一次
- 失败回调只触发一次
- 一条逻辑连接最多选中一个最终远端地址

### 3.2 协议握手扩展

允许修改现有 KCP 握手协议，推荐在现有 `SYN / SYN-ACK / ACK` 握手基础上增加扩展字段，而不是完全引入一套独立命令。

后续协议硬化时建议增加字段：

- `attempt_id`
  - 标识一次逻辑连接尝试
  - 同一条逻辑连接的所有候选 `SYN` 使用同一个 `attempt_id`
- `candidate_seq`
  - 当前候选的序号
- `candidate_count`
  - 可选，标识本次总候选数

设计原则：

- `scid/dcid` 继续表示最终连接双方的连接标识
- `attempt_id` 用于将多个不同 IP 地址上的握手归并为同一逻辑连接
- 被动端按 `attempt_id + peer identity` 识别同一连接尝试

### 3.3 被动端归并规则

被动端收到多个不同地址的 `SYN` 时，最终应按逻辑连接归并，而不是为每个源地址新建连接。

当前第一阶段尚未实现 `attempt_id` 归并。被动端仍通过现有 `on_kcp_connect_t` 与 `kcp_accept()` 接入；一旦某条路径被接受并建立，连接会锁定该 `remote_host`。

归并键固定为：

- `attempt_id`
- 发起方身份标识

处理规则如下：

1. 第一次收到该 `attempt_id` 的 `SYN`，创建一个 pending inbound entry
2. 后续若收到同一 `attempt_id`、但源地址不同的 `SYN`，归并到同一个 pending entry
3. 被动端可以对多个候选地址分别回复 `SYN-ACK`
4. 一旦某个候选路径上的握手完成，被动端锁定该地址
5. 其他候选路径上的后续 `SYN / ACK / retransmit` 一律忽略，必要时可回复 `RST`

### 3.4 主动端收敛规则

主动端内部维护“一个逻辑连接、多个候选路径”的状态：

1. 调用 `kcp_connect_candidates()` 时创建一条逻辑连接。
2. 对所有候选并发发送同一轮 `SYN`。
3. 超时重传时继续向所有候选发送 `SYN`。
4. 第一条成功返回有效握手响应的候选成为最终路径。
5. 连接进入 `CONNECTED` 后，后续发送只走选中的 `remote_host`。
6. 后续若收到其他候选上的迟到包，因 `remote_host` 不匹配会被忽略。

### 3.5 一致性要求

必须保证以下不变量：

- 一条逻辑连接最多只有一个最终 `selected_remote`
- 一旦选中某路径，后续所有发送只走该地址
- 已经被取消的候选不能再推动连接状态
- 已经建立的连接不能被迟到的其他候选握手包切换路径
- 被动端不会因同一 `attempt_id` 创建多条重复连接

## 4. 内部状态模型

### 4.1 连接级状态

- `INIT`
- `SYN_SENT`
- `SYN_RECV`
- `ESTABLISHED`
- `FAILED`
- `CLOSED`

### 4.2 候选级状态

- `IDLE`
- `SYN_INFLIGHT`
- `SYN_ACK_RECV`
- `SELECTED`
- `CANCELLED`
- `FAILED`

### 4.3 状态推进规则

- 连接状态与候选状态分离维护
- 候选可并行推进，但连接只允许一个候选把状态推进到 `ESTABLISHED`
- 一旦某个候选进入 `SELECTED`，其他候选只能进入 `CANCELLED` 或 `FAILED`
- 所有迟到握手包必须先校验 `attempt_id` 和最终选中地址，再决定是否处理

## 5. `../stun` 集成方式

### 5.1 候选来源

多候选地址来源包括：

- 本地多网卡局域网地址
- 对端主 `srflx` 地址
- 对端第二 `srflx` 地址

第一阶段重点是将这些候选地址纳入统一的 `kcp_connect_candidates()` 并发握手模型，而不是在 KCP 内部完整重做 STUN/NTRS 控制层。

### 5.2 接入原则

- `../stun/examples/ntrs_client.cc` 中的探测和候选收集逻辑应提炼为 helper
- 不直接把整个 example 逻辑复制到 KCP 核心
- KCP 核心只依赖“候选集合”和必要的 NAT 诊断结果

### 5.3 UDP 读路径分流

后续接入 NTRS/NAT 后，`kcp_bind()` 后的同一个 socket 需要同时承载 STUN、穿透控制和 KCP 报文，读路径需要支持分流：

- STUN 响应包交给 STUN 解析逻辑
- 穿透相关控制包交给候选握手/会话逻辑
- 标准 KCP 握手或数据包交给现有 KCP 协议栈

若 NTRS 继续走独立 TCP 控制连接，则其消息不进入 UDP 收包分流逻辑。

## 6. 示例与文档

需要升级或替换 `examples/ntrs_kcp_client.cc`，使其满足以下要求：

- 不再伪造 `srflx_*` 字段
- 能基于真实候选地址调用 `kcp_connect_candidates()`
- 支持多候选并发握手
- 在收到某路径成功响应后停止其他候选

建议日志阶段包括：

- `bind_ok`
- `candidate_prepare_ok`
- `connect_attempt_start`
- `syn_sent`
- `syn_ack_selected`
- `other_candidates_cancelled`
- `kcp_connected`
- `kcp_connect_failed`

本设计文档作为后续实现与联调的单一规格说明，文件固定放置于：

- `doc/KCP_STUN_P2P穿透设计方案.md`

## 7. 测试计划

### 7.1 基础回归

- 单地址 `kcp_connect()` 行为保持不变
- 现有普通 client/server echo 不受影响
- 现有 soak/echo 测试继续通过

### 7.2 多候选连接测试

- 两个局域网地址同时传给 `kcp_connect_candidates()`，验证只建立一条连接
- 一个局域网地址与一个 `srflx` 地址并发尝试，验证只收敛到一个最终路径
- `primary` 候选失败、`secondary` 候选成功
- 收到一个候选的 `SYN-ACK` 后，验证其他候选停止重传

### 7.3 被动端归并测试

- 被动端收到同一 `attempt_id` 的多个 `SYN`，只创建一个逻辑连接
- 某一路径先建立后，其他路径上的迟到 `SYN/ACK` 被忽略或回收

### 7.4 穿透联动测试

- 结合 `../stun` 生成 `peer_srflx_ip/port` 和 `peer_srflx_ip_2/port_2`
- 将候选集传给扩展后的 `kcp_connect()`
- 完成 P2P 建连并跑通 echo

### 7.5 长稳测试

- 在 P2P 建连成功后跑长时 echo/soak
- 记录阶段耗时、成功率和失败原因

## 8. 假设与边界

- 首版允许修改 KCP 握手协议和 option 字段
- 首版继续复用 `kcp_bind()` 创建的唯一 UDP socket
- 首版重点是“多候选一致性收敛”和“缩减用户调用手续”
- 首版不实现 TURN
- 首版不承诺对称 NAT 一定成功，但要能明确标识高风险或失败原因

## 9. 开发清单

### 已完成

- `kcp_connect_candidates()` public API。
- 单地址 `kcp_connect()` 兼容包装。
- 主叫端多候选 SYN 并发发送。
- 主叫端收到候选响应后锁定选中 `remote_host`。
- `kcp_ntrs_configure()` / `kcp_ntrs_start()` / `kcp_ntrs_create_session()` public API 合约。
- 默认 NTRS stub，保证未链接 `../stun` 时核心库仍可独立构建。
- `KCPP_ENABLE_NTRS=ON` 构建 `kcp_peer.out`。
- `kcp_peer.out` 使用 `ntrs_async_detect_nat()` 复用 `kcp_bind()` 的 UDP socket。
- `build-musl-ntrs/examples/kcp_peer.out` 静态 musl 构建通过。
- 核心库与 examples 编译通过。

### 下一阶段

- `kcp_ntrs_start()` 完成 connect/auth/request_probe/detect/register/wait_signal 状态机。
- `kcp_ntrs_create_session()` 主叫端通过 Node 获取候选并调用 `kcp_connect_candidates()`。
- 在 `kcp_read_cb()` 中加入 STUN/KCP 包分流。
- 将 `kcp_peer.out` 验证通过的流程沉淀回 `kcp_ntrs_*` 库 API。
- 删除或重写旧 `examples/ntrs_kcp_client.cc`，移除旧文本协议和伪造 NAT 字段。

### 后续硬化

- 增加 `attempt_id`，用于被动端合并同一逻辑连接的多个候选 `SYN`。
- 增加候选级状态和统计日志。
- 增加多候选回归测试与同 LAN / 跨 NAT 集成测试。
