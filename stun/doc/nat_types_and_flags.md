# NAT 类型、行为特征与探测 Flags 说明

本文档用于描述 P2P / UDP 打洞系统中常见的 NAT 类型、NAT 行为、网络拓扑特征和探测 flags。适用于 STUN、STUN、ICE-like、私有 UDP 穿透协议等系统设计。

核心建议：不要只依赖传统 NAT 类型，例如 `FULL_CONE`、`PORT_RESTRICTED`、`SYMMETRIC`。现代穿透系统更应该拆成以下几个维度：

```text
1. 基础 NAT 类型：兼容展示和粗粒度决策
2. Mapping Behavior：内网地址到公网地址的映射行为
3. Filtering Behavior：入站 UDP 包过滤行为
4. Flags：多出口、地址池、端口规律、探测质量等补充信息
5. Confidence / Risk / Strategy Hint：用于后续打洞策略
```

---

## 1. 基础 NAT 类型

### 1.1 OPEN_PUBLIC

#### 含义

`OPEN_PUBLIC` 表示本机直接拥有公网地址，理论上不需要 NAT 转换。

典型特征：

```text
local_ip == srflx_ip
local_port == srflx_port
```

但需要注意：本机有公网地址不代表 UDP 入站一定可达。云安全组、防火墙、IDC ACL 都可能阻断入站 UDP。

#### 典型设备 / 环境

- 云服务器：阿里云 ECS、腾讯云 CVM、AWS EC2、Google Cloud VM
- IDC 物理机：服务器直接配置公网 IPv4
- 家庭宽带公网 IP：路由器拨号获得公网 IPv4，且端口转发到主机
- 企业固定公网出口：某些办公网络中主机直接暴露公网地址

#### 对打洞的影响

如果入站 UDP 可达，成功率最高。  
如果有公网 IP 但防火墙阻断 UDP，表现可能接近 `UDP_BLOCKED`。

#### 建议 flags

```cpp
LOCAL_ADDR_PUBLIC
FILTER_UNKNOWN 或 FILTER_ENDPOINT_INDEPENDENT
```

---

### 1.2 FULL_CONE

#### 含义

`FULL_CONE` 表示内网地址映射到一个稳定公网地址后，任意外部主机都可以向该公网地址发送 UDP 包并被内网主机收到。

行为上通常等价于：

```text
Mapping: Endpoint-Independent Mapping
Filtering: Endpoint-Independent Filtering
```

#### 典型设备 / 环境

- 老式家用路由器的较宽松 NAT 模式
- 部分企业网关的开放 UDP 策略
- 开启 DMZ 主机的家用路由器
- 一些便宜路由器在 UDP 上实现较宽松
- 某些游戏加速器 / P2P 加速网关

#### 对打洞的影响

P2P 打洞成功率高。只要一方是 Full Cone，另一方即使较严格，通常也更容易建立连接。

#### 注意事项

真实网络中完整的 Full Cone 越来越少。很多设备表面上端口稳定，但 filtering 仍然有限制。

---

### 1.3 IP_RESTRICTED

#### 含义

`IP_RESTRICTED`，也称 Restricted Cone。内网主机先向某个外部 IP 发过包后，该外部 IP 才能向映射地址回包。端口不限。

行为上通常对应：

```text
Mapping: Endpoint-Independent Mapping
Filtering: Address-Dependent Filtering
```

例如：

```text
内网主机 -> 8.8.8.8:3478
之后 8.8.8.8:任意端口 -> 公网映射地址 可以进入
其他 IP -> 公网映射地址 被阻断
```

#### 典型设备 / 环境

- 中高端家用路由器
- 小型企业路由器
- MikroTik、OpenWrt、爱快、飞鱼星等策略较常见的 NAT 网关
- 部分校园网出口
- 部分酒店 Wi-Fi 网关

#### 对打洞的影响

比 Full Cone 严格，但仍然适合 UDP 打洞。双方需要先向对方公网地址发 UDP 包，建立过滤状态。

---

### 1.4 PORT_RESTRICTED

#### 含义

`PORT_RESTRICTED`，也称 Port Restricted Cone。内网主机必须先向某个外部 IP:Port 发过包，该 IP:Port 才能向映射地址回包。

