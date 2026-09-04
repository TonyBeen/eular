# libutp NTRS 服务拆分与 Context 接入需求

- 日期:2026-08-18
- 状态:已确认，作为 NTRS 实现的前置需求
- 优先级:本文件在 NAT 服务、Node/Hub TCP 控制链路、地址族、网卡绑定和服务部署范围内有效；
  Rendezvous 半连接协议以 2026-08-22 规格为准。

---

## 1. 目标与边界

NTRS 是由 NAT 服务和打洞服务组成的系统，不是单一服务。两类服务职责、部署地址和状态均须隔离：

```text
同一个 Context UDP socket
  |
  +-- 用户调用 NAT 探测 --> NAT 服务集群（多 IP/端口、服务间协同）
  |
  +-- 用户调用节点注册 --> 打洞服务（半连接、节点表、PING/PONG 保活、RENDEZVOUS 转发）
```

- NAT 服务负责观察 UDP 映射与过滤行为，并向 Context 返回探测结果；它不保存打洞服务的节点注册状态。
- 打洞服务负责维护下挂节点、节点 NAT 记录、节点租约和 RENDEZVOUS 转发；它不与其他打洞服务联邦。
- NAT 服务集群内部允许服务间通信，用于协调多个独立公网 IP/端口的探测；打洞服务不依赖这种协同。
- Context 发往 NAT 服务和打洞服务的报文必须使用同一个已 bind UDP socket。NAT 探测结果才可代表该 Context 后续 P2P 发送的映射行为。
- NAT 服务实际使用的探测 IP 与打洞服务实际使用的 IP 必须不同。两个操作传入的服务 IP 地址
  相同时，相关操作必须失败而非降级为同地址探测。
- Context、Connection 和 Stream 的所有公开接口均非线程安全，只能在其所属 Context 的事件循环
  线程调用；所有 NTRS 完成回调也在该线程同步执行。库不提供跨线程调用、锁或隐式任务串行化。

本文件固定 Context 所需的 UTP 承载 NAT 探测基线、阶段和分类规则；字段编号、编解码模块
接口及服务端内部协同 RPC 在实现 NAT 服务前单独冻结。RENDEZVOUS/FrameRendezvous 线格式以
`2026-08-22-libutp-ntrs-rendezvous-half-association.md` 为准；NTRS 服务端认证和 peer 身份认证仍由后续专项需求确定。

---

## 2. 服务地址与解析

Context 不保存 NAT 或打洞服务地址。NAT 服务地址由每次 `utp_context_probe_nat()` 的
`utp_nat_probe_options_t` 传入；打洞服务地址由后续注册接口的 options 传入。这样同一 Context
可以在不同操作中切换服务 IP 和端口。

- libutp 不内置 DNS 或任何名称解析功能。调用方负责解析、选择具体服务 IP，并传入 IP 字面量
  与端口；主机名、`[IPv6]` 方括号表示和任何 DNS 名称均为非法输入。
- 显式操作开始时，库同步解析地址并复制为任务持有的内部二进制 endpoint；任务开始后不再借用
  调用方字符串。地址为空、端口为 `0` 或只设置其中一项均为非法参数。
- 同一次 NAT 探测内主 NAT endpoint 固定；用户取消或任务完成后才可用下一次调用切换 endpoint。
- NAT 服务下属探测节点的选择与服务间协同由 `ntrs/` 内部协议定义，不在 Context 配置层展开。
- 服务地址与 Context bind 的地址族不匹配时，仅使对应显式操作失败；普通
  `utp_context_connect()` 不受影响。

---

## 3. 用户驱动的 NAT 探测

Context 提供异步接口：

```c
typedef struct utp_nat_probe_options {
    const char* nat_service_address;  // NAT 服务 IPv4/IPv6 字面 IP，仅在调用期间借用
    uint16_t    nat_service_port;     // NAT 服务 UDP 端口
    uint32_t    phase_timeout_ms;     // 每个探测阶段的总时限；0 使用默认 3000 ms
} utp_nat_probe_options_t;

#define UTP_NAT_PROBE_OPTIONS_INIT {0u}

utp_status_t utp_context_probe_nat(utp_context_t* context,
                                   const utp_nat_probe_options_t* options,
                                   utp_on_nat_probe_fn callback,
                                   void* user_data);

utp_status_t utp_context_cancel_nat_probe(utp_context_t* context);
```

调用前 Context 必须已成功 `bind()`。`options`、`nat_service_address` 和 `nat_service_port` 均为
必填；地址在调用期间借用，库成功启动任务后仅保存已解析的二进制 endpoint。`phase_timeout_ms`
是每个阶段的总时限，默认 `3000 ms`，不得小于 `6 ms`。用户决定是否、何时以及多频繁探测；
Context 不在 bind、connect、注册时自动启动探测。

探测任务状态与 NAT 结果缓存分离。任务状态固定为：

```text
IDLE -> PROBING -> IDLE
```

一次正常完成的任务会留下有效 NAT 记录，分类可以是 `UNKNOWN`；永久本地错误
不会创建记录。记录本身只在到达 `expires_at` 时失效，任务回到 `IDLE` 不表示记录被清除。

- `PROBING` 期间再次调用返回 `UTP_STATUS_IN_PROGRESS`，已有任务继续执行。
- `utp_context_cancel_nat_probe()` 仅在 `PROBING` 期间成功；没有活动任务时返回
  `UTP_STATUS_NOT_FOUND`。它不发送取消数据报，而是同步取消本地定时器和待发送项、释放任务并
  返回 `IDLE`；主动取消绝不调用原始 `utp_on_nat_probe_fn`。调用返回后可立刻发起新的探测。
