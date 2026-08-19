# libutp NTRS 服务拆分与 Context 接入需求

- 日期:2026-08-18
- 状态:已确认，作为 NTRS 实现的前置需求
- 优先级:本文件在 NAT 服务、打洞服务和 Context 接入范围内优先于旧设计中的冲突表述。

---

## 1. 目标与边界

NTRS 是由 NAT 服务和打洞服务组成的系统，不是单一服务。两类服务职责、部署地址和状态均须隔离：

```text
同一个 Context UDP socket
  |
  +-- 用户调用 NAT 探测 --> NAT 服务集群（多 IP/端口、服务间协同）
  |
  +-- 用户调用节点注册 --> 打洞服务（常驻控制连接、节点表、保活、CONNECT 转发）
```

- NAT 服务负责观察 UDP 映射与过滤行为，并向 Context 返回探测结果；它不保存节点注册或 rendezvous ticket。
- 打洞服务负责维护下挂节点、节点 NAT 记录、节点租约和 CONNECT 转发；它不与其他打洞服务联邦。
- NAT 服务集群内部允许服务间通信，用于协调多个独立公网 IP/端口的探测；打洞服务不依赖这种协同。
- Context 发往 NAT 服务和打洞服务的报文必须使用同一个已 bind UDP socket。NAT 探测结果才可代表该 Context 后续 P2P 发送的映射行为。
- NAT 服务实际使用的探测 IP 与打洞服务实际使用的 IP 必须不同。两个操作传入的服务 IP 地址
  相同时，相关操作必须失败而非降级为同地址探测。
- Context、Connection 和 Stream 的所有公开接口均非线程安全，只能在其所属 Context 的事件循环
  线程调用；所有 NTRS 完成回调也在该线程同步执行。库不提供跨线程调用、锁或隐式任务串行化。

本文件固定 Context 所需的 UTP 承载 NAT 探测基线、阶段和分类规则；字段编号、编解码模块
接口及服务端内部协同 RPC 在实现 NAT 服务前单独冻结。CONNECT/FrameConnect 线格式、NTRS
服务端认证和 peer 身份认证仍由后续专项需求确定。

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

一次正常完成的任务会留下有效 NAT 记录，分类可以是 `UNKNOWN` 或 `UDP_BLOCKED`；永久本地错误
不会创建记录。记录本身只在到达 `expires_at` 时失效，任务回到 `IDLE` 不表示记录被清除。

- `PROBING` 期间再次调用返回 `UTP_STATUS_IN_PROGRESS`，已有任务继续执行。
- `utp_context_cancel_nat_probe()` 仅在 `PROBING` 期间成功；没有活动任务时返回
  `UTP_STATUS_NOT_FOUND`。它不发送取消数据报，而是同步取消本地定时器和待发送项、释放任务并
  返回 `IDLE`；主动取消绝不调用原始 `utp_on_nat_probe_fn`。调用返回后可立刻发起新的探测。
- 调用成功只表示任务已启动；最终状态通过一次 `utp_on_nat_probe_fn` 回调返回。
- NAT 分类已完成时，即使分类为 `UNKNOWN` 或 `UDP_BLOCKED`，回调也必须返回 `UTP_STATUS_OK`
  与结果视图。只有随机数、内存或永久 socket 错误等操作失败时才返回错误，且结果视图
  为空。结果视图仅在回调期间有效。