行为上通常对应：

```text
Mapping: Endpoint-Independent Mapping
Filtering: Address-and-Port-Dependent Filtering
```

例如：

```text
内网主机 -> 8.8.8.8:3478
只有 8.8.8.8:3478 -> 公网映射地址 可以进入
8.8.8.8:9999 -> 公网映射地址 被阻断
其他 IP:Port -> 公网映射地址 被阻断
```

#### 典型设备 / 环境

- 大量现代家用路由器
- 光猫路由一体设备
- 企业防火墙默认 UDP 策略
- 校园网 / 宿舍网 NAT 出口
- 酒店、商场、机场 Wi-Fi
- 部分 4G/5G CPE 路由器

#### 对打洞的影响

UDP 打洞仍然可行，但双方需要较准确的同步打洞时序。两端同时向对方候选地址发送 UDP 包更重要。

#### 工程建议

- 增加打洞重试轮次
- 同时发送多个候选地址
- 打洞时连续发送短 burst 包
- STUN 协调双方同时发起 punching

---

### 1.5 SYMMETRIC

#### 含义

`SYMMETRIC` 表示同一个内网 UDP socket 访问不同外部目标时，NAT 分配不同公网映射。

典型表现：

```text
local 192.168.1.10:50000 -> stun1:3478 => 1.1.1.1:40001
local 192.168.1.10:50000 -> stun2:3478 => 1.1.1.1:50022
```

或者：

```text
local 192.168.1.10:50000 -> stun1:3478 => 1.1.1.1:40001
local 192.168.1.10:50000 -> stun2:3478 => 2.2.2.2:50022
```

#### 典型设备 / 环境

- 运营商 CGNAT
- 企业级防火墙：Fortinet、Palo Alto、Cisco ASA、H3C、华为 USG 等
- 校园网大规模 NAT
- 数据中心 SNAT 网关
- 多出口策略路由网关
- 某些 4G/5G 蜂窝网络
- 云厂商 NAT Gateway

#### 对打洞的影响

UDP 打洞难度最高。普通 STUN 结果不能直接作为对任意 peer 的可达地址。

#### 工程建议

- 优先尝试 relay fallback
- 如果端口分配有规律，可尝试端口预测
- 需要更高频的打洞包和更精准的时序协调
- 双 symmetric NAT 通常需要中继

---

### 1.6 UDP_BLOCKED

#### 含义

`UDP_BLOCKED` 表示 UDP 探测基本失败，无法从 STUN 服务器获得有效响应。

注意：`probe1` 失败不一定绝对说明 UDP 被阻断，也可能是 STUN 服务不可达、DNS 错误、网络丢包或本地防火墙问题。

#### 典型设备 / 环境

- 企业内网禁止 UDP 出站
- 银行、政企、工业控制网络
- 酒店 / 机场 Wi-Fi 限制 UDP
- 某些代理网络或 VPN 环境
- 防火墙只允许 TCP/443
- 云安全组禁止 UDP

#### 对打洞的影响

无法直接 UDP 打洞。需要 TCP、QUIC over allowed path、WebSocket、TURN/Relay 或其他中继方案。

---

### 1.7 UNKNOWN

#### 含义

`UNKNOWN` 表示探测证据不足，不能可靠归类。

常见原因：

```text
只有一个 STUN 服务器
第二个 STUN 失败
filter probe 没执行
采样次数不足
网络切换
结果冲突
```

#### 工程建议

不要把 UNKNOWN 强行归类成 `PORT_RESTRICTED` 或 `SYMMETRIC`。可以在策略层保守处理，但探测结果层应保持诚实。

---

## 2. Mapping Behavior

Mapping Behavior 描述“内网 socket 访问外部目标时，NAT 如何分配公网地址”。

---

### 2.1 ENDPOINT_INDEPENDENT_MAPPING

#### 含义

同一个内网 socket 无论访问哪个外部目标，都得到相同公网映射。

```text
192.168.1.10:50000 -> stun1 => 1.1.1.1:40000
192.168.1.10:50000 -> stun2 => 1.1.1.1:40000
```