- 调用成功只表示任务已启动；最终状态通过一次 `utp_on_nat_probe_fn` 回调返回。
- NAT 分类已完成时，即使分类为 `UNKNOWN`，回调也必须返回 `UTP_STATUS_OK`
  与结果视图。只有随机数、内存或永久 socket 错误等操作失败时才返回错误，且结果视图
  为空。结果视图仅在回调期间有效。
- NAT 探测结果至少包含地址族、探测侧观测的公网映射 endpoint、NAT 类型、端口样本、探测时间和绝对失效时间。
- 探测侧观测的公网 endpoint 只用于诊断、NAT 分类和端口样本，不作为打洞服务最终保存的公网 endpoint。
- 当前探测以 Context 已 bind socket 的地址族为准：绑定 IPv4 只探测 IPv4，绑定 IPv6 只探测
  IPv6。libutp 不支持双栈 bind，所有 IPv6 socket 均必须设置 `IPV6_V6ONLY=1` 并拒绝 IPv4
  数据报；调用方需要双栈时创建两个独立 Context。两个 Context 的 NAT 记录彼此独立。
- 示例客户端 `ntrsc -6` 强制从 NAT 服务域名选择 AAAA 记录并绑定 `::`；默认仅选择 A 记录并绑定
  `0.0.0.0`。NTRS Node 的 IPv6 UDP 探测使用独立 IPv6-only socket，Hub、Node 的控制连接、
  `probe`、`change_port`、`control` endpoint 必须同族。
- NAT 记录过期后不得继续用于打洞方向早失败、候选优先级或端口预测；使用过期记录时等价于 `UNKNOWN`。

NAT 服务内部可使用协调器向多个探测节点下发计划，Context 只参与 UTP 承载的 NAT 探测并接收结果。Context 不向打洞服务提交探测原始观测，也不伪造 NAT 分类过程。

### 3.1 UTP 承载的 NAT 探测基线

详细探测流程以同级 `ntrs/` 工程的 `binary_protocol.h`、`probe_types.h`、
`ntrs_client.cpp` 及 `doc/NTRS_私有探测协议说明.md` 为分类与服务端协同参考。C 版不复用
旧私有 UDP 线格式，改用本节的 UTP 承载格式；也不复用其 KCP 控制面、`bootstrap_token`、
`session_token` 或跨节点 `probe_auth` HMAC。

每个探测报文都必须带 UTP 固定包头，使用新包类型 `UTP_PACKET_TYPE_NAT_PROBE = 0x07`。它是
无连接控制包，头部必须满足：

```text
scid = 0
dcid = 0
packet_number = Context NAT 探测序号
type = UTP_PACKET_TYPE_NAT_PROBE
reserve = 0
```

`packet_number` 在单个 Context 的 NAT 探测命名空间内从 `1` 单调递增，跨任务不重置；每个
实际发往内核的数据报消耗一个新序号。服务端响应必须精确回显请求的 `packet_number`，不生成
自己的 NAT 探测序号。该包号不属于任何 Connection，也不参与普通 ACK、拥塞控制或重传队列。

所有 NAT 探测请求的 UTP 整包长度必须通过 `PADDING` TLV 填充到固定的
`UTP_NAT_PROBE_MIN_PACKET_SIZE = 128` 字节。NAT 服务对任一请求的响应整包长度不得超过该请求的
整包长度；首期所有合法响应因此也不得超过 128 字节。服务端不得为了填充响应而额外发送数据。
该规则将伪造源地址请求造成的 UDP 反射放大限制为不大于 `1`；服务端仍须按源 IP 做请求速率
限制，保护自身解析和协同资源。

UTP payload 是无 magic 的私有 NTRS 探测帧：

```text
probe_header = version:u8 | message_type:u8 | step:u8 | change_flags:u8
TLV          = type:u16 | length:u16 | value
```

- `version` 固定为 `2`；没有 magic，UTP `type` 已完成第一层解复用。单个 NAT 探测包最大
  `2048` 字节，且不得超过 Context 的当前发送 MTU。
- `message_type` 固定为：`BINDING_REQUEST = 1`、`BINDING_RESPONSE = 2`。
- `step` 固定为：`PRIMARY_BINDING = 1`、`ALTERNATE_BINDING = 2`。
- `change_flags` 是响应源切换要求及响应来源标识：`NONE = 0`、`CHANGE_PORT = 1`、
  `CHANGE_IP = 2`、`CHANGE_IP | CHANGE_PORT = 3`。协议不单独发送只带 `CHANGE_IP` 的请求或响应。
- `PROBE_TOKEN` 为 12 字节随机值；每个实际发送的数据报生成新 token，响应必须原样回显。
- `PADDING` TLV 只允许出现在请求，value 必须全为零；Context 使用它精确填充到 128 字节，NAT
  服务解析后忽略其内容。首期 TLV 编号固定为：`PROBE_TOKEN = 1`、`MAPPED_ADDR = 6`、
  `ORIGIN_ADDR = 7`、`ALTERNATE_PROBE_ENDPOINT = 8`、`PADDING = 12`；未列出的编号保留。
- `request_id`、私有 `sequence` 与 `timestamp_ms` 从线格式删除。Context 按 UTP
  `packet_number` 直接定位当前步骤的发送记录，再校验 `message_type`、`step`、`change_flags`、
  `PROBE_TOKEN` 及预期源 endpoint。一个请求可产生多个响应，因此发送记录按响应来源维护
  已接收位图，不能在收到第一包响应后整体作废。
