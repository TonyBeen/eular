# 0-RTT 协议同步文档（v1）

## 1. 目标

0-RTT 走“首包建连”模型：客户端首包为 `UTP_TYPE_0RTT`，不依赖 `INITIAL` 作为前置。

## 2. connect0Rtt 接口约束

- 新增 `connect0Rtt` 接口，参数由用户显式提供：
  - 目标地址与端口
  - SessionToken（必填）
  - 可选首包数据 earlyDataOpt（可空）
- 不提供自动回退。0-RTT 失败后由业务层自行调用普通 connect。
- 首包若带数据，必须保证完整包大小不超过 1280（含 UTP 头和所有帧）。

## 3. 首包内容

- 首包类型：`UTP_TYPE_0RTT`
- 必带帧：`SessionToken`
- 可选帧：`Stream`
- 首包允许无应用数据（仅携带 SessionToken）。

## 4. 密钥与票据

- 采用“自包含加密 Ticket + anti-replay”。
- 0-RTT 首包不依赖本次 x25519 完成后再加密。
- 票据验证通过后可派生恢复密钥并进入数据处理。

## 5. 服务端处理顺序

收到 0-RTT 首包后：

1. 校验 SessionToken（有效期/完整性/anti-replay）。
2. 校验通过后回调 `OnNewConnection`（允许 local_cid 缺省）。
3. 若应用拒绝，不分配本地 cid。
4. 若应用接受，再分配本地 cid、创建连接并转入 connected。
5. 若首包有数据，建连后立即处理并回 ACK。

## 6. 回调顺序

主动侧在 0-RTT 路径下要求顺序：

1. `OnConnected`
2. `setOnIncomingStream`

原因：`setOnIncomingStream` 注册的回调依赖用户在 `OnConnected` 中注册的逻辑。

## 7. 失败处理与拒绝信号

- token 校验失败、重放、应用拒绝时，默认回 `ConnectionClose`，尽快终止主动侧重试。
- 推荐错误码：
  - `INVALID_TOKEN`
  - `REPLAY`
  - `APP_REJECT`
- 需要做限频和限次，避免反射放大。

## 8. 地址绑定策略

- 不做严格地址绑定，适配 P2P/多网卡/NAT 变化场景。
- 允许在可校验前提下放宽地址一致性。

## 9. HandshakeDone 语义

- 0-RTT 路径不要求 HandshakeDone。
- 主动侧以首包被 ACK 作为连接成功依据。

## 10. 兼容性说明

- 传统握手（INITIAL/HANDSHAKE）可并存。
- 0-RTT 路径独立于 pending-incoming 预创建流程。