#### 典型设备 / 环境

- 多数家用路由器
- OpenWrt 默认 MASQUERADE 场景中的部分配置
- 小型办公室 NAT
- 一些路由器开启 NAT endpoint-independent 行为

#### 对打洞的影响

这是最适合 UDP 打洞的 mapping 行为。

---

### 2.2 ADDRESS_DEPENDENT_MAPPING

#### 含义

NAT 映射与目标 IP 有关。访问不同外部 IP 时可能得到不同公网映射。

```text
local -> 8.8.8.8:3478 => 1.1.1.1:40001
local -> 9.9.9.9:3478 => 1.1.1.1:40002
```

#### 典型设备 / 环境

- 企业防火墙
- 云 NAT Gateway
- 校园网出口 NAT
- 多运营商出口网关
- 部分 CGNAT

#### 对打洞的影响

STUN 上看到的公网地址不一定适用于连接 peer。需要更复杂的候选地址交换和端口预测。

---

### 2.3 ADDRESS_AND_PORT_DEPENDENT_MAPPING

#### 含义

NAT 映射同时依赖目标 IP 和目标端口。

```text
local -> 8.8.8.8:3478 => 1.1.1.1:40001
local -> 8.8.8.8:3479 => 1.1.1.1:40002
local -> 9.9.9.9:3478 => 1.1.1.1:50001
```

#### 典型设备 / 环境

- 严格企业防火墙
- 高安全等级办公网络
- 运营商 CGNAT
- 部分移动网络 NAT
- 部分安全网关 / UTM 设备

#### 对打洞的影响

接近传统 `SYMMETRIC` NAT。直接 UDP 打洞成功率较低。

---

### 2.4 UNSTABLE_MAPPING

#### 含义

同一个 socket 在短时间内多次探测，公网映射不稳定或不可预测。

```text
round 1 => 1.1.1.1:40001
round 2 => 1.1.1.1:55120
round 3 => 2.2.2.2:33009
```

#### 典型设备 / 环境

- CGNAT 地址池轮换
- 多出口负载均衡网关
- VPN 切换网络路径
- 移动网络弱信号切换基站
- SD-WAN 动态选路
- NAT 表压力过大导致映射快速回收

#### 对打洞的影响

非常不利于打洞。应提高 relay 优先级。

---

## 3. Filtering Behavior

Filtering Behavior 描述“外部主机向 NAT 映射地址发 UDP 包时，NAT 是否允许进入”。

---

### 3.1 ENDPOINT_INDEPENDENT_FILTERING

#### 含义

任意外部 IP:Port 都可以向已经建立的映射地址发送 UDP 包。

对应传统 `FULL_CONE` filtering。

#### 典型设备 / 环境

- Full Cone NAT
- 开启 DMZ 的家用路由器
- 防火墙规则较宽松的企业网关
- P2P 友好的游戏路由器

#### 对打洞的影响

打洞成功率高。

---

### 3.2 ADDRESS_DEPENDENT_FILTERING

#### 含义

只有内网主机主动联系过的外部 IP 可以回包，端口不限。

对应传统 `IP_RESTRICTED` filtering。

#### 典型设备 / 环境

- 中等严格的家用路由器
- 小企业路由器
- 部分校园网出口
- 部分 OpenWrt / RouterOS 配置

#### 对打洞的影响

需要双方先互相发包，建立过滤状态。

---

### 3.3 ADDRESS_AND_PORT_DEPENDENT_FILTERING

#### 含义

只有内网主机主动联系过的外部 IP:Port 可以回包。

对应传统 `PORT_RESTRICTED` filtering。

#### 典型设备 / 环境

- 现代家用路由器常见配置
- 企业防火墙默认 UDP 策略
- 酒店 Wi-Fi / 商场 Wi-Fi
- 运营商 CPE
- 校园网 NAT

#### 对打洞的影响

需要精确同步打洞。STUN 应协调双方同时向对方候选地址发包。

---

### 3.4 FILTER_BLOCKED

#### 含义

外部 UDP 包基本无法进入，即使内网曾经发过包，也可能被策略阻断。

#### 典型设备 / 环境