- 正常响应必须携带 `MAPPED_ADDR` 和 `ORIGIN_ADDR`。协同 Node 可用时，`PRIMARY_BINDING` 主响应携带
  同地址族的 `ALTERNATE_PROBE_ENDPOINT`；组合响应必须携带该字段，其值为协同 Node 的主 Binding
  endpoint。没有可用协同 Node 时允许主响应不携带该字段。
- `MAPPED_ADDR` 是 NAT 服务观察到的 Context 公网映射，仅用于该次 NAT 结果，不能覆盖打洞
  服务从常驻控制连接观察到的公网 endpoint。
- `ALTERNATE_PROBE_ENDPOINT` 是 NAT 服务集群的协同探测端点，包含 IP 与端口；它不是客户端
  映射、对端候选或打洞服务 endpoint。它必须与主 NAT 探测 endpoint 使用不同公网 IP，并同样受
  NAT 服务与打洞服务 IP 隔离约束；它用于校验组合响应来源并作为 `ALTERNATE_BINDING` 的目标。

`PROBE_TOKEN` 仅用于关联异步请求和过滤陈旧响应，不是服务端身份认证，也不证明 NAT 结果
真实可信。首期不在 NAT 探测私有帧引入旧 `probe_auth` HMAC；打洞服务认证仍由独立认证需求
定义。

多线出口或 CGNAT 下，NAT 服务的 `MAPPED_ADDR` 与打洞服务观察到的控制连接源地址可能不同。
这不是协议错误，也不拒绝注册；打洞服务始终以自己观察到的 `IP:port` 作为节点最终公网
endpoint，Context 不得用 `MAPPED_ADDR` 覆盖它。

旧 `ntrs` 通过已认证的 KCP 控制面 `NAT_PROBE_REQ/RSP` 预先下发两个探测端点。本次服务
拆分不建立这条 NAT 控制连接，且 Context 只配置一个 NAT 服务 IP，因此必须改为由
`PRIMARY_BINDING` 响应携带 `ALTERNATE_PROBE_ENDPOINT`。NAT 服务集群负责确保该端点属于不同公网 IP；
没有这个字段或字段不合法时，客户端只能完成单端点探测并按本节规定降级，不能回退到旧 KCP
控制面请求端点。

### 3.2 IPv4 探测状态机

IPv4 必须在同一 UDP socket 上按以下顺序探测。NAT 服务负责在 `PRIMARY_BINDING` 时协调另一
公网 IP 的 Node 发送组合响应，Context 不与 NAT 服务集群建立额外连接。

```text
PRIMARY_BINDING -> [证据足够则完成，否则] ALTERNATE_BINDING -> 完成
```

1. `PRIMARY_BINDING`：Context 向配置的 NAT 服务主 endpoint 发送一份
   `BINDING_REQUEST(change_flags = CHANGE_IP | CHANGE_PORT)`。同一请求的 `packet_number` 和
   `PROBE_TOKEN` 应产生两种响应，而不是每个 change flag 各产生一种响应：
   - 主响应：主 Node 从原 IP、原端口发送
     `BINDING_RESPONSE(change_flags = NONE)`，提供主 `MAPPED_ADDR`。
   - 组合响应：协同 Node 从不同 IP、不同端口发送
     `BINDING_RESPONSE(change_flags = CHANGE_IP | CHANGE_PORT)`。其 `ORIGIN_ADDR` 必须等于实际
     UDP 源 endpoint，并携带协同 Node 主 Binding endpoint 作为 `ALTERNATE_PROBE_ENDPOINT`。
   两类响应均到达且主映射有效时，IPv4 可立即判定 `FULL_CONE`，不再执行第二步。
2. `ALTERNATE_BINDING`：只有组合响应未到达且已取得合法协同 endpoint 时才执行。Context 向
   `ALTERNATE_PROBE_ENDPOINT` 发送 `BINDING_REQUEST(change_flags = CHANGE_PORT)`；协同 Node 对同一
   请求返回两种响应：主 Binding endpoint 返回 `NONE` 响应，用于取得第二组 `MAPPED_ADDR`；同 IP
   的 change-port endpoint 返回 `CHANGE_PORT` 响应，用于验证地址限制型过滤。若第二组映射已足以
   判定对称映射，或第二组映射与 `CHANGE_PORT` 响应均已收到，应立即完成；缺少 `CHANGE_PORT`
   响应时必须等待该步骤总超时后才能判定端口限制型。

每个阶段固定进行至多三轮发送；每轮向该阶段目标端点并发发送两个 NAT 探测包，故每阶段
最多实际发送六包。阶段总时限为 `T = phase_timeout_ms`，三轮的等待窗口按 `1:2:3` 分配：

```text
t = 0       : 发送第 1 轮两个包；等待 T / 6
t = T / 6   : 发送第 2 轮两个包；等待 2T / 6
t = T / 2   : 发送第 3 轮两个包；等待 3T / 6
t = T       : 阶段结束
```

每轮中的两个包具有不同的 `packet_number` 和 `PROBE_TOKEN`。每个发送记录可分别接受该步骤允许的
两种响应来源；同一来源的重复响应不重复计数。各轮已发送包的匹配记录均保留至步骤结束，第一轮响应即使在后续轮次到达，仍
按其 `packet_number` 计入；只有阶段推进、阶段结束或取消时才统一失效。耗尽六包或到达 `T`
后进入下一阶段并保留失败证据，不能因单包超时直接终止或判定 NAT 类型。