- NAT 探测结果至少包含地址族、探测侧观测的公网映射 endpoint、NAT 类型、端口样本、探测时间和绝对失效时间。
- 探测侧观测的公网 endpoint 只用于诊断、NAT 分类和端口样本，不作为打洞服务最终保存的公网 endpoint。
- 当前探测以 Context 已 bind socket 的地址族为准：绑定 IPv4 只探测 IPv4，绑定 IPv6 只探测
  IPv6。libutp 不支持双栈 bind，所有 IPv6 socket 均必须设置 `IPV6_V6ONLY=1` 并拒绝 IPv4
  数据报；调用方需要双栈时创建两个独立 Context。两个 Context 的 NAT 记录彼此独立。
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
probe_header = version:u8 | message_type:u8 | phase:u8 | flags:u8
TLV          = type:u16 | length:u16 | value
```

- `version` 首期固定为 `1`；没有 magic，UTP `type` 已完成第一层解复用。单个 NAT 探测包最大
  `2048` 字节，且不得超过 Context 的当前发送 MTU。
- `message_type` 固定为：`PROBE_REQ = 1`、`PROBE_RSP = 2`、`FILTER_REQ = 3`、
  `FILTER_RSP = 4`。`phase` 固定为：`PROBE1 = 1`、`CHANGE_PORT = 2`、`CHANGE_IP = 3`、
  `PROBE2 = 4`。`flags` 首期必须为 `0`。
- `PROBE_TOKEN` 为 12 字节随机值；每个实际发送的数据报生成新 token，响应必须原样回显。
- `PADDING` TLV 只允许出现在请求，value 必须全为零；Context 使用它精确填充到 128 字节，NAT
  服务解析后忽略其内容。首期 TLV 编号固定为：`PROBE_TOKEN = 1`、`MAPPED_ADDR = 6`、
  `ORIGIN_ADDR = 7`、`ALTERNATE_PROBE_ENDPOINT = 8`、`PADDING = 12`；未列出的编号保留。
- `request_id`、私有 `sequence` 与 `timestamp_ms` 从线格式删除。Context 按 UTP
  `packet_number` 直接定位当前阶段的发送记录，再校验 `message_type`、`phase`、`PROBE_TOKEN`
  及预期源 endpoint，不能只按包号、token 或 phase 接受响应。
- 正常响应必须携带 `MAPPED_ADDR` 和 `ORIGIN_ADDR`；首次 `PROBE1` 成功响应还应携带同地址族
  的 `ALTERNATE_PROBE_ENDPOINT`，供后续 `PROBE2` 与 `CHANGE_IP` 使用。
- `MAPPED_ADDR` 是 NAT 服务观察到的 Context 公网映射，仅用于该次 NAT 结果，不能覆盖打洞
  服务从常驻控制连接观察到的公网 endpoint。
- `ALTERNATE_PROBE_ENDPOINT` 是 NAT 服务集群的备用探测端点，包含 IP 与端口；它不是客户端
  映射、对端候选或打洞服务 endpoint。它必须与主 NAT 探测 endpoint 使用不同公网 IP，并同样受
  NAT 服务与打洞服务 IP 隔离约束；它只用于 `CHANGE_IP` 与 `PROBE2`。

`PROBE_TOKEN` 仅用于关联异步请求和过滤陈旧响应，不是服务端身份认证，也不证明 NAT 结果
真实可信。首期不在 NAT 探测私有帧引入旧 `probe_auth` HMAC；打洞服务认证仍由独立认证需求
定义。

多线出口或 CGNAT 下，NAT 服务的 `MAPPED_ADDR` 与打洞服务观察到的控制连接源地址可能不同。
这不是协议错误，也不拒绝注册；打洞服务始终以自己观察到的 `IP:port` 作为节点最终公网
endpoint，Context 不得用 `MAPPED_ADDR` 覆盖它。

旧 `ntrs` 通过已认证的 KCP 控制面 `NAT_PROBE_REQ/RSP` 预先下发 `probe1/probe2`。本次服务
拆分不建立这条 NAT 控制连接，且 Context 只配置一个 NAT 服务 IP，因此必须改为由主 NAT
端点的首个 `PROBE1_RSP` 携带 `ALTERNATE_PROBE_ENDPOINT`。NAT 服务集群负责确保该端点属于不同公网 IP；
没有这个字段或字段不合法时，客户端只能完成单端点探测并按本节规定降级，不能回退到旧 KCP
控制面请求端点。

### 3.2 IPv4 探测状态机

IPv4 必须在同一 UDP socket 上按以下顺序探测。NAT 服务负责在 `CHANGE_IP` 阶段协调另一
公网 IP 的节点发送响应，Context 不与 NAT 服务集群建立额外连接。

```text
PROBE1 -> CHANGE_PORT -> CHANGE_IP -> PROBE2 -> 完成
```

1. `PROBE1`：向配置的 NAT 服务主端点发送 `PROBE_REQ`，记录所有匹配响应的
   `MAPPED_ADDR`、RTT 和不同映射数量。首个有效响应提供 `ALTERNATE_PROBE_ENDPOINT`。
2. `CHANGE_PORT`：仍向主端点发送 `FILTER_REQ`；服务端须从相同公网 IP 的不同 UDP 端口
   返回 `FILTER_RSP`。仅当 UDP 源地址和 `ORIGIN_ADDR` 都等于这个预期的同 IP 异端口时，
   才记录“允许换端口回包”。
3. `CHANGE_IP`：仍由主端点触发 `FILTER_REQ`；NAT 服务集群从 `ALTERNATE_PROBE_ENDPOINT` 对应的不同
   公网 IP 返回 `FILTER_RSP`。仅当 UDP 源地址和 `ORIGIN_ADDR` 都等于预期的 `ALTERNATE_PROBE_ENDPOINT`
   时，才记录“允许换 IP 回包”。
4. `PROBE2`：向 `ALTERNATE_PROBE_ENDPOINT` 发送 `PROBE_REQ`，记录第二组 `MAPPED_ADDR`、RTT 和不同
   映射数量，并与 `PROBE1` 比较。

每个阶段固定进行至多三轮发送；每轮向该阶段目标端点并发发送两个 NAT 探测包，故每阶段
最多实际发送六包。阶段总时限为 `T = phase_timeout_ms`，三轮的等待窗口按 `1:2:3` 分配：

```text
t = 0       : 发送第 1 轮两个包；等待 T / 6
t = T / 6   : 发送第 2 轮两个包；等待 2T / 6
t = T / 2   : 发送第 3 轮两个包；等待 3T / 6
t = T       : 阶段结束
```

每轮中的两个包具有不同的 `packet_number` 和 `PROBE_TOKEN`。映射阶段目标仍为 3 次有效响应；
达到目标后立即取消该阶段其余待匹配记录并进入下一阶段。过滤阶段收到一条完全匹配响应即可
成功并立即推进。各轮已发送包的匹配记录均保留至阶段结束，第一轮响应即使在后续轮次到达，仍
按其 `packet_number` 计入；只有阶段推进、阶段结束或取消时才统一失效。耗尽六包或到达 `T`
后进入下一阶段并保留失败证据，不能因单包超时直接终止或判定 NAT 类型。

没有合法 `ALTERNATE_PROBE_ENDPOINT` 时不得伪造 `PROBE2` 或 `CHANGE_IP`；已得到的 `PROBE1` 结果仍可
完成回调，但 IPv4 分类必须降级为 `UNKNOWN`，除非本地地址与稳定映射完全一致而可判定为
`OPEN_PUBLIC`。过滤探测不可用也同样只降低分类精度，不能覆盖有效的映射观测。

### 3.3 匹配、失败和分类

- `PROBE1`、`PROBE2` 响应的 UDP 源地址必须等于该阶段的目标端点；过滤响应按上节的变源
  规则校验。仅有正确 token 而源地址错误的包必须丢弃。
- 发送返回临时错误时保留该次发送预算并按定时器重试；永久本地发送错误、明确 ICMP
  不可达、纯超时必须分别记录为 `LOCAL_SEND_FAILED`、`ICMP_UNREACHABLE`、`NO_RESPONSE`
  诊断，不能混为一个超时错误。
- 主端点在完整预算内没有任何合法响应时，结果为 `UDP_BLOCKED`；本地永久发送错误导致未能
  完成探测时回调失败，不创建可用于注册的 NAT 记录。辅助端点失败只使类型降级为
  `UNKNOWN`，不能把主端点已经确认的可达性改写为 `UDP_BLOCKED`。
- 主、辅映射一致且各阶段内只有一个映射时，映射行为为 endpoint-independent；不同目标映射
  不同则为对端相关映射；同阶段出现多个映射，或两个公网 IP 不同，归类
  `SYMMETRIC_MULTI_LINE`。
- 映射稳定时，`CHANGE_PORT` 和 `CHANGE_IP` 均成功为 `FULL_CONE`；仅换端口成功为
  `IP_RESTRICTED`；两个过滤阶段均无成功证据为 `PORT_RESTRICTED`。映射对端相关或不稳定
  为 `SYMMETRIC`。本地地址与映射完全一致且映射稳定时为 `OPEN_PUBLIC`。

IPv6 不得套用 IPv4 NAT44 分类。首期仅记录该地址族 UDP 是否可达，以及可选的换端口、换
IP 过滤证据；对外可将可达结果表示为 `OPEN_PUBLIC`、受过滤结果表示为
`OPEN_PUBLIC_WITH_FIREWALL`、完整预算无响应表示为 `UDP_BLOCKED`，其余情况为 `UNKNOWN`。
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
              +---------------+
              |  PROBE1_SEND  |
              +---------------+
                      |
                      v
              +---------------+
              |  PROBE1_WAIT  |<---------------------+
              +---------------+                      |
                |       |                             |
         有效响应 |       +--超时且仍有预算---> PROBE1_SEND
                |                                     |
                +--目标成功数/耗尽预算---------------+
                               |
          有有效 ALTERNATE_PROBE_ENDPOINT ?
                      |                 |
                     是                 否
                      v                 v
        CHANGE_PORT_SEND/WAIT        COMPLETE_RESULT
                      |
                      v
        CHANGE_IP_SEND/WAIT
                      |
                      v
          PROBE2_SEND/WAIT
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
| `IDLE` | `probe_nat()` | 校验 bind、NAT 服务 endpoint、地址族、选项和回调；固定主端点并清空本次临时观测 | `PROBE1_SEND` |
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
`UDP_BLOCKED`。永久发送错误映射为现有 socket 写错误；随机数失败映射为
`UTP_STATUS_RANDOM_GENERATION`。

阶段完成判定如下：

1. `PROBE1` 完成后，若至少收到一个合法响应，锁定首个合法 `MAPPED_ADDR` 作为主映射样本。
   后续响应仍计入成功数、RTT 和不同映射集合。首个合法 `ALTERNATE_PROBE_ENDPOINT` 只能在同地址族、非
   未指定地址、且与主端点具有不同公网 IP 时锁定为辅助端点；后续响应给出不同的
   `ALTERNATE_PROBE_ENDPOINT` 时，保留主映射观测但废弃辅助计划，最终按单端点降级，不能在多个候选间切换。
2. `PROBE1` 没有任何合法响应时，任务仍正常完成，结果为 `UDP_BLOCKED`。这表示在当前
   NAT 服务可达性假设下 UDP 探测不可达，不表示 Context socket 或普通 UTP 连接立即失效。
3. 存在已锁定辅助端点时依次执行 `CHANGE_PORT`、`CHANGE_IP`。每个过滤阶段只需要一条
   完全匹配响应即记录成功并立即推进；耗尽最多 6 次发送只记录该过滤证据失败，不使整个任务失败。
4. `PROBE2` 对锁定的辅助端点执行完整映射预算。其成功与否、映射集合和 `PROBE1` 的比较
   决定 IPv4 最终分类；无有效响应时保留 `PROBE1` 可达性，最终分类为 `UNKNOWN`。
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

---

## 4. 用户驱动的打洞服务注册

Context 提供异步接口：

```c
utp_status_t utp_context_register_rendezvous(
    utp_context_t* context,
    const utp_rendezvous_register_options_t* options,
    utp_on_rendezvous_registered_fn callback,
    void* user_data);

