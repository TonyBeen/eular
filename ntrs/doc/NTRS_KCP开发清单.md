# NTRS KCP 开发清单

## 摘要

本清单定义将现有 `stun`/NTRS 能力演进为同级独立私有库 `ntrs/` 的开发路线，当前范围仅包含 `ntrs + kcp`，不包含 `utp`。目标是让 `ntrs` 成为 KCP 的统一 NAT 探测、会话编排和 UDP 打洞底座，同时保持 KCP 数据面、socket 生命周期和传输语义继续由 `kcp` 自己负责。

当前已知基础：

- `kcp_connect_candidates()` 已存在，可完成多候选并发连接与最终路径收敛。
- `kcp_ntrs_configure()` / `kcp_ntrs_start()` / `kcp_ntrs_create_session()` 已有 public API 合约，但实现仍为 stub。
- `kcp_peer.out` 已验证“同一 UDP socket 复用 + NAT 探测 + UDP 打洞 + KCP 建连”的示例链路可行。
- 现有 `stun` 仍以标准 STUN/RFC5780 语义和 `3478/3479` 默认端口为主，需要迁移为私有 NTRS 协议。

私有探测协议、控制面鉴权、`probe_token` 与 `probe_auth` 的当前实现见 `NTRS_私有探测协议说明.md`。

## 阶段 1：建立 `ntrs` 项目骨架

### 目标

在同级目录下建立 `ntrs/` 项目根，作为后续私有 NAT 探测/打洞库的唯一正式入口。

### 必做项

- 新建基础目录：`include/`、`src/`、`examples/`、`doc/`。
- 新建独立 `CMakeLists.txt`，支持作为同级库被 `kcp` 引入。
- 明确过渡期关系：
  - 旧 `../stun` 短期可作为实现来源。
  - 新增与长期维护一律以 `ntrs` 为主，不再继续扩展 `stun` 语义。
- 将现有 `kcp` 中的 `KCPP_STUN_ROOT`、`KCPP_ENABLE_NTRS` 等构建入口规划为未来切换到 `../ntrs`。

### 验收标准

- `ntrs/` 可单独作为 sibling project 被发现。
- 文档、源码、示例目录结构固定。
- 旧 `stun` 不再被视为长期主项目根。
- `ntrs/include` 已作为公共头入口建立，后续对外接口优先从 `ntrs` 暴露。

## 阶段 2：固定 `ntrs` 与 `kcp` 的边界

### 目标

把 `ntrs` 定义为私有 NAT 探测与会话编排库，把 `kcp` 定义为数据面和传输层适配方。

### 必做项

- `ntrs` 只负责：
  - control/auth/session，包括 `bootstrap_token` 到短期 `session_token` 的控制面鉴权
  - `session_id` / `peer_session_token` 的短期会话授权
  - 私有 NAT 探测
  - `probe_token` 匹配与跨节点 `probe_auth` HMAC 授权
  - UDP 打洞
  - 候选选择
  - 结果诊断
- `ntrs` 不负责：
  - 创建或绑定 UDP socket
  - 绑定网卡、绑定 IP、设置 DF
  - 关闭上层传入的数据面 socket
  - KCP 握手/收发/重传
  - RTT/丢包/MTU 示例统计
- `kcp` 适配层负责：
  - 创建并持有唯一 UDP socket
  - 调用 `ntrs` 完成 NAT 探测与打洞
  - 复用同一 socket 进入 `kcp_listen()` / `kcp_connect_candidates()`
- 保留并实现 `kcp_ntrs_*`，但语义固定为 thin adapter，不再把协议实现放进 `kcp`。

### 验收标准

- 文档中不再要求 `ntrs` 修改 socket 绑定、关闭或 socket 策略。
- `kcp_ntrs_*` 的责任边界明确且稳定。
- 结构化探测结果、`probe_token` 匹配语义和 `probe_auth` 授权语义已在 `ntrs/include` 建立稳定入口。

## 阶段 3：私有 NAT 探测协议替代公开 STUN

### 目标

把现有标准 STUN/RFC5780 NAT 探测迁移为 NTRS 私有协议，默认不暴露公开 `3478/3479` 端口和标准线协议指纹。

### 必做项

- 去掉“标准 STUN 是长期主协议”的前提。
- 将控制面下发的探测端点语义统一为私有 `probe endpoints`。
- 私有 NAT 探测与打洞线协议必须采用全量二进制帧，不再使用任何文本包头。
- 仅字段内容本身允许是文本，例如 `peer_id`、`device_id`，但 framing、消息类型、长度、时间戳和校验字段都必须是二进制。
- 私有 UDP 包类型至少包括：
  - `PROBE_REQ`
  - `PROBE_RSP`
  - `FILTER_REQ`
  - `FILTER_RSP`
  - `PUNCH_REQ`
  - `PUNCH_ACK`
- 保留双节点协同探测能力：
  - `change-port`
  - `change-ip`
  - `probe2`