没有合法 `ALTERNATE_PROBE_ENDPOINT` 时不得伪造 `ALTERNATE_BINDING`；已得到的主映射结果仍可
完成回调，但 IPv4 分类必须降级为 `UNKNOWN`。若本地地址与稳定映射完全一致，可确认没有 NAT，
但尚未取得过滤证据，结果为 `OPEN_PUBLIC_WITH_FIREWALL`；只有组合响应或辅助阶段的
`CHANGE_PORT` 响应到达后，才可判定为 `OPEN_PUBLIC`。过滤探测不可用只降低分类精度，不能覆盖
有效的映射观测。

### 3.3 匹配、失败和分类

- `NONE` 响应的 UDP 源地址必须等于该步骤的目标 endpoint。`CHANGE_PORT` 响应必须来自目标的同
  IP 异端口；组合响应必须来自与主 endpoint 不同 IP，且为所携带协同主 endpoint 的同 IP 异端口。
  所有响应的 UDP 源地址必须等于 `ORIGIN_ADDR`。仅有正确 token 而源地址错误的包必须丢弃。
- 发送返回临时错误时保留该次发送预算并按定时器重试；永久本地发送错误、明确 ICMP
  不可达、纯超时必须分别记录为 `LOCAL_SEND_FAILED`、`ICMP_UNREACHABLE`、`NO_RESPONSE`
  诊断，不能混为一个超时错误。
- 主端点在完整预算内没有任何合法响应时，结果为 `UNKNOWN`；无响应无法区分 UDP 被阻断、临时丢包、
  路由异常或服务端故障。本地永久发送错误导致未能
  完成探测时回调失败，不创建可用于注册的 NAT 记录。辅助端点失败只使类型降级为
  `UNKNOWN`，不能把主端点已经确认的可达性改写为不可达。
- 主、辅映射一致且各步骤内只有一个映射时，映射行为为 endpoint-independent；不同目标映射
  不同则为对端相关映射；同一步骤出现多个映射，或两个公网 IP 不同，归类
  `SYMMETRIC_MULTI_LINE`。
- 映射稳定且不等于本机实际本地地址时，组合响应成功为 `FULL_CONE`；组合响应失败后，主、辅映射
  一致且 `CHANGE_PORT` 成功为 `IP_RESTRICTED`，未收到 `CHANGE_PORT` 为 `PORT_RESTRICTED`。映射对端相关或不稳定
  为 `SYMMETRIC`。本地地址与映射完全一致且映射稳定时，组合响应或 `CHANGE_PORT` 成功为
  `OPEN_PUBLIC`，否则为 `OPEN_PUBLIC_WITH_FIREWALL`。显式绑定具体 IP 时使用该 bind 地址；绑定
  `0.0.0.0` 或 `::` 时，使用首个合法主响应 pktinfo 提供的实际本地目的地址。

IPv6 不得套用 IPv4 NAT44 分类。首期仅记录该地址族 UDP 是否可达，以及可选的组合切换 IP 与端口
响应证据；对外可将可达结果表示为 `OPEN_PUBLIC`、受过滤结果表示为
`OPEN_PUBLIC_WITH_FIREWALL`，其余情况均表示为 `UNKNOWN`。
NAT66/NPTv6 的映射分类以后单独定义。

### 3.4 Context 侧详细状态机

一个 Context 同一时刻只有一个 NAT 探测任务。任务绑定创建它时的 UDP socket 和地址族；在
任务结束前不得因后续 bind、注册或普通 UTP 连接而切换 socket 或地址族。
Context 只绑定一个地址族并只探测该地址族；不支持一个 Context 的双栈 socket。调用方需要
IPv4、IPv6 时必须创建两个 Context，各自拥有以下独立任务和 NAT 记录，不能混用响应或缓存。

```text
                  +-----------------------------+
                  |             IDLE            |
                  +-----------------------------+
                               |
                  probe_nat()  |  校验已 bind 的地址族，固定主端点并建立阶段计划
                               v
              +-----------------------+
              | PRIMARY_BINDING_SEND  |
              +-----------------------+
                      |
                      v
              +-----------------------+
              | PRIMARY_BINDING_WAIT  |<-------------+
              +-----------------------+              |
                |       |                             |
         有效响应 |       +--超时且仍有预算----------+
                |                                     |
                +--目标成功数/耗尽预算---------------+
                               |
          有有效 ALTERNATE_PROBE_ENDPOINT ?
                      |                 |
                     是                 否
                      v                 v
      ALTERNATE_BINDING_SEND/WAIT    COMPLETE_RESULT
                      |
                      v
                COMPLETE_RESULT
                      |
                      v
                    IDLE

     任意 PROBING 子状态 -- cancel_nat_probe() --> IDLE
```

`*_SEND/WAIT` 是同一阶段的两个内部子状态，不是公开 API 状态。`SEND` 每轮构造两个不同的
UTP `packet_number` 和随机 token，并在内核接受两个数据报后进入 `WAIT`；每个包各自记录
单调 `sent_at`。`WAIT` 的轮次定时器按阶段总时限的 `1:2:3` 切分推进下一轮，而不是逐包使
已发送记录失效。一个阶段最多同时保留六条按包号索引的等待记录，因此迟到响应不会与后续
发送混淆。

状态转换逐项规定如下：

