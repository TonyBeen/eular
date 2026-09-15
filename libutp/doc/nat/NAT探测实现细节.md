# NAT 探测实现细节

> 更新时间：2026-09-15
> 实现位于 `c/src/nat/nat.c` 和 `c/src/context/context.c`，公共入口位于 `c/include/utp/nat.h`。

## 1. 入口和生命周期

`utp_context_probe_nat()` 启动一个异步任务，同一 Context 同时只能有一个任务。任务使用 NAT_PROBE 包类型、独立的包号和 8 字节 probe token，不占用 Connection 的 CID/包号空间。结果通过 `utp_on_nat_probe_fn` 回调交付；取消使用 `utp_context_cancel_nat_probe()`，主动取消不调用完成回调。

`phase_timeout_ms` 是每个阶段的总时限，0 时采用 3000 ms。完整轮次因本地 socket 写入失败而无法提交时，任务以 I/O 状态失败；远端响应不足则以 `UTP_STATUS_OK` 返回 `UNKNOWN` 分类，而不是把网络无响应误报成 UDP_BLOCKED。

## 2. Binding 请求和响应

每个 NAT_PROBE BindingRequest 都携带：版本、消息类型、探测步骤、Change 标志、probe token 和填充 TLV。

- `PRIMARY_BINDING`：向主探测 endpoint 发送，同时请求服务端以 `CHANGE_IP|CHANGE_PORT` 返回。服务端响应包含主映射、origin 和备用探测 endpoint；主响应与切换地址响应属于同一次 Binding 请求的两种证据。
- `ALTERNATE_BINDING`：向备用 endpoint 发送，请求 `CHANGE_PORT`，用于区分地址/端口过滤行为及确认备用映射。
- 每轮会成批发送主/辅助探测包，等待一个证据窗口；当前实现最多两个窗口。已获得足够证据时提前结束，不再等待完整预算。

响应必须同时通过包号、token、步骤、响应来源和地址族校验。重复响应按 response mask 去重；IPv6 地址比较保留 family、地址和 scope 语义。

## 3. 分类决策

当前 IPv4 决策顺序为：

1. 没有合法主响应：`UNKNOWN`。
2. 主端点在未向备用 endpoint 主动发包前收到 ChangeIP|ChangePort 响应：`FULL_CONE`，可提前结束。
3. 主/备用映射的 IP 不一致，或同一目的地映射发生变化：`SYMMETRIC_MULTI_LINE`。
4. 映射 IP 相同但端口不同：`SYMMETRIC`。
5. 映射一致且 ChangePort 成功：`IP_RESTRICTED`。
6. 映射一致且 ChangePort 失败：`PORT_RESTRICTED`。

若绑定地址本身就是公网地址，且映射与本地地址一致，则根据 Change 响应覆盖为 `OPEN_PUBLIC` 或 `OPEN_PUBLIC_WITH_FIREWALL`。IPv6 使用相同的开放/防火墙判定，但不输出 IPv4 NAT 的对称分类。`UDP_BLOCKED` 当前仅为兼容枚举值，探测器不会产出。

端口样本最多保存 `UTP_NAT_PORT_SAMPLE_CAPACITY` 个，并按端口去重；结果带有主/备用 RTT、地址族和单调时间有效期。

## 4. 与 NTRS 的关系

NAT 结果不会改变 UTP 的普通握手语义。注册 NTRS 或发送 Rendezvous REQUEST 时，Context 可携带 NAT 分类、主映射和本地候选地址；对称或多线路结果供 NTRS 决定是否执行 calibration。NAT 探测失败只影响本次探测任务，不影响已有 Connection。

## 5. 工具

NAT 探测客户端为 `ntrs_natc`；Hub/Node 服务为 `natd_hub` 和 `natd_node`。构建和默认端口见 `ntrs/README.md`。Node 主探测默认使用 UDP `7800`。
