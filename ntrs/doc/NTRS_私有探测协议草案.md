# NTRS 私有探测协议草案

## 摘要

NTRS 不再以标准 STUN/RFC5780 作为长期公开探测协议，而是采用私有 NAT 探测和打洞协议。目标是减少默认公开暴露面，避免 `3478/3479` 固定端口和标准 STUN 线协议指纹，同时保留当前双节点 NAT 分类、候选生成和打洞编排能力。

## 控制面下发内容

控制面至少需要向 client 下发：

- `probe_endpoint_primary`
- `probe_endpoint_secondary`
- `probe_token`
- `session_id`
- `target_node_id`
- 当前允许的 `phase`

控制面仍负责把 peer/session/phase 与数据面探测绑定起来。

## 数据面私有消息类型

私有 UDP 探测包采用全量二进制帧，不使用文本包头。允许个别业务字段内容为文本，例如 `peer_id`，但线协议 framing、类型、长度和校验必须全部是二进制字段。

私有 UDP 探测包至少包括：

- `PROBE_REQ`
- `PROBE_RSP`
- `FILTER_REQ`
- `FILTER_RSP`
- `PUNCH_REQ`
- `PUNCH_ACK`

保留现有双节点协同语义：

- `probe1`
- `change-port`
- `change-ip`
- `probe2`

## 最小包字段

所有私有探测/打洞包都必须是二进制定长或带长度前缀的二进制结构，至少带：

- `version`
- `msg_type`
- `request_id`
- `probe_token`
- `phase`
- `sequence`
- `timestamp`
- `auth_tag`

响应包根据需要附带：

- `mapped_addr`
- `origin_addr`
- `other_addr`
- 对应 request 的 `sequence/request_id`

## `probe_token` 方向

`probe_token` 采用控制面签发的短期带 MAC token。

建议 token 载荷包含：

- `ver`
- `kid`
- `exp`
- `nonce`
- `peer_id`
- `session_id`
- `target_node_id`
- `phase_mask`
- 端点信息或 flags

建议校验方式：

- `HMAC-SHA256(secret[kid], payload)`
- 可截断 MAC，缩短包长
- token 过期、phase 不符、target node 不符时静默丢弃

默认不绑定完整 `src_ip:src_port`，避免 NAT 变化导致误杀；如需绑定，优先只绑定更稳定的信息。

## 结果判定语义

NTRS 结果不能只有成功/失败，必须输出结构化结果，至少区分：

- `SUCCESS`
- `PROBABLE_LOSS`
- `NO_RESPONSE`
- `BLOCKED`
- `LOCAL_SEND_FAILED`
- `ICMP_UNREACHABLE`
- `TIMED_OUT`

### NAT 探测判定要求

- 不允许“一次超时就判死”。
- 需要多轮发送和多数成功规则。
- 本地明确发送失败与纯超时必须区分。
- 明确 ICMP 不可达与纯无响应必须区分。

### UDP 打洞判定要求

- 成功依据必须是合法 `PUNCH_REQ/ACK` 闭环。
- 不允许“收到任意 UDP 包就视为成功”。
- `PUNCH_ACK` 不允许继续触发回包，避免自激回环。

### MTU/探测失败解释要求

- 若后续示例层继续做 MTU 测试，示例也应区分：
  - 本地 `EMSGSIZE`
  - 对端未回 ACK
  - 疑似丢包
- 不把所有 probe timeout 都直接解释为 MTU 过大。

## 端口和暴露面

- 不再默认固定 `3478/3479`。
- node 应通过配置决定私有 probe 端口。
- 数据面端口只对携带合法 token 的私有包响应。
- 对未知包、公开 STUN 包、非法 token 包一律静默丢弃。

## 当前范围约束

本草案只服务于 `ntrs + kcp` 路线：

- 不定义 `utp` 接口
- 不定义 TURN
- 不定义对第三方标准 STUN client 的兼容模式
