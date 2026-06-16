# STUN NAT探测流程图

本文描述当前实现的 STUN NAT 探测流程：使用完整异步 full STUN 顺序采集事实，再推导 `MappingBehavior` 和 `FilteringBehavior`，最后组合得到兼容 `NatType` 与 `NatFlags`。随后 `peer` 会把 `local` / `srflx` 候选一并注册给 Node，用于会话协商和并发打洞。

## 流程图

```mermaid
flowchart TD
    A[开始\npeer 创建 control 连接] --> B[AUTH_REQ / AUTH_RSP\n拿 control session token]
    B --> C{是否显式传入 stun1}
    C -- 否 --> D[NAT_PROBE_REQ / NAT_PROBE_RSP\n获取 stun1 / stun2]
    C -- 是 --> E[解析 stun1 / stun2]
    D --> E
    E --> F[创建 probe UDP socket\n如未显式绑定则按 stun1 路由选本地出口 IP 并绑定]
    F --> G[对 stun1 多轮 Binding Request\n记录 srflx1 / RESPONSE-ORIGIN / OTHER-ADDRESS / success / rtt / distinct mappings]
    G --> H{probe1 是否有成功样本}
    H -- 否 --> I[UDP 基础路径不可用\nNatType=UNKNOWN\nFiltering=BLOCKED\nflags: UDP_BLOCKED + PROBE_DEGRADED]
    H -- 是 --> J[向 stun1 发送 CHANGE-REQUEST(change-port)\n由本节点 3479 回应 same_ip_diff_port]
    J --> K[记录 same_ip_diff_port 是否收到]
    K --> L[向 stun1 发送 CHANGE-REQUEST(change-ip)\n由协同 node 3478 代发回应]
    L --> M[记录 diff_ip 是否收到]
    M --> N{是否有 stun2}
    N -- 否 --> O[探测不完整\nMapping=UNKNOWN\nFiltering=UNKNOWN\nNatType=OPEN_PUBLIC 或 UNKNOWN\nflags: PROBE_DEGRADED]
    N -- 是 --> P[对 stun2 多轮 Binding Request\n记录 srflx2 / success / failure / rtt / distinct mappings]
    P --> Q{probe2 是否有成功样本}
    Q -- 否 --> R[第二视角失败\nMapping=UNKNOWN\nFiltering=UNKNOWN\nNatType=UNKNOWN\nflags: MAPPING_UNSTABLE + PROBE_DEGRADED]
    Q -- 是 --> S[推导 MappingBehavior\n同目标抖动 -> UNSTABLE\n双视角一致 -> ENDPOINT_INDEPENDENT\n双视角变化 -> ADDRESS_DEPENDENT]
    S --> T[推导 FilteringBehavior\n两者都收到 -> ENDPOINT_INDEPENDENT\n只 same_ip_diff_port -> ADDRESS_DEPENDENT\n两者都未收到 -> ADDRESS_AND_PORT_DEPENDENT\n对称映射或证据冲突 -> UNKNOWN]
    T --> U[组合 NatType]
    U --> V{local == srflx 且 mapping endpoint independent}
    V -- 是 --> W[NatType=OPEN_PUBLIC\n可叠加 LOCAL_ADDR_PUBLIC]
    V -- 否 --> X{mapping 是否 address dependent 或 unstable}
    X -- 是 --> Y[NatType=SYMMETRIC\n可叠加 MAPPING_UNSTABLE / MULTI_EXTERNAL_IP]
    X -- 否 --> Z{filtering 行为}
    Z -- ENDPOINT_INDEPENDENT --> ZA[NatType=FULL_CONE]
    Z -- ADDRESS_DEPENDENT --> ZB[NatType=IP_RESTRICTED]
    Z -- ADDRESS_AND_PORT_DEPENDENT --> ZC[NatType=PORT_RESTRICTED]
    Z -- UNKNOWN --> ZD[NatType=UNKNOWN\nflags: PROBE_DEGRADED]
    I --> ZE[fill_nat_info]
    O --> ZE
    R --> ZE
    W --> ZE
    Y --> ZE
    ZA --> ZE
    ZB --> ZE
    ZC --> ZE
    ZD --> ZE
    ZE --> ZF[REGISTER_REQ / REGISTER_RSP\n上报 nat_class / nat_flags / mapping_behavior / filtering_behavior]
    ZF --> ZG[SESSION_CREATE / SESSION_NOTIFY\n转发 peer_nat_class / peer_nat_flags / peer_mapping_behavior / peer_filtering_behavior\n并下发 host_local / srflx_primary / srflx_secondary 候选]
    ZG --> ZH[peer 使用与 NAT 探测相同的 UDP socket\n并发向候选地址发 punch 包]
    ZH --> ZI[谁先收到回包就选谁\n同机/同局域网场景优先命中 host_local]
```

## 行为与类型

`NatType` 只表达兼容主类型：

| `NatType` | 来源 |
| --- | --- |
| `UNKNOWN` | 证据不足，或 UDP blocked 但无法判断 NAT 拓扑 |
| `OPEN_PUBLIC` | 本地公网且 `local_ip:port == srflx_ip:port`，mapping endpoint independent |
| `FULL_CONE` | mapping endpoint independent + filtering endpoint independent |
| `IP_RESTRICTED` | mapping endpoint independent + filtering address dependent |
| `PORT_RESTRICTED` | mapping endpoint independent + filtering address-and-port dependent |
| `SYMMETRIC` | mapping address dependent 或 unstable |

`UDP_BLOCKED` 不再作为 `NatType`，只作为 flag，因为它描述 UDP 传输可达性，不描述 NAT 拓扑。

当前实现中，`MULTI_HOMED_NAT` 已被移除；多出口或不同外部 IP 只作为 flag 保留，不再单独提升成主类型。

## Flags

当前建议保留这些可由现有探测链路支撑的 flags：

| Flag | 含义 |
| --- | --- |
| `LOCAL_ADDR_PUBLIC` | 本地绑定地址是公网地址 |
| `UDP_BLOCKED` | 基础 UDP/STUN 路径失败 |
| `PROBE_DEGRADED` | 探测不完整或证据冲突 |
| `MAPPING_UNSTABLE` | 观测到映射不稳定（observed mapping instability） |
| `MULTI_EXTERNAL_IP` | 观测到多个外部 IP（multiple external IPs observed） |

`same_ip_diff_port_rx` 和 `diff_ip_rx` 继续作为原始探测字段保留，但不再作为长期业务 flags；它们用于推导 `FilteringBehavior`。

## 会话候选

当前 `SESSION_CREATE_RSP / SESSION_NOTIFY` 中会下发：

- `host_local`
- `srflx_primary`
- `srflx_secondary`（仅当与 `srflx_primary` 不同才保留）

`peer` 会使用与 NAT 探测相同的 UDP socket 并发尝试这些候选。对同机或同局域网场景，`host_local` 往往会先成功。

## stun_node filter probe 端口

`stun_node` 主 STUN 端口默认 `3478`，同 IP 备用端口默认 `3479`。filter probe 中：

- `same_ip_diff_port` 从本节点备用端口发出，默认 `ipA:3479 -> peer_srflx`。
- `diff_ip` 从另一个 STUN node 的主 STUN 端口代发，默认 `ipB:3478 -> peer_srflx`。
- full STUN 顺序固定为 `stun1 -> change-port -> change-ip -> stun2`，这样可以在直接访问 `stun2` 之前先完成 filtering 判定，避免污染 `change-ip` 结果。
