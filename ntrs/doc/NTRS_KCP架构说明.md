# NTRS KCP 架构说明

## 摘要

`ntrs` 是与 `kcp` 同级的私有 NAT 探测与 UDP 打洞库。它负责控制面、私有 NAT 探测协议、候选选择和打洞编排；`kcp` 通过薄适配层接入，并继续负责 UDP socket 生命周期、KCP 握手、连接管理和数据收发。

当前范围只覆盖 `ntrs + kcp`，不涉及 `utp`。

私有探测协议和鉴权细节见 `NTRS_私有探测协议说明.md`。本文只描述 `ntrs` 与 `kcp` 的模块边界和数据流。

## 分层

### `ntrs/core`

负责：

- control/auth/session
- `bootstrap_token` 到短期 `session_token` 的控制面鉴权
- `session_id` / `peer_session_token` 的短期会话授权
- `probe_token` 匹配与跨节点 `probe_auth` HMAC 授权
- 私有 NAT 探测协议编解码
- NAT 分类状态机
- UDP 打洞状态机
- 候选选择与打洞策略
- 结构化结果输出

不负责：

- 创建或绑定 UDP socket
- 设置 bind-device / bind-ip / DF
- KCP 握手或数据收发
- 示例层 RTT/MTU/loss 统计

### `ntrs/runtime`

负责：

- `node` / `hub` / `peer` / `nat_detect` 示例与联调工具
- 私有 probe responder、调试日志、验证脚本

### `kcp` 适配层

负责：

- 持有并复用 `kcp_bind()` 创建的同一个 UDP socket
- 把 `kcp_ntrs_*` 调用转发到 `ntrs` 会话接口
- 将 `selected candidate` 交给 `kcp_connect_candidates()` 或 `kcp_listen()`
- 在 UDP 读路径对 NTRS 包与 KCP 包做分流

## 数据流

1. 上层创建 `KcpContext`。
2. `kcp_bind()` 绑定唯一 UDP socket。
3. `kcp_ntrs_configure()` 注入 node/auth/local 参数。
4. `kcp_ntrs_start()` 启动 NTRS：
   - control connect/auth，使用 `bootstrap_token` 换取短期 `session_token`
   - request probe endpoints
   - NAT detect
   - register
   - wait signal
5. `kcp_ntrs_create_session()`：
   - 主叫端发起会话
   - 获取对端候选
   - 完成 UDP hole punch
6. `ntrs` 返回选中的 candidate 和诊断结果。
7. `kcp` 继续使用同一个 UDP socket 调 `kcp_connect_candidates()` 或被动 accept 流程。

## 私有协议约束

- NTRS 数据面协议统一使用二进制帧，不再使用文本前缀识别消息类型。
- `peer_id`、`device_id` 等业务字段内容可以是文本，但必须通过二进制 TLV 或定长二进制字段承载。
- `probe/punch` 二进制帧的消息类型、阶段、请求编号、序列号、长度和认证字段必须全部是二进制。
- 当前实现中 `probe_token` 用于匹配探测上下文；跨节点授权使用独立 `probe_auth = HMAC-SHA256(shared_secret, payload)`。
- 当前默认使用 IPv4 UDP 探测，示例工具通过 `-4/--ipv4` 和 `-6/--ipv6` 显式选择地址族；IPv6 endpoint 使用 `[ipv6]:port` 格式。IPv6 现阶段用于可达性和过滤判断，不直接复用 IPv4 NAT44 分类结论。

## 接口边界

### `ntrs` 对 `kcp` 的要求

- 传入一个已创建、已绑定、可收发的 UDP socket。
- 由 `kcp` 保证 socket 生命周期。
- `ntrs` 只允许读写、收包分流和 `getsockname/getpeername/getaddrinfo` 类信息获取。

### `kcp` 不应再承担的内容

- 不在 `kcp` 核心里直接实现私有 probe/punch 协议。
- 不在 `kcp` 核心里继续扩展公开 STUN 语义。
- 不将 NTRS 示例代码直接复制进 `kcp` 协议栈。

## 旧 `stun` 的过渡关系

- 旧 `../stun` 现阶段仍可作为实现来源和验证来源。
- 新架构以 `ntrs` 为长期主项目根，不再继续扩大 `stun` 公开协议角色。
- 旧 `STUN1/STUN2/STUN_ENDPOINT` 语义应迁移为私有 `probe endpoints`。

## 当前 KCP 侧已存在基础

- `kcp_p2p_candidate_t`
- `kcp_connect_candidates()`
- `kcp_ntrs_configure()` / `kcp_ntrs_start()` / `kcp_ntrs_create_session()` / `kcp_ntrs_stop()` 合约
- `kcp_peer.out` 示例链路

这些能力是 NTRS 接入 KCP 的基础，但 `kcp_ntrs_*` 当前仍需要由 stub 迁移到真实 adapter。