- 企业防火墙只允许 DNS/特定 UDP
- 严格校园网
- 酒店网络
- 公共 Wi-Fi 隔离网络
- 云安全组禁止入站 UDP

#### 对打洞的影响

UDP 打洞成功率极低。需要 relay。

---

## 4. 常见 NAT / 网络 Flags

### 4.1 LOCAL_ADDR_PUBLIC

#### 含义

本地接口地址是公网地址，或者 STUN 观察到的地址与本地地址一致。

#### 例子

```text
local: 203.0.113.10:50000
stun : 203.0.113.10:50000
```

#### 典型设备 / 环境

- 云服务器
- IDC 裸金属服务器
- 企业公网主机
- 家庭宽带桥接模式下 PC 直接拨号

#### 注意事项

有公网地址不代表可入站。仍然要结合 filter probe。

---

### 4.2 PROBE_DEGRADED

#### 含义

探测结果不完整，置信度下降。

#### 触发场景

```text
stun2 缺失
stun2 失败
filter probe 未执行
采样次数不足
部分轮次超时
control_fd 不可用
```

#### 典型设备 / 环境

这不是设备类型，而是探测质量标记。任何网络都可能出现。

#### 策略影响

- 不要输出过于确定的 NAT 类型
- 降低 confidence
- 提高打洞重试次数
- 更早准备 relay fallback

---

### 4.3 MAPPING_UNSTABLE

#### 含义

公网映射地址或端口不稳定。

#### 例子

```text
round 1: 1.1.1.1:40001
round 2: 1.1.1.1:45088
round 3: 1.1.1.1:32019
```

或者：

```text
round 1: 1.1.1.1:40001
round 2: 2.2.2.2:45088
```

#### 典型设备 / 环境

- CGNAT
- 多出口网关
- SD-WAN
- VPN/代理切换路径
- 移动网络基站切换
- NAT 表压力较大的企业网关

#### 对打洞的影响

非常不利。候选地址可能很快失效。

---

### 4.4 MULTI_EXTERNAL_IP

#### 含义

探测中观察到了多个公网 IP。

这是“事实标记”，表示结果里确实出现了多个 external IP。

#### 例子

```text
stun1 => 1.1.1.10:40001
stun2 => 2.2.2.20:50002
```

#### 典型设备 / 环境

- 多运营商出口网关
- 企业双线出口
- 运营商 CGNAT 地址池
- 云 NAT Gateway 地址池
- SD-WAN 动态出口
- VPN 分流环境

#### 对打洞的影响

不能只注册一个 srflx 地址。建议保存多个候选地址。

---

### 4.5 MULTI_HOMED_NAT

#### 含义

推断 NAT 网关具有多个上游出口，且访问不同目标时可能稳定选择不同出口。

这是“拓扑推断标记”，不是单纯观测事实。

#### 例子

```text
连续多轮：
stun1 总是 => 电信出口 1.1.1.10
stun2 总是 => 联通出口 2.2.2.20
```

#### 典型设备 / 环境

- 企业双 WAN 路由器
- 爱快 / RouterOS / OpenWrt 多线负载均衡
- SD-WAN 网关
- 校园网多运营商出口
- IDC 多线 BGP 或策略路由出口
- 公司总部防火墙连接多条 ISP

#### 对打洞的影响

目标相关的出口选择会导致 STUN 地址不一定适用于 peer 通信。打洞策略应更保守。

---

### 4.6 EXTERNAL_IP_POOL

#### 含义

NAT 使用公网地址池，external IP 可能在多个地址之间轮换。

#### 例子

```text
round 1 => 1.1.1.10
round 2 => 1.1.1.11
round 3 => 1.1.1.12
```

#### 典型设备 / 环境

- 运营商 CGNAT
- 云 NAT Gateway 地址池
- 企业大规模出口 NAT 池
- 数据中心 SNAT 池
- 大型校园网出口

#### 与 MULTI_HOMED_NAT 区别

```text
MULTI_HOMED_NAT 更像多条出口链路 / 多运营商
EXTERNAL_IP_POOL 更像同一出口 NAT 设备有多个公网地址可分配
```

实际网络中两者可能同时存在。

---

