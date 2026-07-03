# NTRS 私有探测协议说明

## 摘要

NTRS 不以标准 STUN/RFC5780 作为长期公开探测协议，而是采用私有 NAT 探测和打洞协议。目标是减少默认公开暴露面，避免 `3478/3479` 固定端口和标准 STUN 线协议指纹，同时保留双节点 NAT 分类、候选生成和打洞编排能力。

本文档描述当前 `ntrs` 实现中的控制面鉴权、会话 token、私有探测授权和后续协议演进方向。

当前公开 API 位于 `include/ntrs/ntrs.h`。内部协议、codec、auth、io、probe types 等头文件位于 `src/ntrs/`，不作为外部 API 暴露。

## 控制面鉴权

控制面鉴权采用两阶段模型：

1. client 使用 `bootstrap_token` 发起 `AUTH_REQ`。
2. node 校验 `bootstrap_token` 后签发短期 `session_token`。
3. 后续控制面请求携带 `session_token`，node 按连接 fd、peer_id、过期时间进行校验。

### `bootstrap_token`

`bootstrap_token` 是 client 初次接入控制面时提交的引导密钥。node 侧对应 `ControlAuthManager` 的 `shared_secret`。

- 默认值为 `ntrs-dev-secret`。
- 生产环境必须通过 `--auth-secret` 或等价配置替换为强随机密钥。
- `bootstrap_token` 只用于换取短期 `session_token`，不直接作为后续控制请求的长期凭据。

### `session_token`

`session_token` 是 node 签发的短期控制会话 token，当前格式为：

- `ctl_` 前缀
- 16 字节随机数
- 32 个十六进制字符编码

node 将 `session_token` 绑定到当前控制连接 fd，并记录：

- `peer_id`
- `token`
- `fd`
- `expire_at_sec`

后续请求通过 `validateSession(fd, peer_id, session_token, now_sec)` 校验：

- fd 必须存在已签发 session。
- `session_token` 必须匹配。
- 如果请求指定 `peer_id`，必须与 session 中的 `peer_id` 一致。
- 当前时间必须早于 `expire_at_sec`。
- 校验成功后刷新 session 过期时间。

当前默认 session TTL 为 30 秒，属于滑动续期。

## 控制面请求鉴权范围

以下请求必须携带有效 `session_token`：

- `REGISTER_REQ`
- `HEARTBEAT_REQ`
- `UNREGISTER_REQ`
- `SESSION_CREATE_REQ`
- `NAT_PROBE_REQ`
- `FILTER_PROBE_REQ`
- 节点联邦请求，例如 `SERVER_INFO_REQ`、`SERVER_SEND_PROBE_REQ`

其中：

- peer 普通请求通常校验 `fd + peer_id + session_token`。
- NAT probe 类请求可只校验 `fd + session_token`，再从 session 中取 owner peer id。
- 节点联邦请求使用固定 peer id `service_node_federation` 进行控制面 session 校验。

## Peer 会话授权

当一个 peer 请求与另一个 peer 建立穿透会话时，node 处理 `SESSION_CREATE_REQ`：

1. 校验发起方的控制面 `session_token`。
2. 确认发起方 peer/device 已注册在当前 fd 上。
3. 查找目标 peer/device 是否在线。
4. 生成 `session_id` 和 `peer_session_token`。
5. 返回目标地址、目标 NAT 分类、角色、打洞策略和短期会话凭据。

当前格式：

- `session_id`: `sid_` + 16 字节随机数的 hex 编码
- `peer_session_token`: `peer_` + 16 字节随机数的 hex 编码

peer session 由 `ControlAuthManager` 保存，并带有独立过期时间。

## 随机 token 生成

控制面 session、peer session id、peer session token 均使用随机 token。

当前随机源优先级：

1. `std::random_device`
2. 平台安全随机源
   - Linux: `getrandom()`
   - Unix-like fallback: `/dev/urandom`
   - Windows: `rand_s()`