typedef void (*utp_on_rendezvous_unregistered_fn)(utp_context_t* context,
                                                   utp_status_t status,
                                                   void* user_data);

utp_status_t utp_context_unregister_rendezvous(
    utp_context_t* context,
    utp_on_rendezvous_unregistered_fn callback,
    void* user_data);
```

`utp_rendezvous_register_options_t` 至少包含 `rendezvous_service_address`、
`rendezvous_service_port`、`node_name`、连接超时和重试配置。其中服务地址是 IPv4/IPv6
字面 IP，端口不得为 `0`；地址只在调用期间借用，注册任务启动后 Context 只保存解析后的
二进制 endpoint。`node_name` 是打洞服务下挂节点的路由键。该接口是幂等 upsert：首次调用
创建注册，已注册后再次调用则更新该节点信息，不新增公开更新接口。

状态机固定为：

```text
Disconnected -> Registering -> Registered <-> Updating -> Expired
                                |
                                +-> Unregistering -> Disconnected
```

- 调用前 Context 必须已 bind，`rendezvous_service_address` 与
  `rendezvous_service_port` 必须同时有效，且地址族必须与 Context 已 bind socket 一致。
  Context 不从 `utp_context_options_t` 或此前 NAT 探测操作继承打洞服务地址。
- `Registering` 或 `Updating` 期间再次调用返回 `UTP_STATUS_IN_PROGRESS`。
- `Registered` 时调用 `utp_context_unregister_rendezvous()` 会发送 `NODE_UNREGISTER`；
  `Registering`、`Updating` 或 `Unregistering` 时调用返回 `UTP_STATUS_IN_PROGRESS`，`Disconnected`
  或 `Expired` 时返回 `UTP_STATUS_NOT_FOUND`。
- `Disconnected` 时调用会建立控制连接并发送 `NODE_REGISTER`；`Registered` 时调用会在既有控制连接上发送 `NODE_UPDATE`，并仍通过同一个完成回调报告结果。
- 反注册成功收到确认后，Context 先清除本地注册 generation 和租约、关闭内部控制连接并进入
  `Disconnected`，再调用 `utp_on_rendezvous_unregistered_fn(UTP_STATUS_OK, ...)`。NAT 探测缓存
  不受影响，用户随后可再次调用注册 upsert。
- 反注册请求超时、控制连接关闭或出现永久发送错误时，Context 同样关闭本地控制连接并进入
  `Disconnected`，再通过反注册回调报告对应错误。`NODE_UNREGISTER_REJECT` 固定回调
  `UTP_STATUS_RENDEZVOUS_REJECTED`。服务端可能已处理但响应丢失；关闭控制连接
  能使仍残留的注册按连接失效/租约规则回收，调用方可以在回调后重新注册。
- Context 与打洞服务建立常驻 UTP 控制连接。控制连接的加密、服务端认证与信任锚要求由 NTRS 认证需求单独定义，本接口不提供明文回退。
- 注册成功后，Context 保持控制连接直到用户销毁 Context、连接失效或服务端拒绝/过期注册。
- Context 销毁时不发送额外注销请求；服务端依赖控制连接失效和租约超时回收节点状态。

注册请求或后续更新向打洞服务上报每个地址族的最新 NAT 记录：

```text
node_name
nat_class
local_candidates[]     // 本次注册时重新枚举得到的有效本地 IP
probe_time
expires_at
port_samples
```

Context 在每次用户调用注册 upsert 时重新枚举 `local_candidates[]`，而不是复用 NAT 探测时的本地地址。未显式绑定网卡时，枚举结果必须排除 loopback、link-local、Docker、veth、bridge、tunnel 等虚拟或容器接口地址；显式绑定网卡时只枚举该网卡地址。打洞服务从节点控制连接的源地址自行记录公网 endpoint，节点不能上报或覆盖该值。

- 注册信息必须完整。Context 至少须持有一条未过期 NAT 记录；`nat_class` 可以为 `UNKNOWN`，但本地候选、探测时间、失效时间和端口样本不得缺失。否则注册接口返回状态错误。
- NAT 探测完成仅更新 Context 缓存，绝不自动发送 `NODE_UPDATE`。用户需要上报新结果时，必须再次调用注册 upsert 接口。
- NAT 记录过期后，Context 必须在下一次 `NODE_KEEPALIVE` 中标记为 `UNKNOWN`，直至用户再次调用 NAT 探测。

更新操作始终读取 Context 缓存的 NAT 记录，调用方不能直接伪造 `nat_class`、探测侧公网 endpoint 或端口样本。用户需要刷新 NAT 类型时，应先调用 `utp_context_probe_nat()`，再显式调用一次注册 upsert 同步。调用方可在重注册时传入新的 `node_name`；打洞服务以当前控制连接绑定的旧名称为源，检查新名称是否已被其他活跃节点占用后原子删除旧索引、写入新索引并递增 generation。新名称冲突时返回 `UTP_STATUS_EXISTS`，整个更新失败，旧注册保持不变。

### 4.1 注册令牌与反注册条件

服务端处理 `NODE_REGISTER` 成功后，必须在 `NODE_REGISTER_OK` 中签发随机的 32 字节
`registration_token`，并绑定到该节点的 `node_name`、`generation` 与当前控制连接。Context
只在内部保存该 token，不通过公共回调、配置或日志暴露它。一个已注册节点的 `NODE_UPDATE`
不改变 token；重新注册得到新的 generation 和 token。

`NODE_UNREGISTER` 必须携带：

```text
node_name
generation
registration_token[32]
```

服务端只有在三者全部匹配，且请求来自当前注册所绑定的控制连接时才删除节点。token 比较必须
使用常量时间比较。任一项不匹配时不得修改节点表，返回 `NODE_UNREGISTER_REJECT`。这保证旧
连接、旧 generation 或旧 token 的延迟反注册请求不能删除后来同名的新注册。

---

## 5. 打洞服务节点协议与保活

节点注册控制流使用常驻 UTP 连接中的单条双向控制流。控制消息采用有界、长度前缀的应用层封装；精确二进制编码在实现前单独冻结。

首期消息语义固定为：

| 消息 | 方向 | 语义 |
|---|---|---|
| `NODE_REGISTER` | Context -> 打洞服务 | 首次注册节点名称与当前 NAT 记录 |
| `NODE_REGISTER_OK` | 打洞服务 -> Context | 注册成功，返回 generation、有效租约和 `registration_token` |
| `NODE_REGISTER_REJECT` | 打洞服务 -> Context | 注册拒绝及错误原因 |
| `NODE_UPDATE` | Context -> 打洞服务 | 覆盖已注册节点的 NAT 记录 |
| `NODE_UPDATE_OK` | 打洞服务 -> Context | 更新成功，返回新的 generation 与有效租约 |
| `NODE_UPDATE_REJECT` | 打洞服务 -> Context | 更新或改名被拒绝，旧注册保持有效 |
| `NODE_KEEPALIVE` | Context -> 打洞服务 | 刷新节点注册租约，携带 generation |
| `NODE_EXPIRED` | 打洞服务 -> Context | 服务端主动通知节点注册已失效 |
| `NODE_UNREGISTER` | Context -> 打洞服务 | 携带 node_name、generation 和 registration_token 删除节点 |
| `NODE_UNREGISTER_OK` | 打洞服务 -> Context | 节点已删除，可关闭控制连接 |
| `NODE_UNREGISTER_REJECT` | 打洞服务 -> Context | 删除被拒绝；连接随后关闭并按租约回收 |

保活有两层，职责不得混淆：

- UTP transport keepalive 维持 Context 与打洞服务之间的 UDP 映射和控制连接存活。
- `NODE_KEEPALIVE` 刷新打洞服务注册表中的节点租约。它不替代 transport keepalive。

打洞服务以 `node_name` 索引节点。同名活跃注册直接拒绝并返回 `UTP_STATUS_EXISTS`，不替换旧连接。节点 NAT 记录、租约和控制连接任一失效时，打洞服务必须停止向该节点转发新的 CONNECT。

---

## 6. 与后续打洞的关系

- 打洞服务只消费节点主动上报且未过期的 NAT 记录，用于 CONNECT 方向判定、候选下发和端口预测。
- `UTP_TYPE_CONNECT`、`FrameConnect`、rendezvous ticket、候选竞速和 CONNECT 重传仍按快可达打洞设计实现；本文件不改变其 `rendezvous_id` 为唯一归并键的约束。
- `FrameConnect` 线格式必须新增显式消息种类，以区分 `REQUEST`、`RESPONSE`、`FORWARD`、`RESULT`、`CANCEL` 和 `COMPLETE`。`expiry` 仅为有效性边界，不属于 `rendezvous_id` 的相等键。
- NAT 探测结果由节点上报给其打洞服务；打洞服务之间不转发节点注册、NAT 记录或 rendezvous ticket。

---

## 7. 实现顺序与验收

1. 在 Context 中加入调用方提供的服务 IP endpoint 配置和 NAT 探测状态机。
2. 定义 NAT 服务 UTP 承载探测计划、观测与结果协议，并完成多节点协同服务。
3. 实现用户调用的打洞服务注册、控制流编解码、节点租约和双层保活。
4. 定义并实现 `FrameConnect`，再接入 CONNECT 转发、方向判定与打洞状态机。

验收至少覆盖：

- NAT 探测与打洞注册传入相同服务 IP 时拒绝相关操作。
- 同一 Context socket 的 NAT 探测结果能异步回调；并发探测返回 `IN_PROGRESS` 且已有任务继续执行。
- 取消 NAT 探测不触发回调，调用返回后可以立刻启动下一次探测。
- IPv4/IPv6 NAT 记录互不覆盖，过期记录降级为 `UNKNOWN`。
- 注册、反注册、重连替换、租约刷新、UTP 控制连接失效和节点过期均能正确回收服务端节点状态。
- NAT 服务集群协同不要求打洞服务之间建立任何服务间通信。