| 当前状态 | 事件 | 动作 | 下一状态 |
|---|---|---|---|
| `IDLE` | `probe_nat()` | 校验 bind、NAT 服务 endpoint、地址族、选项和回调；固定主端点并清空本次临时观测 | `PRIMARY_BINDING_SEND` |
| 任意 `*_SEND/*_WAIT` | `cancel_nat_probe()` | 取消定时器和待发送项，释放任务但不调用原回调 | `IDLE` |
| 任意 `*_SEND` | 一轮的两个 UDP 包均发送成功 | 记录两条 `packet_number/token/sent_at`，启动本轮窗口定时器 | 对应 `*_WAIT` |
| 任意 `*_SEND` | 临时发送阻塞 | 保持本次发送预算，按 Context 下一次可写调度或短定时器重试；阶段总期限仍受预算窗口限制 | 原状态 |
| 任意 `*_SEND` | 永久本地发送错误或随机数失败 | 记录本地诊断，取消全部定时器 | `COMPLETE_ERROR` |
| 任意 `*_WAIT` | 收到完全匹配的合法响应 | 按 `packet_number` 定位记录并写入该阶段观测；达到阶段成功条件时取消阶段全部等待记录 | 当前阶段的完成判定 |
| 任意 `*_WAIT` | 本轮窗口到期且尚有轮次 | 保留已发送的全部等待记录，增加本阶段已发送计数 | 对应 `*_SEND` |
| 任意 `*_WAIT` | 第三轮窗口到期 | 取消阶段全部等待记录，保留已收集观测和超时诊断 | 当前阶段的完成判定 |
| `COMPLETE_RESULT` | 结果已写入 Context 缓存 | 清理任务，再调用用户回调 | `IDLE` |
| `COMPLETE_ERROR` | 错误已固定 | 清理任务，再调用用户回调 | `IDLE` |

临时发送阻塞不得无限延长任务：任何阶段的总期限均为其配置的 `phase_timeout_ms`。总期限内
一直无法向内核提交一个完整的两包轮次时，任务以本地 I/O 失败结束，而不是伪装成
`UNKNOWN`。永久发送错误映射为现有 socket 写错误；随机数失败映射为
`UTP_STATUS_RANDOM_GENERATION`。

阶段完成判定如下：

1. `PRIMARY_BINDING` 收到首个合法主响应后，锁定其 `MAPPED_ADDR` 作为主映射样本。
   后续响应仍计入成功数、RTT 和不同映射集合。首个合法 `ALTERNATE_PROBE_ENDPOINT` 只能在同地址族、非
   未指定地址、且与主端点具有不同公网 IP 时锁定为辅助端点；后续响应给出不同的
   `ALTERNATE_PROBE_ENDPOINT` 时，保留主映射观测但废弃辅助计划，最终按单端点降级，不能在多个候选间切换。
2. 同一步骤的合法主响应与组合响应均到达后立即完成；IPv4 分类为 `FULL_CONE`，本地地址与映射
   一致时覆盖为 `OPEN_PUBLIC`。组合响应先到时保留证据，但必须等主响应提供主映射后才能完成。
3. `PRIMARY_BINDING` 没有任何合法主响应时，任务仍正常完成，结果为 `UNKNOWN`。这表示当前探测没有取得
   足够证据，不表示 Context socket 或普通 UTP 连接立即失效。
4. 组合响应未到达但存在已锁定辅助端点时执行 `ALTERNATE_BINDING`。辅映射与主映射不同后立即
   按映射差异完成；映射一致且 `CHANGE_PORT` 响应到达后立即完成；只有映射一致但缺少
   `CHANGE_PORT` 响应时等待完整步骤超时。辅助主响应始终未到达时分类为 `UNKNOWN`。
5. 计算最终 NAT 分类、端口样本、探测时间与绝对过期时间后，先原子替换同地址族的 Context
   NAT 缓存，再转换为 `COMPLETE_RESULT`。回调期间用户立刻发起注册时，读取到的必须是新缓存。

### 3.5 收包、回调与销毁时序

Context 从 UDP socket 取到一个数据报后，按以下固定顺序处理：

1. 先有界解码 UTP 固定包头。`type == UTP_PACKET_TYPE_NAT_PROBE` 必须在 Connection、pending
   和普通 Initial 解复用之前优先交给 NAT 模块，不能按零 CID 落入普通握手分支。
2. NAT 模块只接受 `scid == 0`、`dcid == 0`、非零 `packet_number`、`reserve == 0` 且长度严格
   等于 UTP 头部 `payload_length` 的包；不满足任一条件直接丢弃。
3. 对满足头部条件的 NAT 包有界解析 `version/message_type/phase/flags` 和 TLV。没有活动 NAT
   任务、版本错误、格式错误、未知消息类型或未知阶段时直接丢弃，不向普通 UTP 路径回退。
4. 仅当 `packet_number` 命中当前阶段尚未消费的发送记录，且 `message_type`、`phase`、
   `PROBE_TOKEN`、预期源地址与阶段规则均匹配时，才消费并改变 NAT 状态。包号命中后也必须
   完成其余校验，不能只按包号接受响应。
5. 每条发送记录最多接受一次响应；重复响应先前的包号、已超时阶段的迟到响应或取消后的响应
   均静默丢弃。在一次批量收包中，某个响应导致阶段切换后，后续数据报立即按新阶段匹配。

状态提交必须先于用户回调：在调用 `utp_on_nat_probe_fn` 前取消定时器、移除活动任务、更新
缓存并将公开状态设回 `IDLE`。因此回调中再次调用 `utp_context_probe_nat()` 可以启动下一次
探测；回调中调用注册 upsert 可以读取刚完成的 NAT 记录。回调不可重入地触发同一任务的第二次
完成通知。

Context 销毁期间不调用 NAT 探测回调。销毁流程只取消定时器和待发送项，并释放任务内存；
此时 Context 已不再可用，不能向用户交付只在回调期间有效的结果视图。