3. 极端情况下使用弱随机兜底，保证探测流程可继续

弱随机兜底只用于系统安全随机源不可用的异常环境。正常生产环境应命中前两级随机源。

## 私有 UDP 探测

私有 UDP 探测包采用二进制帧，不使用标准 STUN 文本或公开线协议指纹。允许个别业务字段内容为文本，例如 `peer_id`，但 framing、类型、长度和校验字段应使用二进制结构。

NAT 探测流程图见 [`ntrs-nat-detect-flow.svg`](ntrs-nat-detect-flow.svg)。

当前实现默认使用 IPv4 探测。`natc`、`natc_multi`、`peer`、`node` 示例支持通过 `-4/--ipv4` 或 `-6/--ipv6` 选择 UDP 探测地址族；IPv6 endpoint 使用 `[ipv6]:port` 文本格式，避免和端口分隔符冲突。

IPv6 当前作为可选探测能力接入，用于验证 IPv6 UDP 可达性和过滤行为；不把 IPv4 的 NAT44 类型分类逻辑直接套用到 IPv6。IPv6 结果应按 `ipv6_reachable`、`ipv6_filtered`、`ipv6_timeout` 等语义解释，后续如需支持 NAT66/NPTv6 细分类，应单独定义判定规则。

私有 UDP 探测包至少覆盖：

- `PROBE_REQ`
- `PROBE_RSP`
- `FILTER_REQ`
- `FILTER_RSP`
- `PUNCH_REQ`
- `PUNCH_ACK`

保留双节点协同语义：

- `probe1`
- `change-port`
- `change-ip`
- `probe2`

## 控制面 NAT 字段

客户端注册和会话信令只传递对打洞决策必要的 NAT 信息：

- `LOCAL_IP` / `LOCAL_PORT`
- `SRFLX_IP` / `SRFLX_PORT`
- `SRFLX_IP_2` / `SRFLX_PORT_2`
- `PROBE1_OK`
- `PROBE2_OK`
- `PROBE1_RTT_MS`
- `PROBE2_RTT_MS`
- `PROBE_ROUNDS`
- `PROBE1_SUCCESS_COUNT`
- `PROBE2_SUCCESS_COUNT`
- `PROBE1_DISTINCT_MAPPINGS`
- `PROBE2_DISTINCT_MAPPINGS`
- `NAT_CLASS`

以下历史字段不再作为客户端 NAT 结果或 peer 信令字段使用：

- `NAT_FLAGS`
- `MAPPING_BEHAVIOR`
- `FILTERING_BEHAVIOR`
- `PEER_NAT_FLAGS`
- `PEER_MAPPING_BEHAVIOR`
- `PEER_FILTERING_BEHAVIOR`
- `PEER_NAT_TYPE`

`NAT_TYPE` 仍可用于 node/hub 集群状态描述服务节点自身类型，不用于客户端 NAT 探测结果。客户端侧只使用 `NAT_CLASS`。

## 探测包字段

私有探测/打洞包至少应携带：

- `version`
- `msg_type`
- `request_id`
- `probe_token`
- `phase`
- `sequence`
- `timestamp`
- `auth_tag` 或等价授权字段

响应包根据需要附带：

- `mapped_addr`
- `origin_addr`
- `other_addr`
- 对应 request 的 `sequence/request_id`

## `probe_token` 与 `probe_auth`

当前实现中需要区分两个概念：

- `probe_token`: 探测流程中的随机 token，用于匹配请求和响应。
- `probe_auth`: 跨节点 probe 授权 HMAC，用于证明某个节点被允许向指定目标发送 probe。

当前 `probe_auth` 由 `MintProbeAuthorization()` 生成，签名消息为：

```text
probe|owner_peer_id|target_ip|target_port|probe_token|expire_at_sec
```

签名算法：

```text
HMAC-SHA256(shared_secret, payload)
```

输出为 32 字节摘要的 64 字符十六进制字符串。

接收方通过 `ValidateProbeAuthorization()` 校验：