### 4.7 CGNAT_SUSPECTED

#### 含义

怀疑用户位于运营商级 NAT 后面。

常见依据：

```text
WAN 地址属于 100.64.0.0/10
WAN 地址是私网地址
STUN 公网地址与路由器 WAN 地址不一致
多个用户共享同一公网 IP
```

#### 典型设备 / 环境

- 家庭宽带没有公网 IPv4
- 4G/5G 蜂窝网络
- 校园网宽带
- 二级运营商宽带
- 公寓统一宽带

#### 对打洞的影响

CGNAT 下 UDP 打洞不一定失败，但 symmetric、地址池、短生命周期等问题更常见。

---

### 4.8 DOUBLE_NAT

#### 含义

存在两层或更多层 NAT。

#### 例子

```text
PC 192.168.1.100
  -> 家用路由器 192.168.1.1
  -> 光猫 NAT 192.168.100.1
  -> 运营商公网/CGNAT
```

#### 典型设备 / 环境

- 光猫路由模式 + 自己再接一个路由器
- 公寓网络二级路由
- 公司内网再挂 Wi-Fi 路由器
- 手机热点后再接 CPE
- 虚拟机 NAT 后再经过宿主机 NAT

#### 对打洞的影响

增加不确定性。尤其当上级 NAT 是 CGNAT 或 symmetric NAT 时，成功率明显下降。

---

### 4.9 HAIRPIN_SUPPORTED

#### 含义

NAT 支持 hairpin，也叫 NAT loopback。内网主机可以访问同一 NAT 后面另一个主机的公网映射地址。

#### 例子

```text
A: 192.168.1.10
B: 192.168.1.11
公网映射: 1.1.1.1:40000 -> B:50000

A 访问 1.1.1.1:40000 可以到达 B
```

#### 典型设备 / 环境

- OpenWrt 可配置支持
- MikroTik RouterOS 支持 hairpin NAT
- 一些华硕、网件、TP-Link 路由器支持 NAT loopback
- 企业防火墙配置后支持

#### 对打洞的影响

同一 NAT 后面的 peer 可以通过公网候选地址互通，简化局域网内发现失败时的连接路径。

---

### 4.10 HAIRPIN_BROKEN

#### 含义

NAT 不支持 hairpin，或者支持不完整。

#### 典型设备 / 环境

- 很多运营商光猫
- 低端家用路由器
- 某些企业防火墙默认禁止内到内回流
- 公共 Wi-Fi 隔离环境

#### 对打洞的影响

同一 NAT 后面的 peer 如果只交换公网地址，可能连接失败。应优先尝试 LAN candidate。

---

### 4.11 PORT_PRESERVING

#### 含义

NAT 尽量保持内网源端口不变。

```text
192.168.1.10:50000 -> 1.1.1.1:50000
```

#### 典型设备 / 环境

- 家用路由器常见
- OpenWrt / Linux conntrack NAT 在端口未冲突时常见
- 小型办公路由器
- 某些云 NAT 配置

#### 对打洞的影响

有利。peer 可以更容易预测端口。

---

### 4.12 PORT_SEQUENTIAL

#### 含义

NAT 分配端口呈递增或递减规律。

```text
40001
40002
40003
40004
```

#### 典型设备 / 环境

- 部分家用路由器
- Linux iptables/nftables NAT 某些场景
- 小型企业路由器
- 一些 CGNAT 设备

#### 对打洞的影响

对 symmetric NAT 端口预测有帮助，但存在并发干扰风险。

---

### 4.13 PORT_RANDOMIZED

#### 含义

NAT 端口分配随机或高熵，不容易预测。

```text
42351
17893
51244
29001
```

#### 典型设备 / 环境

- 安全性较高的企业防火墙
- 运营商 CGNAT
- 云 NAT Gateway
- 移动网络 NAT
- 开启端口随机化策略的路由器

#### 对打洞的影响

端口预测几乎不可用。应提高 relay 优先级。

---

### 4.14 PORT_DELTA_STABLE

#### 含义

公网端口与本地端口之间存在稳定偏移。

```text
local 50000 -> external 60000
local 50001 -> external 60001
```

#### 典型设备 / 环境