- 数据面探测包必须带短期 `probe_token`，用于匹配请求、响应和探测阶段。
- 跨节点 probe 请求必须带 `probe_auth`，当前实现为 `HMAC-SHA256(shared_secret, payload)`。
- 默认不再硬编码 `3478/3479`，改为 node 显式配置私有 probe 端口。
- 探测默认使用 IPv4；示例工具支持 `-4/--ipv4` 和 `-6/--ipv6` 显式选择地址族，IPv6 endpoint 使用 `[ipv6]:port` 格式。
- IPv6 第一阶段只要求可达性、超时和过滤行为可观测，不要求复用 IPv4 NAT44 类型分类。
- 非法 token、过期授权、错误 phase 一律静默丢弃，不返回标准 STUN 错误响应。

### 验收标准

- 探测端口对标准 STUN `Binding Request` 无响应。
- 抓包中不再出现标准 STUN magic cookie、Binding type 和标准 attribute 作为主探测协议。
- 双节点 NAT 分类链路仍可表达现有 mapping/filtering 推断需求。
- IPv6 模式下可以完成二进制 probe/filter 收发，并输出 IPv6 专属可达性结果。

## 阶段 4：KCP 薄适配落地

### 目标

让 `kcp` 通过稳定的 NTRS adapter API 接入 NAT 探测和打洞，而不再依赖示例流程和临时桥接代码。

### 必做项

- 为 `ntrs` 设计统一会话驱动入口，供 `kcp_ntrs_*` 转发使用。
- `kcp_ntrs_configure()`：负责保存 NTRS 控制面与本地配置。
- `kcp_ntrs_start()`：负责触发 control/auth/request_probe/detect/register/wait_signal 状态机。
- `kcp_ntrs_create_session()`：负责主叫端会话创建、候选获取和后续候选连接触发。
- `kcp` 数据面继续复用：
  - `kcp_bind()` 的 UDP socket
  - `kcp_connect_candidates()`
- `kcp_read_cb()` 或等效读路径需支持分流：
  - NTRS 私有探测/打洞包
  - KCP 握手与数据包
- 删除或重写旧 `examples/ntrs_kcp_client.cc`，不再保留伪造 NAT 字段和旧文本协议路径。

### 验收标准

- `kcp_ntrs_*` 不再返回固定 `NOT_SUPPORT`。
- KCP 通过同一 UDP socket 完成 NAT 探测、打洞和最终建连。
- 示例中已验证的 `kcp_peer.out` 链路能沉淀回库 API，而不是只停留在 example。

## 阶段 5：可解释结果模型

### 目标

`ntrs` 在 NAT 探测、打洞和探测失败判定时，必须区分“偶发丢包/超时”和“真正不可达/本地发送失败/被阻断”，不能只返回布尔值。

### 必做项

- 为 NAT 探测与打洞定义结构化结果：
  - `SUCCESS`
  - `PROBABLE_LOSS`
  - `NO_RESPONSE`
  - `BLOCKED`
  - `LOCAL_SEND_FAILED`
  - `ICMP_UNREACHABLE`
  - `TIMED_OUT`
- 结果结构至少包含：
  - 发送次数
  - 接收次数
  - ack 次数
  - timeout 次数
  - 本地 socket error
  - phase
  - selected candidate
- 判定规则采用多信号综合和保守策略：
  - 本地明确发送失败，直接归类本地错误。
  - 明确 ICMP 不可达，归类不可达。
  - 仅单次或少量超时但此前有回包，优先归类为高概率丢包。
  - 多轮无任何响应，才归类 `NO_RESPONSE/BLOCKED/TIMED_OUT`。
- 打洞成功条件必须绑定合法 `PUNCH_REQ/ACK` 闭环，而不是“收到任意 UDP 包”。

### 验收标准

- 同样是失败，可以区分是：
  - 包没发出去
  - 明确被拒绝
  - 仅疑似丢包
  - 完全无响应
- 上层 KCP adapter 可以获得结构化失败原因，而不需要再猜测。

## 当前已验证能力

以下能力可作为迁移基础，但不代表架构已稳定：

- `kcp_connect_candidates()` 主叫端多候选并发连接。
- 首个完成握手的候选会锁定为最终 `remote_host`。
- `kcp_ntrs_*` public API 合约已存在。
- `kcp_peer.out` 已能复用同一 UDP socket 执行探测、打洞和 KCP 候选连接。
- `build-musl-ntrs/examples/kcp_peer.out` 已可通过静态 musl 构建。

## 直接开发顺序

建议实施顺序固定为：

1. 建立同级 `ntrs/` 项目骨架与文档。
2. 从旧 `stun` 中提炼 `ntrs` 核心职责，固定边界。
3. 把公开 STUN 探测替换为私有 probe/punch 协议。
4. 实现 `kcp_ntrs_*` 到 `ntrs` 的真实适配。
5. 再做结果模型和失败原因收敛。

## 非本轮范围

- `utp` 或 `libutp` 适配。
- TURN。
- 把 RTT/丢包/MTU 示例探测沉入核心库。
- “伪装成其他公开协议”的逃逸设计。
