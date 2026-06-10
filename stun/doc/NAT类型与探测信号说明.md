# NAT类型与探测信号说明

本文整理 `NTRS/STUN` 侧用于 P2P 穿透决策的 NAT/UDP 可达性类型，以及建议保留的最小探测信号集合。

## 说明

- 标准 `STUN RFC 5389` 只能稳定提供 `srflx` 映射、UDP 基础可达性和粗略 RTT。
- 完整 NAT 分类不能只靠单个 STUN 结果，需要组合：
  - 多 STUN 视角
  - 多轮映射采样
  - filter probe
  - hairpin probe
  - 本地地址/多网卡信息
- 设计原则：
  - 主类型只保留能描述 NAT 拓扑和过滤行为的少数几类
  - UDP 可达性、降级状态和探测可信度放进 flags
  - 业务层优先看 `class` 判断打洞模型，再看 `flags` 决定是否退回 TCP/relay
- 当前实现补充：
  - 探测顺序为 `stun1 -> change-port -> change-ip -> stun2`
  - `change-port` 由本节点 `3479/udp` 回应，`change-ip` 由协同节点 `3478/udp` 代发回应
  - 若未显式指定 `--bind-ip` 或 `--bind-device`，探测逻辑会先根据 `stun1` 的路由选本地出口 IP，并把 probe socket 绑定到该出口，尽量避免多网卡路径漂移导致的误判
  - `ntrs_peer` 会把 `local_ip:local_port` 一并注册，并在会话阶段交换 `host_local` 候选

## 建议主类型

### `open_public`

- 无 NAT，公网直达。
- 需要信号：
  - 本地可用地址为公网地址
  - `local_ip:port` 与 `srflx_ip:port` 一致
  - 多个 STUN 视角下映射一致
  - 映射没有明显抖动

### `full_cone_nat`

- 全锥形 NAT。
- 需要信号：
  - 双 STUN 视角下 `srflx` 一致
  - 多轮探测映射稳定
  - `same_ip_diff_port` filter probe 收到
  - `diff_ip` filter probe 也收到

### `ip_restricted_nat`

- 地址受限锥形 NAT。
- 需要信号：
  - 双 STUN 视角下 `srflx` 一致
  - 多轮探测映射稳定
  - `same_ip_diff_port` probe 收到
  - `diff_ip` probe 收不到

### `port_restricted_nat`

- 端口受限锥形 NAT，或在信息不足时的保守回退类型。
- 需要信号：
  - 双 STUN 视角下 `srflx` 一致
  - 多轮探测映射稳定
  - `same_ip_diff_port` probe 收不到
  - `diff_ip` probe 收不到
- 保守回退：
  - 缺少 `stun2`
  - 或 filter probe 没有执行
  - 但现有信号不足以证明其为 `symmetric_nat`

### `symmetric_nat`

- 对不同目标生成不同映射，或网络环境表现出不可信任的映射不稳定。
- 需要信号：
  - 至少两个不同公网视角的 STUN
  - `srflx1 != srflx2`
  - 或多轮采样中 `probe*_distinct_mappings > 1`
  - 或第二视角可达性差，无法信任直接打洞
  - 或多出口路径导致的外部地址变化

## 建议删除并 flag 化的旧类型

这些状态不再建议保留为独立 `class`：

- `open_public_udp_blocked`
  - 不再作为 NAT 主类型
  - UDP 不通由 `UDP_BLOCKED` 表达，本地是否公网由 `LOCAL_ADDR_PUBLIC` 表达
- `single_stun_limited`
  - 并入 `port_restricted_nat`
  - 探测不完整由 `PROBE_DEGRADED` 表达
- `partial_udp_reachability`
  - 并入 `symmetric_nat`
  - 同时打 `MAPPING_UNSTABLE` 或 `PROBE_DEGRADED`
- `multi_homed_public`
  - 并入 `open_public`
  - 通过 `MULTI_EXTERNAL_IP` 表达多出口
- `multi_homed_nat`
  - 已移除
  - 外部地址变化通过 `MULTI_EXTERNAL_IP` 表达，不再提升成单独主类型