- 某些嵌入式路由器
- 一些 NAT 池实现
- 某些运营商网关
- 特定 Linux NAT 配置

#### 对打洞的影响

可以辅助端口预测，但要谨慎使用，需要多轮采样确认。

---

### 4.15 SHORT_MAPPING_LIFETIME

#### 含义

UDP NAT 映射生命周期很短。

例如：

```text
无流量 15 秒后映射失效
```

#### 典型设备 / 环境

- 运营商 CGNAT
- 移动网络 NAT
- 负载较高的企业防火墙
- 酒店 / 公共 Wi-Fi
- 资源紧张的低端路由器

#### 对打洞的影响

需要更频繁 keepalive。打洞窗口更短。

#### 策略建议

```text
keepalive interval < mapping lifetime / 2
```

---

### 4.16 LONG_MAPPING_LIFETIME

#### 含义

UDP 映射生命周期较长。

例如：

```text
无流量 2-5 分钟仍保持映射
```

#### 典型设备 / 环境

- 家用路由器
- 小企业网关
- 配置较宽松的防火墙
- 专用 P2P 友好网关

#### 对打洞的影响

有利于连接保持，可以降低 keepalive 频率。

---

### 4.17 IPV6_NATIVE

#### 含义

主机拥有原生 IPv6 地址，并且可以直接通过 IPv6 通信。

#### 典型设备 / 环境

- 中国移动 / 中国电信 / 中国联通部分宽带
- 日本、欧洲、美国部分家庭宽带
- 云服务器 IPv6
- 校园网 IPv6
- 手机 4G/5G IPv6 网络

#### 对打洞的影响

IPv6 下通常没有传统 IPv4 NAT，但仍可能有防火墙。P2P 系统应优先尝试 IPv6 direct candidate。

---

### 4.18 IPV6_ONLY

#### 含义

网络主要或完全基于 IPv6，IPv4 通过 NAT64、464XLAT 或代理机制访问。

#### 典型设备 / 环境

- 移动网络
- 某些校园网
- 某些云原生网络
- IPv6-only 数据中心环境

#### 对打洞的影响

IPv4 STUN 可能不可用。协议必须支持 IPv6 STUN / IPv6 candidate。

---

### 4.19 NAT64

#### 含义

IPv6-only 客户端通过 NAT64 访问 IPv4 服务。

#### 典型设备 / 环境

- 移动运营商网络
- IPv6-only 企业网络
- 某些校园网
- 云 VPC IPv6-only 环境

#### 对打洞的影响

IPv4 peer-to-peer 连接路径可能不可用或不对称。应优先 IPv6 或 relay。

---

### 4.20 DS_LITE

#### 含义

DS-Lite 是运营商用 IPv6 承载 IPv4 的技术，用户侧没有真正公网 IPv4，IPv4 流量经过运营商 AFTR 设备 NAT。

#### 典型设备 / 环境

- 日本、欧洲部分家庭宽带
- 运营商 IPv6 转型网络
- 宽带光猫显示 IPv6，IPv4 经过隧道

#### 对打洞的影响

IPv4 UDP 打洞通常受 CGNAT 影响较大。IPv6 直连通常更值得尝试。

---

## 5. 推荐字段设计

建议不要把所有信息塞进一个 `nat_type`。可以设计为：

```cpp
struct NatInfo {
    NatType compat_type;

    MappingBehavior mapping_behavior;
    FilteringBehavior filtering_behavior;

    uint64_t flags;

    std::vector<SockAddr> observed_srflx_addrs;
    std::vector<IpAddr> observed_external_ips;

    uint32_t probe_success_count;
    uint32_t probe_failure_count;

    bool filter_probe_executed;
    bool same_ip_diff_port_rx;
    bool diff_ip_rx;

    NatConfidence confidence;
    NatRisk risk;
    StrategyHint strategy_hint;

    uint64_t probe_timestamp_ms;
    uint32_t nat_info_version;
};
```

---

## 6. 推荐 enum 示例