- `owner_peer_id` 非空
- `target_ip` 非空
- `target_port != 0`
- `probe_token` 非空
- `authorization` 非空
- `expire_at_sec` 未过期
- HMAC 重新计算结果匹配

默认不绑定完整 `src_ip:src_port`，避免 NAT 映射变化导致误杀。如需绑定，应优先绑定更稳定的上下文信息。

## 结果判定语义

NTRS 内部探测不能只有成功/失败，内部日志和后续扩展可以区分：

- `SUCCESS`
- `PROBABLE_LOSS`
- `NO_RESPONSE`
- `BLOCKED`
- `LOCAL_SEND_FAILED`
- `ICMP_UNREACHABLE`
- `TIMED_OUT`

### 对外 NAT 分类

当前公开 API 不再暴露 mapping/filtering 细分类，也不再暴露 `nat_type` 字符串。对外只返回 `ntrs_nat_class_t`：

- `NTRS_NAT_CLASS_UNKNOWN`
- `NTRS_NAT_CLASS_OPEN_PUBLIC`
- `NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL`
- `NTRS_NAT_CLASS_FULL_CONE`
- `NTRS_NAT_CLASS_IP_RESTRICTED`
- `NTRS_NAT_CLASS_PORT_RESTRICTED`
- `NTRS_NAT_CLASS_SYMMETRIC`
- `NTRS_NAT_CLASS_SYMMETRIC_MULTI_LINE`
- `NTRS_NAT_CLASS_UDP_BLOCKED`

`ntrs_nat_info_t` 保留 endpoint 与探测辅助统计：

- 本地 UDP endpoint
- 主/辅探测端点观察到的公网映射 endpoint
- 主/辅探测是否成功
- 主/辅探测 RTT
- 探测轮数
- 主/辅探测成功次数
- 主/辅探测观察到的不同公网映射数量

mapping/filtering 推断仍可作为内部中间状态参与分类，但不进入公开 API 或 peer 信令。

### NAT 探测判定要求

- 不允许“一次超时就判死”。
- 需要多轮发送和多数成功规则。
- 本地明确发送失败与纯超时必须区分。
- 明确 ICMP 不可达与纯无响应必须区分。
- UDP 完全不可达时归类为 `NTRS_NAT_CLASS_UDP_BLOCKED`。
- 多公网 IP 或多线路映射时归类为 `NTRS_NAT_CLASS_SYMMETRIC_MULTI_LINE`。

### UDP 打洞判定要求

- 成功依据必须是合法 `PUNCH_REQ/ACK` 闭环。
- 不允许“收到任意 UDP 包就视为成功”。
- `PUNCH_ACK` 不允许继续触发回包，避免自激回环。

### MTU/探测失败解释要求

若后续示例层继续做 MTU 测试，示例也应区分：

- 本地 `EMSGSIZE`
- 对端未回 ACK
- 疑似丢包

不把所有 probe timeout 都直接解释为 MTU 过大。

## 端口和暴露面

- 不再默认固定 `3478/3479`。
- node 应通过配置决定私有 probe 端口。
- 数据面端口只对携带合法 token 或合法私有探测上下文的包响应。
- 对未知包、公开 STUN 包、非法 token 包一律静默丢弃。

## 后续演进

当前实现采用随机 `probe_token` + 独立 `probe_auth` 的方式。后续可以演进为自描述 MAC token：

- token payload 包含 `ver`、`kid`、`exp`、`nonce`、`peer_id`、`session_id`、`target_node_id`、`phase_mask`、端点 flags。
- token 使用 `HMAC-SHA256(secret[kid], payload)`。
- 可截断 MAC 以缩短包长。
- token 过期、phase 不符、target node 不符时静默丢弃。

## 当前范围约束

本文档只服务于 `ntrs + kcp` 路线：

- 不定义 `utp` 接口
- 不定义 TURN
- 不定义对第三方标准 STUN client 的兼容模式