### 3.6 NAT 服务 Hub 与 Node 协同

NAT 服务采用独立的 Hub + Node 拓扑。Hub 仅承担 Node 注册、健康状态、协同节点分配和
成员信息下发；它绝不处于客户端组合 Binding 响应的实时转发路径。Node 到 Hub、Node 到 Node 的
控制连接使用 TCP；Hub 与每个 Node 均同时配置 `--cert FILE --key FILE` 时，在 TCP 上启用 TLS
1.3，否则使用明文 TCP，便于本地部署和抓包测试。只提供其中一个参数属于配置错误。首期 TLS
只提供通信加密，允许自签名证书，不在协议层校验证书链、主机名或 Node 身份。

每个 Node 在对外服务前必须已监听以下端点，并通过常驻 Hub 控制连接注册：

```text
node_id, boot_id, load, heartbeat_interval
每个地址族的 public_ip、probe_endpoint、change_port_endpoint、control_endpoint
```

- `probe_endpoint` 是客户端发送 `PRIMARY_BINDING` 和 `ALTERNATE_BINDING` 请求的 UDP endpoint。
- `change_port_endpoint` 与同族 `probe_endpoint` 必须使用相同公网 IP、不同 UDP port，仅用于
  本机发送 `CHANGE_PORT` 或 `CHANGE_IP | CHANGE_PORT` 响应，不接收客户端 Binding 请求。
- `control_endpoint` 是其他 Node 建立 TCP 或 TCP + TLS 协同连接的 endpoint；它可以是 Node 间可路由的
  私网地址，不能由客户端 NAT 探测使用。
- Node 的 UDP `probe_endpoint` 与 `change_port_endpoint` 可以各自以 `SO_REUSEPORT` 绑定多个 socket；
  每个 socket 归属一个独立 libevent event loop 线程。客户端探测请求没有跨包服务端状态，worker 必须在
  收到单包后完成严格校验、构造本地响应并立即发送，不能将请求投递给其他 worker。
  `CHANGE_PORT` 的响应必须由处理该请求的同 worker `change_port_endpoint` socket 发送，确保内核
  UDP 源端口与协议 `ORIGIN_ADDR` 一致。只有跨 Node 的组合响应请求投递到 Node 主 loop。
- Node 必须在解析 NAT 帧前按源 IP 限速，所有 UDP worker 共享同一组额度，不得按 `IP:port` 或单 worker
  计数。首期采用令牌桶，默认稳态额度为每源 IP `128` 包/秒、突发 `256` 包；Node 命令行可通过
  `--source-rate` 与 `--source-burst` 覆盖。超过额度的报文静默丢弃，不发送本地响应、不投递跨 Node 请求。
- Hub 接受注册后回复 `NODE_REGISTER_OK` 或 `NODE_REGISTER_REJECT`，成功后单独发送该 Node 当前
  地址族的 `NODE_ASSIGNMENT`。描述包含 `node_id`、`boot_id`、`probe_endpoint` 和
  `control_endpoint`。TLS 完成或明文 TCP 已连接后的第一条应用层消息必须为 `NODE_REGISTER`；先收到其他消息或任意
  非协议数据均关闭该 Hub 控制连接。

服务部署遵循以下地址归属规则：

- Hub 和 Node 都支持 `--interface NAME`。在 Linux 上它通过 `SO_BINDTODEVICE` 约束全部服务 socket：
  Hub 的 TCP 监听，及 Node 的 UDP probe/change-port、Node control TCP 监听、Node 到 Hub 和 Node
  到 Node 的所有主动 TCP 连接。多线部署必须指定与预期公网出口相同的网卡。
- Hub、Node 默认是 IPv4 实例：Hub 监听 `0.0.0.0:24000`；Node 监听 `0.0.0.0:24001`（probe）、
  `0.0.0.0:24002`（change-port）和 `0.0.0.0:24003`（control）。`-6` 创建独立 IPv6 实例：Hub
  默认监听 `[::]:24000`，Node 默认监听 `[::]:24001`、`[::]:24002`、`[::]:24003`。IPv4、IPv6 实例
  可在同一主机使用相同端口号；IPv6 socket 必须 `IPV6_V6ONLY`，地址族隔离不依赖 `SO_REUSEPORT`。
  Node 注册包中的 endpoint 地址仅用于保留线格式，Node 只提供这三个服务端口，不能声明自身公网服务 IP。
- Hub 必须以 `accept()` 获得的 Node 控制 TCP 连接源地址为准，覆盖该 Node 注册中
  `public_endpoint`、`probe_endpoint`、`change_port_endpoint` 和 `control_endpoint` 的 IP，端口保持 Node
  注册值。Hub 下发 assignment 时只能使用该观测地址；这避免 Node 伪造服务地址，也能匹配绑定网卡后的实际出口。
- `nat_detect_hub --listen` 与 `nat_detect_node --hub` 均接受 `HOST:PORT` 或 `[HOST]:PORT`，在服务启动期
  同步解析为一个数字地址。默认 IPv4 实例只选择 A 记录，`-6` 实例只选择 AAAA 记录，不能回退到异族地址。
  Node 的三个本地监听 endpoint 仍必须为数字 IP，且必须与 Hub 控制连接使用相同地址族。`node_id` 命令行
  参数是线上唯一的可读文本标识，长度为 1 至 128 字节；服务端将其直接写入固定长度协议字段，未使用的
  字节必须为零，禁止哈希或截断。