## 建议附加标签

### `carrier_grade_nat_suspected`

- 运营商级 NAT 怀疑标签。
- 需要信号：
  - 本地为私网
  - 外部映射落在共享地址或运营商常见 NAT 区间
  - 端口策略严格、稳定性差或 hairpin 异常

### `hairpin_supported`

- 支持 NAT hairpin / loopback。
- 需要信号：
  - 自身向自身 `srflx` 探测成功
  - 或协作节点辅助下的公网回环探测成功

### `hairpin_unsupported`

- 不支持 hairpin。
- 需要信号：
  - 自回环探测失败
  - 且基础 UDP 连通性正常

### `mapping_unstable`

- 映射不稳定。
- 推荐文案理解：`observed mapping instability`
- 需要信号：
  - 同一视角多轮采样 `srflx` 变化

### `probe_degraded`

- 探测已降级，分类可信度下降。
- 需要信号：
  - 第二视角缺失
  - filter probe 失败或未执行
  - 样本轮数不足

### `udp_blocked`

- UDP 基础路径不可用。
- 需要信号：
  - 主 STUN 连续失败
  - 多轮采样拿不到 `srflx`
  - 控制面正常但 UDP 不通
- 注意事项：
  - 这是可达性 flag，不是 NAT 主类型
  - `OPEN_PUBLIC + UDP_BLOCKED` 表示公网主机 UDP 被防火墙或安全组限制
  - `UNKNOWN + UDP_BLOCKED` 表示 UDP 不通且无法判断 NAT 拓扑

### `multi_external_ip`

- 看到多个外部地址或出口。
- 推荐文案理解：`multiple external IPs observed`
- 需要信号：
  - 双视角或多轮采样看到多个公网映射

### `local_addr_public`

- 本地绑定地址就是公网地址。
- 需要信号：
  - `local_ip:local_port == srflx_ip:srflx_port`

## 建议最小探测信号集合

建议至少统一采集这些信号：

- `local_ip`
- `local_port`
- `srflx_ip`
- `srflx_port`
- `srflx_ip_2`
- `srflx_port_2`
- `probe1_ok`
- `probe2_ok`
- `probe1_rtt_ms`
- `probe2_rtt_ms`
- `probe1_success_count`
- `probe2_success_count`
- `probe1_distinct_mappings`
- `probe2_distinct_mappings`
- `filter_same_ip_diff_port_rx`
- `filter_diff_ip_rx`
- `hairpin_rx`
- `is_local_ip_public`
- `multiple_local_interfaces`
- `multiple_external_ips_seen`

## 信号与类型的对应关系

- 是否公网直达：
  - 看 `local` 与 `srflx`
- 是否对称：
  - 看双 STUN / 多目标映射是否变化
- 是否 `full cone / restricted`：
  - 看 filter probe
- 是否多出口：
  - 看多个外部地址，并通过 `MULTI_EXTERNAL_IP` 标记
- 是否 UDP 被阻断：
  - 看基础 STUN 连通性
- 是否支持 hairpin：
  - 看自回环探测
- 是否同机/同局域网优先连接：
  - 看 `local_ip:local_port`
  - 会话阶段交换 `host_local` 候选，并与 `srflx` 候选并发尝试

## 实现建议

- 主类型只保留少量可直接驱动打洞策略的状态。
- 复杂环境特征通过附加标签表达。
- 分类应优先输出“是否可打洞、风险多高”，再输出精细 NAT 名称。
- 当探测信号不全时，不再返回独立的“信息不足类”，而是回退到保守主类并明确打上 `probe_degraded`。

## 推荐精简后模型

建议最终 NAT 主类收敛为：

- `unknown`
- `open_public`
- `full_cone_nat`
- `ip_restricted_nat`
- `port_restricted_nat`
- `symmetric_nat`

建议最终 flags 收敛为：

- `udp_blocked`
- `mapping_unstable`
- `probe_degraded`
- `multi_external_ip`
- `local_addr_public`