```cpp
enum class NatType : uint8_t {
    Unknown,
    OpenPublic,
    FullCone,
    IpRestricted,
    PortRestricted,
    Symmetric,
    UdpBlocked,
};

enum class MappingBehavior : uint8_t {
    Unknown,
    EndpointIndependent,
    AddressDependent,
    AddressAndPortDependent,
    Unstable,
};

enum class FilteringBehavior : uint8_t {
    Unknown,
    EndpointIndependent,
    AddressDependent,
    AddressAndPortDependent,
    Blocked,
};

enum NatFlags : uint64_t {
    NAT_FLAG_LOCAL_ADDR_PUBLIC        = 1ull << 0,
    NAT_FLAG_UDP_BLOCKED              = 1ull << 1,
    NAT_FLAG_PROBE_DEGRADED           = 1ull << 2,
    NAT_FLAG_MAPPING_UNSTABLE         = 1ull << 3,
    NAT_FLAG_MULTI_EXTERNAL_IP        = 1ull << 4,
    NAT_FLAG_MULTI_HOMED_NAT          = 1ull << 5,
    NAT_FLAG_EXTERNAL_IP_POOL         = 1ull << 6,
    NAT_FLAG_CGNAT_SUSPECTED          = 1ull << 7,
    NAT_FLAG_DOUBLE_NAT               = 1ull << 8,
    NAT_FLAG_HAIRPIN_SUPPORTED        = 1ull << 9,
    NAT_FLAG_HAIRPIN_BROKEN           = 1ull << 10,
    NAT_FLAG_PORT_PRESERVING          = 1ull << 11,
    NAT_FLAG_PORT_SEQUENTIAL          = 1ull << 12,
    NAT_FLAG_PORT_RANDOMIZED          = 1ull << 13,
    NAT_FLAG_PORT_DELTA_STABLE        = 1ull << 14,
    NAT_FLAG_SHORT_MAPPING_LIFETIME   = 1ull << 15,
    NAT_FLAG_LONG_MAPPING_LIFETIME    = 1ull << 16,
    NAT_FLAG_IPV6_NATIVE              = 1ull << 17,
    NAT_FLAG_IPV6_ONLY                = 1ull << 18,
    NAT_FLAG_NAT64                    = 1ull << 19,
    NAT_FLAG_DS_LITE                  = 1ull << 20,
};
```

---

## 7. 策略建议

### 7.1 打洞优先级

```text
IPv6 direct
  > OpenPublic / FullCone
  > IP_RESTRICTED
  > PORT_RESTRICTED
  > Symmetric with predictable port
  > Symmetric with random port
  > Relay
```

### 7.2 遇到 PROBE_DEGRADED

```text
1. 降低 confidence
2. 不要强行给确定 NAT 类型
3. 策略层按更保守类型处理
4. 后续允许重新探测
```

### 7.3 遇到 MULTI_EXTERNAL_IP

```text
1. 注册多个 srflx candidates
2. 增加 simultaneous punching 目标数量
3. 标记 risk high 或 medium-high
4. 必要时尝试 relay fallback
```

### 7.4 遇到 MAPPING_UNSTABLE

```text
1. 不要依赖单个 STUN 结果
2. 增加采样轮次
3. 尽快发起打洞，缩短地址使用窗口
4. 优先准备 relay
```

### 7.5 遇到 PORT_RANDOMIZED

```text
1. 不做端口预测，或仅做有限范围尝试
2. 避免大量端口扫描式 punching
3. 提高 relay 优先级
```

---

## 8. 总结

对于一个可靠的 P2P 穿透系统，推荐输出不是单一 NAT 类型，而是：

```text
NatType compat_type
MappingBehavior mapping_behavior
FilteringBehavior filtering_behavior
NatFlags flags
NatConfidence confidence
NatRisk risk
StrategyHint strategy_hint
```

其中：

```text
NatType 用于兼容和展示
MappingBehavior 决定公网映射是否可复用
FilteringBehavior 决定入站 UDP 是否可进入
Flags 描述特殊网络环境和探测质量
StrategyHint 指导后续打洞、重试和 relay fallback
```

这样才能避免把 `stun2 失败`、`filter probe 缺失`、`多出口地址变化` 等复杂情况错误归类成传统 NAT 类型，从而提升 P2P 连接成功率。