- Hub 以 `node_id + boot_id` 标识一个 Node 实例。相同 `node_id` 的新 `boot_id` 替换旧实例，Hub
  关闭旧 Hub 控制连接。Node 的 Hub 控制连接断开或心跳超时后，Hub 只从成员表清理该实例，既不向其他
  Node 广播下线，也不主动改写已有 assignment；该实例之后不能再被新分配。

Hub 对每个 Node、每个地址族独立分配至多两个协同 Node：`primary` 与 `backup`。候选必须在线、
地址族匹配、不等于自身，且公网 IP 与本 Node 不同；`primary` 和 `backup` 之间同样必须使用不同
公网 IP。候选不足时允许只下发一个或空 assignment。新 Node 注册时，Hub 仅为新 Node 生成 assignment，
并为现有 assignment 补齐此前为空的角色，不替换健康角色；Node 下线、心跳、负载变动和 boot 替换均不触发
其他 Node 的主动重排。每个地址族的 `assignment_version` 单调递增，Node 只接受更大的版本。

Node 收到 assignment 后，以 `node_id + boot_id` 作为连接复用键，为 primary 与 backup 异步建立
或复用 Node 间 TCP 或 TCP + TLS 长连接。禁止以 IP:port 作为实例身份键。assignment 不再引用某个 Node
不构成关闭既有链路的理由，因为对端仍可能将本 Node 作为协同方；链路仅因自身失效、duplicate 或后续
空闲回收策略关闭。双方同时拨号时，连接建立后
先交换 `NODE_LINK_HELLO(node_id, boot_id, initiator_node_id, initiator_nonce)`；同一对实例存在多条
连接时，双方按 `(initiator_node_id, initiator_nonce)` 的字典序保留唯一一条，另一条发送
`NODE_LINK_DUPLICATE` 后关闭。因 duplicate 关闭不得触发重连或向 Hub 报告节点失效。

Node 选择 primary 且对应 Node 间控制链路已经激活后，收到每个 `PRIMARY_BINDING` 请求时使用同一份
assignment 快照完成两项动作：从自身 `probe_endpoint` 发送主响应，并经 primary 长连接发送一次
`NAT_FORWARD_BINDING_RESPONSE` 控制消息。消息携带 `forward_id`、目标 Node instance、客户端 endpoint、
原 UTP `packet_number`、12 字节 token 和固定的 `PRIMARY_BINDING` step。协同 Node 对 `forward_id` 做
短期去重，从自身 `change_port_endpoint` 向客户端发送一份
`BINDING_RESPONSE(change_flags = CHANGE_IP | CHANGE_PORT)`，并在响应中将自身 `probe_endpoint` 作为
`ALTERNATE_PROBE_ENDPOINT`。主 Node 不保存按客户端 endpoint 建立的亲和表；每个请求的本地响应与
跨 Node 转发直接使用该次读取的同一 assignment。primary 链路未激活或已失效时，主 Node 仅发送不带
可用协同计划的本地响应，不转发组合响应，也不得临时改用 backup。

Node 间控制连接必须有 `PING/PONG`，建议间隔 10 秒，连续 3 次无响应判定链接失效。Hub 心跳与
Node 间 PING/PONG 职责独立。Node 检测到 primary 或 backup 链接失效时，向 Hub 发送
`NODE_ASSIGNMENT_REQUEST`，其中明确地址族、失效角色、当前 assignment 版本及对应失效实例。仅 backup
失效时 Hub 保留 primary 并补 backup；primary 失效时可提升健康 backup 为 primary 后补 backup；两者失效时
重新选择。请求版本落后于 Hub 当前版本，或失效实例不匹配当前 assignment 时，Hub 只回当前 assignment，
不按陈旧视图重排。Hub 连接失效后，Node 不得为新的 `PRIMARY_BINDING` 下发协同 endpoint 或转发组合响应。

Hub 对每个 Node、每地址族保存至多两个未过期失效实例，排除期为 30 秒。新的
`NODE_ASSIGNMENT_REQUEST` 按实例合并并刷新其排除期，不得清除另一未过期实例；容量满时淘汰最早过期项。
期间的心跳、负载更新或其他 Node 的普通上线不会将已排除实例重新选为该 Node 的主备；Hub
定时 sweep 到期后才恢复正常候选选择。

所有 TCP 控制消息使用有界长度前缀、版本和消息类型。未知类型、非法长度、状态不允许的消息或
任意非协议数据均直接关闭对应 TCP 连接。TLS 启用时，由于首期未认证 Node 身份，该设计只能防御
被动监听；明文模式仅用于测试或受信网络。Hub 与 Node 的强身份认证将在后续 NTRS 认证专项中叠加。

### 3.7 Hub 与 Node 控制线格式

首期 Hub 控制连接与 Node 间控制连接复用一套二进制前缀。所有整数均为网络字节序，任何保留字段
必须为零，且一次 TCP 读取或 TLS record 中可以携带任意数量的完整或部分消息，接收端必须按长度前缀累积解析：

```text
control_header = version:u8 | type:u8 | reserved:u16 | payload_length:u32
```

- `version = 3`，`payload_length` 不含 8 字节头，完整消息上限固定为 4096 字节。版本 1 的 16 字节
  `node_id` 以及版本 2 的旧 NAT 转发消息均不再兼容。
- 首期 type：`NODE_REGISTER=1`、`NODE_REGISTER_OK=2`、`NODE_REGISTER_REJECT=3`、
  `NODE_ASSIGNMENT=4`、`NODE_ASSIGNMENT_REQUEST=5`、`NODE_HEARTBEAT=6`、
  `NODE_LINK_HELLO=7`、`NODE_LINK_DUPLICATE=8`、`NODE_LINK_PING=9`、`NODE_LINK_PONG=10`、
  `NAT_FORWARD_BINDING_RESPONSE=11`。未定义 type 必须断开连接。
- `node_id` 为零填充的 1 至 128 字节文本，`boot_id` 为固定 16 字节；`initiator_nonce` 固定 16 字节。Node
  instance 线格式始终为 `node_id | boot_id`，不能以地址替代。
- endpoint 固定为 `family:u8 | port:u16 | address:16 bytes`。IPv4 仅使用 address 前 4 字节，剩余
  12 字节必须为零；公网 IP endpoint 的 `port=0`。地址族只允许 IPv4 或 IPv6。
- `NODE_REGISTER` payload 固定为 312 字节：`instance:144 | load:u32 | heartbeat_ms:u32 | ipv4_family:80 |
  ipv6_family:80`。family 段为 `valid:u8 | family:u8 | reserved:u16 | public | probe | change_port |
  control`，四个 endpoint 均为上述固定格式。`valid=0` 时其余 79 字节必须为零；`valid=1` 时四个
  endpoint 均须同族，public/probe/change_port 必须同 IP，且 probe 与 change_port 端口不同。
- `NODE_ASSIGNMENT` payload 固定为 376 字节：`family:u8 | flags:u8 | reserved:u16 | version:u64 |
  primary:182 | backup:182`。flags bit0/bit1 分别表示主/备存在；peer 段为 `instance:144 | probe:19 |
  control:19`。不存在的 peer 段必须全零。Hub 只在目标 Node 的当前 `boot_id` 匹配时接受 heartbeat、
  assignment request 或其他有状态控制消息。
- `NODE_HEARTBEAT` payload 固定为 148 字节：`instance:144 | load:u32`。
- `NODE_ASSIGNMENT_REQUEST` payload 固定为 444 字节：`family:u8 | failed_roles:u8 | reserved:u16 |
  assignment_version:u64 | requester_instance:144 | failed_primary:144 | failed_backup:144`。`failed_roles`
  bit0/bit1 分别表示 primary/backup，未置位角色的对应实例必须全零。
- `NODE_LINK_HELLO` payload 固定为 288 字节：`sender_instance:144 | initiator_node_id:128 |
  initiator_nonce:16`。出站连接在 TLS 完成或明文 TCP 已连接后立即发送；入站连接收到并校验后使用同一组 initiator
  字段回送。`initiator_node_id` 与 `initiator_nonce` 均不得全零。
- `NAT_FORWARD_BINDING_RESPONSE` payload 固定为 195 字节：`forward_id:u64 | target_instance:144 |
  client_endpoint:19 | packet_number:u64 | token:12 | step:u8 | reserved:3`。`target_instance` 必须等于
  接收 Node 的当前实例，`packet_number` 和 `forward_id` 均不得为零，`step` 固定为
  `PRIMARY_BINDING`，保留字段必须为零。该消息只在已激活的 Node 间控制链路上传输；协同 Node 对
  `forward_id` 做短期去重，然后从自身 change-port endpoint 构造并发送带原
  `packet_number/token/step` 的组合 `BINDING_RESPONSE`。

---

## 4. 用户驱动的打洞服务注册

注册、更新与反注册的公开 API、状态机和线格式，以
[`2026-08-22-libutp-ntrs-rendezvous-half-association.md`](2026-08-22-libutp-ntrs-rendezvous-half-association.md)
第 3、7 节为准。注册关联使用零 CID `UTP_TYPE_RENDEZVOUS` 半连接和 8 字节
`registration_token`，不建立控制 connection，也不使用 `generation`、`NODE_*` 消息或 32 字节 token。

- 调用前 Context 必须已 bind；NTRS endpoint 由当前注册调用显式提供，地址族必须与 Context socket
  一致，且不得从 NAT 探测配置隐式继承。
- 每次注册或更新都重新枚举 local candidates。未显式绑定网卡时排除 loopback、link-local、Docker、
  veth、bridge、tunnel 等虚拟或容器接口地址；显式绑定网卡时只枚举该网卡，最多保留四项。
- NAT 探测只更新 Context 缓存，不自动重注册。调用方决定何时再次调用注册接口；`UNKNOWN` 允许注册，
  并按对称 NAT 的随机端口候选规则处理。

---

## 5. 实现顺序与验收

1. 在 Context 中加入调用方提供的服务 IP endpoint 配置和 NAT 探测状态机。
2. 定义 NAT 服务 UTP 承载探测计划、观测与结果协议，并完成多节点协同服务。
3. 按 2026-08-22 半连接规格实现用户驱动的 NTRS 注册、反注册、PING/PONG、校准与节点租约。
4. 实现 `UTP_TYPE_RENDEZVOUS`、`FrameRendezvous`、REQUEST/REDIRECT/FORWARD/PUNCH、
   CandidatePlan 和零 CID 单次开洞状态机。

验收至少覆盖：

- NAT 探测与打洞注册传入相同服务 IP 时拒绝相关操作。
- 同一 Context socket 的 NAT 探测结果能异步回调；并发探测返回 `IN_PROGRESS` 且已有任务继续执行。
- 取消 NAT 探测不触发回调，调用返回后可以立刻启动下一次探测。
- IPv4/IPv6 NAT 记录互不覆盖，过期记录降级为 `UNKNOWN`。
- 注册、反注册、重连替换、PING/PONG 关联失效和节点过期均能正确回收服务端节点状态。
- NAT 服务集群协同不要求打洞服务之间建立任何服务间通信。
