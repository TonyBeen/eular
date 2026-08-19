# libutp + NTRS 快可达打洞建连设计

- 日期:2026-07-24
- 状态:主体定稿(12 条边界 + 评审 H1–H5 处理完;见 §14/§15)
- 范围:libutp(现有传输,cpp/)+ NTRS(rendezvous/中继服务)的**明文 + 无身份加密**快可达核心(身份/抗主动 MITM 归 crypto spec)

> 实施补充：NAT 服务与打洞服务的部署边界、Context 显式 NAT 探测与节点注册，以
> [`2026-08-18-libutp-ntrs-service-requirements.md`](2026-08-18-libutp-ntrs-service-requirements.md) 为准；
> 与本文旧表述冲突时，补充需求优先。

---

## 1. 目标与范围

**目标**:UDP 路径可达时,用最少 RTT 完成 peer↔peer 直连,并支持 0-RTT 重连。把原来"先打通、再 utp 握手"的两段流程**融合为一段**:握手直接搭在打洞包上。

**范围内**
- NTRS rendezvous(单包 CONNECT)、连接方向判定、打洞、对称 NAT 处理。
- 融合的打洞 + utp 握手(复用现有 Initial/Handshake/0RTT)。
- 统一 0-RTT 重连;明文与加密两条 peer 路径(punch 层做成加密无关)。

**范围外(各自另立 spec)**
- 调度层(pid 从哪来是应用/上层的事)。
- 完整加密身份握手(Ed25519 目录、双向 Finished、抗主动 MITM)。
- relay/TURN 中继转发数据(仅 NTRS 信令中转,不转发业务数据)。
- 双对称 NAT 穿透。

**验收标准**:netem / 真实 NAT 下的**打洞成功率**与 **P99 建连尾延迟**作为回归门槛。诊断时**区分"建连耗时"与"建连后会话 RTT",不互相反推**。

**RTT 目标**(`RTT_AR`=到 NTRS 一个往返,`RTT_AB`=peer 直连一个往返)
- 冷启动:`RTT_AR + RTT_AB`
- **热路径(0 信令)= 仅限"持有对端签发的 resumption 票据的重连"**:B 能**无状态验票**接受一个未经 NtrsB 转发的直连 Initial,无需 pending 状态 → `0 信令 + RTT_AB`。**无票据时不存在 0 信令**——B 没有 pending 状态就无法接受无中继的入站 Initial,必须经 NtrsB(见 §9)。
- 0-RTT 重连命中:early_data 在第 0 个包交付

---

## 2. 术语

沿用 libutp / QUIC / ICE 既有词,不自造:

| 词 | 含义 |
|---|---|
| `cid` / `scid` / `dcid` | 连接 ID / 源 cid / 目的 cid(header 内,各 4 字节) |
| `Initial` / `Handshake` / `0RTT` | 现有 utp 握手包(header `types` 0x01/0x02/0x03) |
| `CONNECT` | 新增信令包类型(见 §5) |
| `FrameConnect` | 新增帧,承载一个节点的 rendezvous 信息(见 §5) |
| `srflx` | server-reflexive 地址(NAT 外网映射,ICE 术语) |
| host-local | 本机局域网地址候选 |
| `ACTIVE` / `PASSIVE` | 连接方向角色:主动发 Initial / 被动回 Handshake |
| home Ntrs | 节点常驻保活的 NTRS(NtrsA 之于 A) |

---

## 3. 拓扑

- **每个节点有常驻 home Ntrs**,以 utp keepalive 保活:A↔NtrsA、B↔NtrsB。**NtrsA 与 NtrsB 可能不是同一台服务**。
- **NTRS 是集群,机器之间通信仅为 NAT 探测协同**(线上无双网卡,必须多机多 IP 观测才能分类 NAT)。**不做 rendezvous 转发联邦**。
- **A 连 B:直接给 NtrsB 发一个单独的 CONNECT 包**(A 从应用拿到 NtrsB 地址 + B 的 pid;A↔NtrsB 无 keepalive)。NtrsA 不在这条路径上。
- NTRS 做 **rendezvous 中转 + 方向判定 + 限速/资源保护**;**不参与 cid 分配,也不参与密钥派生**。

**NTRS 认证边界(重要)**:
- **A ↔ home NtrsA(有 keepalive)= 认证**:自签 **Ed25519 根**——A 预置 root pin,NtrsA 出示 root 签发的 NodeCertificate + 签名握手,A 验证;支持根证书轮换(泄漏可换)。详见独立的 **NTRS 认证 spec**(nat.md §8.3.2)。
- **A → NtrsB(跨服务,单包 CONNECT)= 不认证**:单包做不了握手,**不靠认证、靠 `rendezvous_id`(关联)+ M1/M2/M3(§12 DoS)替代**。这也是这两者存在的根本原因。跨 NtrsB 这条腿的网络 MITM 不设防(与首期 peer 无身份/不抗 MITM 一致)。

```
        NAT 探测协同
   NtrsA  ⇄⇄⇄⇄⇄  NtrsB
    │(keepalive)      │(keepalive)
    A                 B
    └── CONNECT 单包 ──► NtrsB(A 直接够到 B 的 Ntrs)
```

---

## 4. 整体流程(三个子系统)

### 4.1 子系统一:NAT 探测(应用层控制,不在快路径)

- **应用层决定探不探、探多频**(例如周期探测)。公网节点(如 NTRS 本身)**不探**。
- 探测走 NTRS 集群多机协同(多 IP 观测)→ 得到 `{srflx, NAT 类型}`。**NAT 类型 = NTRS 的 9 个 `NTRS_NAT_CLASS_*` 常量(0–8),映射到打洞行为类见 §6.3。** 对称侧还上报近期外网端口样本(供 §7 端口预测)。
- 探完**上报给 home Ntrs**(A→NtrsA)。每个 Ntrs 缓存其名下节点的 `{srflx, NAT 类型, 端口样本}`。
- 结论:NAT 探测结果**缓存复用**,不在每次连接的关键路径上。

### 4.2 子系统二:Rendezvous(单 datagram CONNECT,但可靠)

```
① A → NtrsB   CONNECT(单个 datagram 编码,无 keepalive 连接)
     header: types=CONNECT, scid=A_cid, dcid=0
     payload: FrameConnect{ rendezvous_id, src_pid=A, dst_pid=B,
                            src_transport_cid=A_cid, nat_type=NatA,
                            candidates=[host-local, srflx], expiry,
                            [eph_pubkey, nonce] 仅加密 }

② NtrsB 判方向(NatA × NatB;NatB 来自 B 的上报)
     → 转发给 B(走 B↔NtrsB keepalive 连接):
         header: Ntrs↔B 的 cid;payload: FrameConnect{A 的信息 + direction 已定}
     → 回 A:{ role(ACTIVE/PASSIVE), B 的候选, NatB,
              [B 的 eph_pubkey, nonce] 仅加密 }
```

**可靠性(#4):"单包" = 单个 datagram 编码,不是"只发一次"。** UDP 下 CONNECT / 回复 / 转发任一丢失都会伤成功率与 P99,所以:
- **`rendezvous_id` 作幂等键**:A 在短 PTO 内**重发 CONNECT**,直到收到回复或超时。
- **NtrsB 幂等**:对同一 `rendezvous_id` **缓存判定结果**,重复 CONNECT **重发相同回复**;但**对 B 的激活(转发)必须去重**,不重复触发 B。
- **回复/转发丢失**:A 的重发驱动 NtrsB 重发回复;NtrsB 对 B 的转发也可在未见 B 进展时有限重发(去重后)。
- **取消**:A 可发 CONNECT-cancel(带 rendezvous_id),best-effort;TTL 兜底(§6.8)。

要点:
- **rendezvous_id + A 的 transport CID + 全部 rendezvous 信息都在 FrameConnect 帧里**,因转发那一腿 header 属于 Ntrs↔B 连接,只有放帧里才能存活到 B。
- 转发的 CONNECT 先到 B,B 拿到 rendezvous_id + A 的 transport CID + srflx +(加密时)临时公钥,建 `RendezvousPending`(§11),**不必等 A 的直连 Initial 就能开始发 PATH_CHALLENGE / 回 Handshake**。

### 4.3 子系统三:融合的打洞 + 握手

```
③ ACTIVE 方发 Initial(= 打洞包);PASSIVE 方先发开洞包(scid=dcid=0,静默丢),收到 Initial 后回 Handshake
   client 收 Handshake→connected 并发数据/HandshakeDone;server 收到 client 的 HandshakeDone 帧(ack 匹配)→ promote 建真连接+connected
   B 按 rendezvous_id 去重(转发 CONNECT 与直连 Initial 归一条 RendezvousPending)
   Initial/Handshake 的重传 = 打洞重试(同一套定时器,首包 sub-RTO 小突发再退避);起 keepalive 维持映射
```

> 此流程只描述普通 Initial/punch。持有恢复状态的加密 0-RTT 走 `utp-10` §10.6 的两消息流程：server 验证 0-RTT 后回复 early_s2c 加密 `HANDSHAKE_DONE`，不等待 client→server HandshakeDone。

**角色与开洞包**
- **ACTIVE / PASSIVE 只决定握手角色**:ACTIVE=client(发 Initial=ClientHello),PASSIVE=server(回 Handshake=ServerHello),由 NtrsB 按 NAT 封闭度判定(见 §6.3)。
- **开洞包 = 纯 hole-opener**:PASSIVE 先发一个 **`PATH_CHALLENGE` 类型、header `scid=dcid=0`、不带 FrameConnect/握手/数据**的包,唯一作用是打开自己的 NAT 出向过滤(NAT 在发送瞬间建映射,与是否到达对端无关)。**ACTIVE 的 Initial 本身就是它的开洞包**,不另发。不分 NAT 类型统一如此。
- **接收方对 `scid=dcid=0` 的包:静默丢弃,绝不回 Reset/PATH_RESPONSE/任何错误。**(类型可区分:`INITIAL+dcid=0` 是被动建连;`scid=dcid=0` 是开洞包→丢。)这样开洞包**不成为反射向量**,也天然处理 hairpinning 误投(打到本地别的设备→那台设备静默丢)。
- **它不做路径验证**:无响应=无连通性反馈;真正的路径确认靠后续握手(带真实 CID 的 Initial/Handshake/HandshakeDone)。开洞包仍计入 M2 预算(§12)。

**connected 判定(对齐现有代码,唯一、无歧义)**
- client(ACTIVE)发 Initial;**收到 `HANDSHAKE` → connected**(可发 1-RTT 数据),随后发 HandshakeDone(可 piggyback 在首个数据包上)。
- server(PASSIVE)发 Handshake 后处于 **pending**(`PendingIncomingConnection`);**收到 client 的 `HandshakeDone` 帧且 `ack_handshake_pn == 本端 Handshake 包号` → promote 建真连接 + connected**(`context_impl.cpp:1544-1642` 现有逻辑)。HandshakeDone 之前到的数据被 **buffer**、promote 时按序回放(现有机制,流层保证按序)。
  - 排除:`scid=dcid=0` 开洞包(§4.3 已静默丢,不进此判定)。
  - 加密下:promote 那个包**能 AEAD 解密**才算(隐式密钥确认)。
- **为什么只认 HandshakeDone 而不是"任一非 Initial 包"**(评审 C2 结论):HandshakeDone 常 piggyback 在首个数据包上,常见即"数据+HandshakeDone 同包"→ 直接 promote+交付、不 buffer;只有该包丢时才 buffer 后续、靠 **HandshakeDone 可靠重传**补齐(数据不丢、流层按序、还带回 HandshakeDelay)。"任一非 Initial 包即 promote"依赖流重组 + HandshakeDone 重传两层隐含正确性、且漏 HandshakeDelay,收益边际,不采用。
- **可靠 + 收敛**:client 收 Handshake 即 connected(乐观 1-RTT);Handshake 丢则 client 重传 Initial、server 补发 Handshake;HandshakeDone 丢则 client 重传;始终无进展则 pending TTL 超时干净失败。**无 split-brain**。

**融合带来的规划要点**
1. **重试合并**:旧的"先打通再握手"两段变一段;Initial/Handshake 的重传同时充当打洞重试。
2. **NAT 探测前置**:CONNECT 里 A 的 NAT 类型/srflx 是探测好缓存的,不现探。
3. **方向前置**:NtrsB 用双方 NAT 类型定方向后才转发。

---

## 5. 报文与帧设计

**原则:一个 utp 包只有一个 20 字节头部;CONNECT 只是 `types` 的一个新值,其余信息全放帧里。**

### 5.1 包类型(header `types`)

```c
// 现有(src/proto/proto.h)
UTP_TYPE_INITIAL          0x01  // Client Hello
UTP_TYPE_HANDSHAKE        0x02  // Server Hello
UTP_TYPE_0RTT             0x03  // 0-RTT Data
UTP_TYPE_CONNECTION_CLOSE 0x04
UTP_TYPE_CTRL             0x05
// 新增
UTP_TYPE_CONNECT          0x06  // rendezvous 信令(A→NtrsB、NtrsB→B 转发、NtrsB→A 回复)
```

- 握手仍复用 `INITIAL/HANDSHAKE/0RTT`;HandshakeDone 保持现有帧(`kFrameHandshakeDone`,可 piggyback)。
- 打洞探测 / 路径验证复用现有 `PATH_CHALLENGE / PATH_RESPONSE` 帧(等价于 STUN 连通性检查),不新增探测包类型。
- **不升 `UTP_PROTOCOL_VERSION`**(线上尚无部署的 libutp,无向后兼容负担;新类型/帧直接并入当前版本,**所有节点/NtrsB 原子同版本升级**)。**未知帧沿用现有语义:`packet_in` 遇未知 frame 返回 `UTP_ERR_FRAME_UNEXPECTED`(拒绝该包)**——同版本下不应出现未知帧;**不承诺"优雅跳过"**(现有编码非 length-delimited,无法安全跳过未知帧)。可跳过的扩展帧(length-delimited 编码)+ 版本策略留后续(见 §14 #11)。

### 5.2 FrameConnect(新增帧)

承载一个节点的 rendezvous 信息。**一份定义、三处复用**:A→NtrsB 的 CONNECT、NtrsB→B 的转发、以及搭在打洞 Initial/0RTT 里做**归并键**与身份提示。

```
FrameConnect {
  rendezvous_id        // 128 位随机,A 每次 attempt 生成——归并/幂等/pending 的唯一键
  src_pid              // 发起方 pid;A 自报,未认证提示(真身份见 crypto spec)
  dst_pid              // 目标方 pid;NtrsB 的转发路由键;打洞包里兼作"发给我的吗"合法性检查
  src_transport_cid    // 发起方为这次连接生成的 transport CID(A 的 scid),转发后靠帧存活,供对端填 dcid
  nat_type             // 供 NtrsB 判方向
  candidates[]         // host-local + srflx(对称 NAT 追加预测端口)
  direction            // ACTIVE / PASSIVE;A→Ntrs 时为请求,Ntrs 转发时写入已定值
  expiry               // rendezvous 有效期
  [eph_pubkey, nonce]  // 仅加密模式
  [token]              // 可选,首期不用(§12);crypto spec 认证 hook,present 才验
}
```

**关键:`rendezvous_id`(128 位)是归并/去重/pending 的唯一键;32 位 transport CID 只做 transport demux,不承担会话身份**(见 §6.2/§6.3 的原因)。

各字段用在哪条腿:

| 字段 | A→NtrsB CONNECT | NtrsB→B 转发 | 打洞 Initial/0RTT | 备注 |
|---|---|---|---|---|
| `rendezvous_id` | A 生成 | 透传 | 带 | **归并/幂等/pending 唯一键**(§6.2) |
| `src_pid` | A 自报 | 透传给 B(应用层"谁在连") | 带(0-RTT 直连先到时供 B 识别) | **未认证提示**,不做安全判断 |
| `dst_pid` | =B,路由用 | =B | =本端 pid,合法性检查 | 不能省(NtrsB 路由靠它) |
| `src_transport_cid` | =A 的 scid | 透传(转发后靠帧存活) | 与 header `scid` 一致 | 供对端填 dcid;**非归并键** |
| `nat_type` | A 的类型 | 透传 | 省略 | 判方向(打洞包不带,对端已从转发 CONNECT 得到) |
| `candidates` | A 的候选 | 透传 | 省略 | 同上 |
| `direction` | 请求/未定 | 已定值 | 已定值 | ACTIVE/PASSIVE |
| `expiry` | A 定 | 透传 | 带 | 归并键的一部分 + pending TTL |
| `eph_pubkey,nonce` | 仅加密 | 透传(B 可靠拿到公钥) | 直连份用于交叉校验 | 仅加密模式 |

### 5.3 demux 规则(区分直连与打洞,零冲突)

**判据 = 包内有没有 `FrameConnect`**,贯穿 INITIAL 与 0RTT:
- **有 `FrameConnect`** → rendezvous/打洞分支(按 **`rendezvous_id`** 匹配/提升/去重,见 §6.2)。
- **无 `FrameConnect`** → 现有直连分支,**一字不改**(`isPassiveInitial` 等逻辑保持)。

---

## 6. #1 定稿:方向 + cid 归并 + 反向提升

### 6.1 libutp 的 cid 模型(前提,不改)

1. **cid 由接收/被动侧(server)分配**,防止其众多连接间冲突。
2. **dcid=0 表示"还不知道你的 cid"**(握手/0-RTT 引导态);握手成功后 dcid 有值。
3. **client 连多个 server 时,scid 由本端控制**,用来区分是哪个 server。

因此:**B 为 A↔B 连接新分配的 cid,NtrsB 事先拿不到,A 只能从 B 的应答里学到,dcid=0 引导期保留。**

**CID 分配规则**:`cid==0` 是唯一保留值(`dcid=0`=未知引导态;`scid=dcid=0`=开洞包,§4.3)。分配时**重生成直到 `cid != 0` 且不在本地连接表**;其余 `1..2³²-1` 全部可用。**不额外保留低段**——一对多分发在 overlay 层做(gossip/mesh over unicast),transport 无广播原语、无 well-known CID 需求,故无需预留。

**scid 碰撞与解复用(三层保证)**:
- **不同主机**各自生成相同 scid → 直连 Initial(dcid=0)按 **`(对端 IP+端口 + scid)`** 解复用(`context_impl.cpp:1819` 含 ip 与 port),地址不同天然区分;握手后按**本端分配的 cid** 解复用。
- **同一主机的多个 Context** → 各绑**不同 UDP 端口**,`(IP+端口)` 不同 → 即便 scid 相同也不冲突。
- **同一 Context 内**对同一对端的多条连接 → 由 scid 本地去重("重生成直到 `cid!=0` 且不在本地连接表")保证唯一。
- **实现约束**:**去掉 `socket/udp.cpp:225` 无条件 `SO_REUSEPORT`**——它启用多 socket 同端口 + 内核负载均衡,会把一个 Context 的包投递到另一个 Context/线程,破坏 `(IP+端口)` 解复用。UDP 端口 `close()` 后立即释放,顺序 rebind 同端口无需该选项;若需应对"旧 socket 未关就 rebind"的竞态,用 `SO_REUSEADDR`(不带负载均衡),不用 REUSEPORT。

### 6.2 归并键 = `rendezvous_id`(128 位),CID 只做 transport demux

**为什么不能用 32 位 CID 做归并键**:CID 32 位、可被对端选择;约 1 万并发 attempt 时随机碰撞概率已约 1%,攻击者还能主动构造碰撞 → 错误归并会导致错误提升/取消,严重时跨连接数据混淆。所以**传输 CID 与 rendezvous 身份必须分离**。

- A 在 `Connect()` 时生成 **128 位随机 `rendezvous_id`**,放进 `FrameConnect`。**归并/去重/提升/pending 的相等键 = `rendezvous_id`(唯一)**;`dst_pid` 作一致性校验(对不上即拒)、`expiry` 作有效性边界(过期即拒)——**二者都不进相等键**(时间戳进相等键会导致逐字节比对失败)。
- 传输 CID 各自按 libutp 现有规则(§6.1):A 的 `scid=A_cid` 在 header;B 作为 server 分配 `B_cid`;A 从 B 的包 `scid` 学到 B_cid。**CID 只用于 transport demux,不承担会话身份**。
- B 侧维护 `RendezvousPending` 表,**键 = rendezvous_id**;先查表,已存在就复用,不重复激活(见 §11 RendezvousPending)。

### 6.3 方向判定 + NAT 类型处理(由 NtrsB 定)

NAT 探测输出对齐 **NTRS 的 9 个类型常量**(`NTRS_NAT_CLASS_*`,值 0–8);打洞逻辑按下表归并到**行为类**:

| NTRS 类型 | 行为类 | 说明 |
|---|---|---|
| `OPEN_PUBLIC`(1)、`FULL_CONE`(3) | **Open** | 对谁都放行,理想被叫方 |
| `IP_RESTRICTED`(4) | **IP限制** | 过滤只认 IP,对端端口能进 → 无需预测 |
| `PORT_RESTRICTED`(5)、`OPEN_PUBLIC_WITH_FIREWALL`(2) | **端口限制** | 过滤认精确端口;(2)端口稳定可预测 |
| `SYMMETRIC`(6) | **对称(同 IP)** | 端口不可预测但 IP 稳定 → 预测可行 |
| `SYMMETRIC_CHANGE_LINE`(7) | **对称(多线)** | 端口+IP 都变 → 预测基本无效 |
| `UNKNOWN`(0) | **按对称,best-effort** | 方向当 ACTIVE,永不早失败 |
| `UDP_BLOCKED`(8) | **不可达** | 任一侧=8 → NtrsB 立即回失败 |

**方向规则**:封闭度 `Open < IP限制 < 端口限制 < 对称 < 对称多线`;**更封闭一侧 = ACTIVE(先发 Initial)**,开放侧 PASSIVE。**平级(同封闭度,如 Open×Open、端口×端口)时:主叫方(调 `Connect` 的 A)= ACTIVE**,被叫 = PASSIVE——按连接发起方裁决,确定且无歧义。理由:对称对每个目标用不可预测端口,只有它先发才暴露该端口;开放侧从**收到包的源地址**回(§7),多数无需预测。

**可达性矩阵(行为类)**

| | Open | IP限制 | 端口限制 | 对称 | 对称多线 |
|---|---|---|---|---|---|
| **Open** | ✓ | ✓ | ✓ | ✓ | ✓ |
| **IP限制** | ✓ | ✓ | ✓ | ✓ | ✓ |
| **端口限制** | ✓ | ✓ | ✓ | **P** | **P弱** |
| **对称** | ✓ | ✓ | **P** | ✗ | ✗ |
| **对称多线** | ✓ | ✓ | **P弱** | ✗ | ✗ |

**四档处理(实现时按此分派)**
- **✓ 直接可达**:正常打洞,预期成功。
- **P 预测(保成)**:对称(同 IP)× 端口限制——**主动**采样对称端口、算规律、喷预测端口(§7)。
- **best-effort(给机会不加码)**:`UNKNOWN`、对称多线 × 端口限制(`P弱`)——用手上已有候选/预测跑正常竞速,通了算赚到、到超时没通就失败;**不早失败,也不为它造新机制**。
- **✗ 早失败**:NtrsB 在 CONNECT 时直接回失败,连试都不试。

**NtrsB 早失败逻辑**
- **任一侧 `UDP_BLOCKED`(8)** → 立即回失败,独立错误码 `kUdpBlocked`(无 UDP 路径)。
- **双方均确诊为对称(6/7)** → 若**公网 IP 不同** → 立即回失败(`kDoubleSymmetric`);若**公网 IP 相同** → 仍转发(可能同 LAN,靠 host-local §6.6 救)。
- 含 `UNKNOWN` 的组合**一律不早失败**,best-effort 照打(两个 Unknown 很可能其实是锥型)。

### 6.4 角色定义 + 两种建连流程 + 匹配/提升

**三套角色必须分清(全文以此为准,不再用"B"代指 PASSIVE)**
- **主叫 A / 被叫 B**(应用语义):A 调 `Connect(B)`;B 是目标 pid,经其 home NtrsB 收到转发 CONNECT。
- **ACTIVE / PASSIVE**(传输/握手角色,NtrsB 按 §6.3 判定):**ACTIVE 发 Initial(=client/ClientHello);PASSIVE 发开洞包 + 回 Handshake(=server/ServerHello)。与主叫/被叫无关**——反向打洞时被叫 B 是 ACTIVE。
- **pending 归属**:**被叫 B 收到转发 CONNECT 即建 `RendezvousPending`**(存 rid、A_cid、候选、M2 预算、TTL),**不论 B 是 ACTIVE 还是 PASSIVE**;**主叫 A 的 `ConnectAttempt` 就是 A 侧 pending**。
- **应用回调**:**主叫 A 得 `Connected`(promote,仅一次);被叫 B 得 `OnNewConnection`**。都与 ACTIVE/PASSIVE 无关。
- **CID 学习**:**被叫 B 从转发 CONNECT 的 `src_transport_cid` 就拿到 A_cid**,发包可直接填 `dcid=A_cid`;**主叫 A 从对端首包的 `scid` 学到对端 cid**。每方分配自己的 scid(§6.1)。

**Case 1 — 正常(A=ACTIVE, B=PASSIVE)**
```
A:  Connect(B) → 生成 rid + A_cid → 发 CONNECT 给 NtrsB
NtrsB: 定 A=ACTIVE / B=PASSIVE → 转发给 B(附 A 候选+A_cid) + 回 A(role=ACTIVE, 附 B 候选)
B:  收转发 CONNECT → 建 RendezvousPending;因 PASSIVE → 发开洞包(scid=dcid=0)开自己过滤
A:  发 Initial(scid=A_cid, dcid=0, FrameConnect{rid}) → B 各候选
B:  收 Initial → rid 命中 pending → 建真正 passive Connection、分配 B_cid → 回 Handshake(scid=B_cid, dcid=A_cid)
     → B 应用得 OnNewConnection
A:  收 Handshake → 从 scid 学到 B_cid → Connect 成功(Connected 回调,一次)
```

**Case 2 — 反向(A=PASSIVE, B=ACTIVE;如 B 对称、A FullCone)**
```
A:  Connect(B) → 生成 rid + A_cid → 发 CONNECT 给 NtrsB
NtrsB: 定 B=ACTIVE / A=PASSIVE → 转发给 B(role=ACTIVE, 附 A 候选+A_cid) + 回 A(role=PASSIVE, 附 B 候选)
A:  ConnectAttempt 即 A 侧 pending;因 PASSIVE → 发开洞包(scid=dcid=0),等 B 的 Initial
B:  收转发 CONNECT → 因 ACTIVE → 作 client 建连、分配 B_cid → 发 Initial(scid=B_cid, dcid=A_cid〔已从转发得〕, FrameConnect{rid}) → A 各候选
A:  收 B 的 Initial → rid 命中自己的 ConnectAttempt → promote 为 Connect 结果;从 scid 学到 B_cid
     → 作 server 回 Handshake(scid=A_cid, dcid=B_cid) → A 得 Connected(一次)
B:  收 Handshake → 确认/学到 A_cid → 握手完成 → B 应用得 OnNewConnection
```
> 两 case 的差别只在"谁 ACTIVE、谁先发 Initial、谁 promote";**建 pending / 回调归属 / CID 分配规则完全一致**。这解决了旧稿把"B"当"PASSIVE"、以及"scid 必须==A_cid"与"scid 由发包方控制"的自相矛盾。

**匹配/提升规则(按 rendezvous_id)**
- 入站包带 `FrameConnect` 且 **`rendezvous_id` 等于本端登记值** → 命中;并校验 `dst_pid` 一致、未过期(`expiry` 仅有效性边界)。**CID 值不参与匹配**,从对端 `scid` 学到其 transport cid。
  - 命中方是**主叫**(其 ConnectAttempt 登记了该 rid)→ **promote 为 Connect 结果**,不走 `OnNewConnection`。
  - 命中方是**被叫**(其 RendezvousPending 登记了该 rid)→ 归入该 pending。
- 带 `FrameConnect` 但无登记匹配 → 被叫首次收到 → **新建 RendezvousPending**。
- 无 `FrameConnect` → 现有直连逻辑;`scid=dcid=0`(开洞包)→ 静默丢(§4.3)。

### 6.5 同时开不重复 / 迟到去重

- 两端各建连接对象但都引用同一 **rendezvous_id**;交叉包按 rendezvous_id 归一条,不新建。
- 提交路由后,同 rendezvous_id 的迟到包 → 当迁移探测(PATH 验证,不更优不切),**绝不新建连接**;attempt 已终态则丢弃。

### 6.6 #12 定稿:host-local 候选与 hairpinning

同一 NAT 后的两节点(同 LAN / 同 CGNAT)打对方 srflx 常因 NAT 不支持 hairpin 失败,必须靠 host-local 内网直连。

1. **候选采集**(进 `candidates[]`):host-local(枚举本机接口地址,排除 loopback;含 **IPv4 私网** + **IPv6 GUA**)、srflx(NTRS 观测)、对称时加 predicted 端口(§7)。
2. **默认包含 host-local(含私网),不默认过滤**——hairpinning 靠它;并发竞速下不可达私网路径快速失败、不阻塞 srflx。
3. **无需同 NAT 检测(候选优先级层面)**:host-local + srflx(+predicted)**全部并发竞速**,正确路径自然胜出——同 LAN 下 host-local 更快且 srflx-hairpin 失败,host-local 赢;非同 LAN 则 srflx 赢。(注:NtrsB 在**双对称早失败**时会比较双方公网 IP 决定"同 IP 走 LAN / 异 IP 秒失败",见 §6.3;那是失败门控,与本条候选优先级无关。)
4. **并发竞速**(复用 §6.5):首个完成握手的路由胜出、其余取消。**附带救回同 LAN 的双对称**——内网两端间无 NAT,公网判"双对称连不通",内网 host-local 照样通。
5. **误投安全**:私网地址可能撞本地别的设备;打洞探测用 `scid=dcid=0` 开洞包(§4.3),错设备收到**静默丢弃**、不回错误 → **不建假连接**,仅少量杂散包。携带 rendezvous 绑定的握手 Initial 若误投,也因 rendezvous_id/dst_pid 不匹配而被拒。
6. **上限(接 #8)**:候选数设上限(主网卡 host-local > srflx > 有限 predicted),保证 CONNECT/握手包 ≤ 保守 MTU、并发扇出受控。
7. **IPv6 直连**:双方均有 GUA 时,IPv6 常是无 NAT、最快路径,作高优先候选;**同协议族配对**。
8. **隐私**:交换 host-local 泄露内网地址;可选抑制(类 WebRTC mDNS),**本期不做,标注**。

### 6.7 #8 定稿:握手包 MTU 与候选表上限

候选一多(多网卡 host-local + 对称的 predicted 端口)握手包超 MTU → 分片/丢弃 → 连不上。

1. **保守 MTU floor = 1280 字节(可配)**:握手/打洞/CONNECT 全部 ≤ 1280(IPv6 min,与现有 `connect0Rtt` 单包上限 1280 一致),**置 DF 不分片**。连接建立后由**现有 PLPMTUD(`src/mtu/`)向上探测**(1280→1400→1500);握手阶段不探、固定用 floor。
2. **候选数上限,按优先级保留**(高→低):① **与 Ntrs 通信/NAT 探测所用的本地 IP**(连接 socket 绑定、srflx 已确认——多宿主机发往不同目的可能走不同出口,但只有它确认可用、映射已知,**最可信**);② 其余 host-local(去重、排除 loopback/link-local);③ srflx(v4/v6 各留);④ predicted 端口(对称,≤16)。**host-local 合计 ≤8**;总量受 MTU 预算约束,**不足时从低优先(predicted)先砍**,保住高优先 host-local + srflx。
3. **截断不静默**:超限按优先级丢弃并 **log 丢了哪些**(no silent caps),诊断可见。
4. **并发扇出 = 候选数**:候选已 capped(一二十个),首轮全并发即受控,无需额外 stagger。
5. **early_data 分包**:punch 0RTT 包 = FrameConnect + 票据 + 填到 MTU 的 early_data;**超出部分作后续普通流数据**发,不硬塞一个包。
6. **与 §12 M1 共存**:CONNECT 填充到 [反放大下限, MTU];NtrsB 回复也 ≤ MTU(即 M1 上界)。B 候选多到超 MTU 时,NtrsB **按同优先级截断**后再回复/转发。

### 6.8 #7 定稿:取消、半开清理与资源上限

M2(§12)已让 B 向未验证地址的发送很快耗尽预算而停发,所以 #7 只需管**状态回收**与**取消协调**,不必担心"B 一直打死掉的 A"。

1. **B 半开 TTL 清理**:B 由转发 CONNECT 触发的尝试(不论握手中 ACTIVE/PASSIVE)设 **TTL(数秒,够打洞完成)**,超时未完成握手 → 清理临时 cid、定时器、pending 项。**复用现有"半连接超时"机制**;M2 已停发,TTL 只回收状态。
2. **竞速输家取消**:某候选完成握手、route 提交 → 立即冻结其余候选子路径(停发、删临时状态/定时器、释放临时 cid)。首个胜出即取消其余(接 §6.5)。
3. **取消/超时只影响 pending,绝不关已建成的 Connection**:处理"成功 vs 取消/超时交叉"竞态——只终止仍在握手的尝试,已 `Established` 不受影响。
4. **显式取消(可选,best-effort)**:A 放弃/超时可发 CONNECT-cancel 给 NtrsB → 尽力转发让 B 提前清理;丢失无妨,TTL 兜底。**首期可只靠 TTL,cancel 作优化**。
5. **资源上限归口**:
   - **B**:并发入站半开数上限(全局 + 按源 IP)、pending 表上限;超限直接丢弃转发 CONNECT、不建半开(联动 §12 M3)。
   - **NtrsB**:rendezvous 转发上下文 TTL + 数量/速率上限(§12)。
   - **A**:并发 outstanding ConnectAttempt 上限;每 attempt 候选/探测/总超时上限(§6.7)。
   - **临时 cid** 均有生命周期,超时回收。
6. **A 侧总超时**:ConnectAttempt 超时 → `kConnectTimeout` 失败回调,清理全部子路径(并可选发 cancel)。

### 6.9 #10 定稿:保活与 NAT 映射维持

libutp 已有 keepalive:`kFramePing` + 计时器 + `keepalive_probes=3`/`keepalive_timeout=1500ms`,配置 `enable_keepalive`、`keepalive_interval=0`(自动推算)、`max_idle_timeout=30000ms`。#10 是把它用对到 P2P 打洞场景。

1. **P2P keepalive 间隔须 < 典型 NAT 映射超时**:`max_idle_timeout=30s`、`keepalive_interval=0`(从 30s 推算)对 P2P 太松——很多 NAT(尤其运营商)~20–30s 就回收 UDP 映射。**P2P 连接显式设较短间隔(建议默认 ~15s,可配更低)**,留足余量。
2. **空闲触发(沿用)**:keepalive 只在空闲时发;数据流量本身刷新映射并重置计时器,活跃连接不浪费 Ping。
3. **双向各自刷新**:两端各发 Ping 保活自己那侧的 NAT 映射。
4. **走已提交 route**:Ping 在 committed 5-tuple 上发;连接迁移时 keepalive 跟随新 route。
5. **失效检测 + consent**:`keepalive_probes` 次探测连续丢失(每 `keepalive_timeout`)→ 判路径失效。Ping 是 ACK-eliciting,**兼作 consent/存活检查**,避免向被 NAT 重分配的陈旧映射发数据。
6. **失效后行为——首期保持现状(失败关闭)**:现有实现 keepalive 超预算直接 `abortConnection()`(`connection_impl.cpp:3266`)。**首期沿用**:映射失效/网络变化 → 失败关闭,上层重新 `Connect`。
7. **PathRecovery(不断线恢复)= 移出首期范围**:"在其余候选 PATH 重验证 / 经 NTRS 重新 CONNECT 打洞、保住逻辑连接"需要独立的 **PathRecovery 状态、stream 暂停/恢复、恢复总时限、新旧路径包号与密钥规则**,与当前 abort 语义相反且未在 §11 落地。**留后续 spec**,不在首期,以免状态机无法收敛。

---

## 7. #2 定稿:对称 NAT 可达(回观测源 + 端口预测)

**接收方回 Handshake,一律回到它实际收到那个包的源地址(peer-reflexive),不是回候选表里预填的地址。**

- 候选表只是"先往哪些地址试"的起点。
- 对称侧真实端口不在任何候选表里,只能从收到的包学到 → **以观测源为准**。
- A 收到 B 从"候选表外的地址"回来的 Handshake,照样**按 rendezvous_id 认出同一条连接**,并**把该真实地址提交为路由**。

**端口预测(仅 `对称(同 IP)` × `端口限制`,见 §6.3 的 P 格)**:
- 为什么只有这一格需要:对称 × Open/IP限制 不需要(Open 全放行;IP限制过滤只认 IP,对称端口能进);端口限制侧的过滤**认精确端口**,而对称侧朝它的端口不可预测、且端口限制侧要**先朝那个确切端口发包**才开过滤 → 必须预测。
- 做法:NtrsB 采样对称侧近期外网端口,算递增规律,给端口限制侧下发"实际 + 若干预测端口";端口限制侧对这些候选**同一轮并发喷**(不逐个多等 RTT),命中即通。
- **`对称多线`(ChangeLine,IP 也变)= P弱**:预测基本无效,首期只 best-effort 跑一遍常规候选,不做增强。
- **双对称显式失败**(§6.3 NtrsB 早失败;同公网 IP 时留 host-local)。

---

## 8. #3 定稿:一次 Connect 一个 transport CID + 一个 rendezvous_id,重复数据去重

- **规则:一次 `Connect` 从头到尾——A 侧一个 `A_cid`(transport)、全程一个 `rendezvous_id`(会话身份);直连/打洞/0-RTT/1-RTT 全是同一条连接,不是多条。**
- **实现约束(必须写死)**:竞速的直连分支与打洞分支,是**同一个 ConnectAttempt 下的多个目标地址(子路径)**,**共用同一 `rendezvous_id`、同一 `A_cid`、一份 send state**;**绝不能实现成两个独立 Connect / 两个 rendezvous_id**,否则 B 看成两条 → 数据双交付。
- **重复数据靠现有两层去重,收到相同按一份处理**:
  - 包号重放过滤(同 pn 去重)。
  - 流 offset 重组(同一段数据不管从哪条路来,按 offset 只交付一份)。
- **注**:归并/提升按 `rendezvous_id`(§6.2),数据去重按 transport CID + pn/offset;两者分工不同,不可互相替代。

---

## 9. 0-RTT 重连叠加(接 §8)

- **判据统一到 0RTT**:`0RTT` **无** `FrameConnect` = **直连 0-RTT**(打缓存候选,**必须携带对端签发的 resumption 票据**,B 无状态验票即可接受,无需 pending 状态);**有** `FrameConnect` = **打洞 0-RTT**(打新鲜候选,B 靠 `rendezvous_id` 认 RendezvousPending)。
- **热路径 0 信令的 B 端接受条件(#7)**:直连 0-RTT(无 FrameConnect)能被 B 接受,**当且仅当**它带有效 resumption 票据——票据是 B 之前签发、绑定对端与地址、B 用自己密钥无状态校验(§9.1)。**没有票据的裸 Initial 打到 B,B 无 pending 状态,按现有直连被动/丢弃处理,不构成 0 信令快路径**;此时必须经 NtrsB 转发建立 RendezvousPending。
- **happy-eyeballs 并行**:命中缓存(有票据 + 有候选)时,直连 0-RTT 与新鲜 rendezvous 并行,**同一 rendezvous_id**,谁先通谁赢,重复按 §8 去重。
- **统一 API**(破坏性变更):
  ```cpp
  struct Connect0RttInfo {
      std::string ip; uint16_t port;         // = 缓存候选
      uint32_t timeout; int8_t retries;
      std::vector<uint8_t> resumption;       // 自描述 blob:明文票据 / 加密状态,库自动识别
      std::vector<uint8_t> early_data; bool early_fin;
  };
  int32_t connect0Rtt(const Connect0RttInfo&);       // 唯一入口
  int32_t exportResumption(std::vector<uint8_t>&);   // 取代 exportSessionToken + exportSessionResumptionState
  ```
  - blob 头:`version | type(PLAINTEXT_TICKET|ENCRYPTED_STATE) | cipher | expire`,AEAD 保护、版本化,篡改即拒。
  - 删 `connect0RttWithState`;`setResumptionSecret` 保留(加密状态落盘密钥)。
  - **加密 0-RTT 策略门控**:重放模型未放行前遇 `ENCRYPTED_STATE` 明确拒绝 + 回调 reason,不静默降级;抗重放窗口两条路合并成一处。
- **连接连续性**:带 0-RTT 的打洞包**就是连接第 0 个包**。early_data 是绑 cid、ACK 前缓冲的可靠流数据 → 哪条路通就在哪条(重)传;0-RTT 被拒则**同 cid 退回 1-RTT、按相同 offset 重放**;不丢不重复交付。

### 9.1 #6 定稿:0-RTT early_data 重放契约

libutp 已有 `zero_rtt_replay_window=10s`、`zero_rtt_token_max_lifetime=600s`、`zero_rtt_replay_rejected` 统计、`OnZeroRttDecision` 回调。#6 把契约与判定层次定清。

1. **应用契约(核心)**:0-RTT early_data **可能被重放**(网络重发,或攻击者在窗口外、token 有效期内重放)。
   - **接收端**:交给应用的 early_data 必须**标记"0-RTT,可能重放"**;应用只在**幂等操作**上处理,或把副作用**推迟到握手完成(1-RTT)后**;也可经 `OnZeroRttDecision` 选择接受/拒绝。
   - **发送端**:`connect0Rtt` 的 `early_data` 文档标注"只放幂等/可安全重发的请求"。
2. **抗重放分两层,别混**:
   - **同一 Connect 的直连-0RTT 与打洞-0RTT 副本**(同 cid、同 offset)→ 走**流 offset 去重**(§8),**交付一次,不算重放、不拒**。
   - **跨连接重放**(旧 0-RTT 包被重发去开伪连接)→ 走 **replay window(按 token 记忆)+ token 有效期**,拒绝并计 `zero_rtt_replay_rejected`。
3. **窗口覆盖两条 0-RTT 路**:重放判定按 **token/连接建立** 走,与 early_data 走直连还是打洞无关——**一次新连接只判一次**,不因两条路重复触发误判。
4. **窗口是缓解非保证**:`replay_window(10s) < token 有效期(600s)`,窗口外、有效期内的重放**可能漏网** → **应用幂等契约是强制的**,窗口只缩小攻击面。可选:缩短 token 有效期 / 对非幂等操作直接拒 0-RTT。
5. **被拒不交付**:判为重放/无效票据的 early_data 一律不交付上层,计入统计。
6. **与加密门控对接(§9)**:本条定义**明文票据** 0-RTT 契约;**加密状态** 0-RTT 在重放模型放行前保持门控,放行后复用同一契约与窗口。

---

## 10. 加密叠加(punch 层加密无关)

- punch 层做成**加密无关**:明文/加密走同一套打洞流程,加密只是把临时公钥搭进消息。
- **临时公钥走可靠中介路径**解决打洞丢包:
  - `FrameConnect` 在加密模式带 `eph_pubkey, nonce`;NtrsB 转发给 B → **B 从可靠转发路径拿到 A 的公钥**,即使 A 直连 Initial 丢也能派生。
  - NtrsB→A 回复带 B 的 `eph_pubkey, nonce` → A 同样从可靠路径拿到。
  - 双方在打洞包发出前即可派生密钥,**加密 0 额外 RTT**。
  - 直连 Initial 里也带公钥用于**一致性交叉校验**(relay 份与直连份须相等,不等即拒)。
- **安全边界(诚实声明)**:临时公钥经 NtrsB 中转**不泄密**(被动 NtrsB 无法从两个公钥推出共享密钥,DH 困难性),携带公钥零机密性代价;NtrsB 能做的只有**主动替换公钥 MITM**,而这正是"无身份加密不抗主动 MITM"的既有边界,与是否经中继无关。抗主动 MITM 需身份签名,属独立 crypto spec。

### 10.1 #9 定稿:普通 Initial/punch 的 HandshakeDone 传输行为

普通 Initial/punch 的 HandshakeDone **保持现有帧 `kFrameHandshakeDone`(可 piggyback,pending/acked/重传定时器)**。**server 的 connected 判据 = 收到 client 的 HandshakeDone 帧(ack 匹配)→ promote**(§4.3,对齐现有代码,评审 C2 定 A)。加密恢复 0-RTT 是独立例外：服务端的 early_s2c `HANDSHAKE_DONE` 响应复用 header `types = UTP_TYPE_HANDSHAKE`，线格式与状态机以 `utp-10` §10.7 为准。libutp 无独立 "Finished";加密的密钥确认是隐式的(能 AEAD 解密即密钥一致)。nat.md 的显式 Finished / 双向 key confirmation / 抗主动 MITM 属 crypto spec,不在本 spec。

1. **可靠送达**:HandshakeDone 及 client 的后续包有 pn、要 ACK、丢了重传;`connected` 不清空未确认包的重传队列(§4.3 不变量)。重传耗尽 → 该路径失败。
2. **connected 判据**:client 收 `HANDSHAKE` → connected;server 收 client 的 HandshakeDone 帧(ack 匹配)→ promote(加密下须能 AEAD 解密)。开洞包不参与(§4.3)。
3. **只在胜出/已提交 route 上发,不 spray**:握手在多候选竞速,**首个走完握手的路径提交为 route**,其余候选取消(§6.8)。
4. **committed route 完不成 → 迁移**:握手包重传耗尽,若仍在竞速窗口内 fail over 到次优候选;都失败则连接失败。
5. **按 rendezvous_id 归并**:所属连接按 rendezvous_id 归并;数据去重按 transport CID + 包号/offset(§8),不新建连接。
6. **密钥材料清零**:握手失败/取消/超时/关闭的所有路径,临时私钥、shared secret、AEAD key、中间 HKDF 输出必须清零。

---

## 11. 对现有代码的改动清单

**改(仅 rendezvous 模式生效,直连路径不动)**
- `src/proto/proto.h`:新增 `UTP_TYPE_CONNECT 0x06`;**`UTP_PROTOCOL_VERSION` 不升**(greenfield 无兼容负担)。HandshakeDone **保持现有帧**,不提升为包类型。
- 新增 `FrameConnect` 帧编解码(含 128 位 `rendezvous_id`)。
- `connection_impl`/`context_impl` 入站分派:在 `isPassiveInitial` 之外,新增"包内含 `FrameConnect` → 按 **`rendezvous_id`** 匹配 attempt/RendezvousPending"的分支;命中→提升,未命中→新建 RendezvousPending,无帧→现状。**另加规则:`scid==0 && dcid==0` 的包(开洞包)静默丢弃,绝不回 Reset**(§4.3)。**注意**:现有被动 Initial 按 `(dcid==scid, peer ip:port)` 匹配(`context_impl.cpp:1819`),打洞 Initial 不走该匹配,靠 rendezvous_id。
- **`RendezvousPending`(新,独立状态,不复用 `initPassive`)**:现有 `ConnectionImpl::initPassive()` 直接置 `kStateConnected`(`connection_impl.cpp:617`),不是半连接,**不可复用**。被叫 B 收转发 CONNECT 即建 pending,存 `{rendezvous_id, A_cid, 候选, M2 预算, TTL, 本端 ACTIVE/PASSIVE}`,并发开洞包(若 PASSIVE)。pending → 真连接的转换**按 §6.4 两 case 分**:
  - **B=PASSIVE(Case 1)**:发开洞包;**收到匹配 rid 的 Initial → 分配 B_cid、回 Handshake、转入 pending;收到 client 的 HandshakeDone(ack 匹配)才 promote 建真 `Connection`+connected**(对齐现有 `PendingIncomingConnection`,C2=A)。HandshakeDone 前的数据 buffer、promote 时回放。
  - **B=ACTIVE(Case 2,反向)**:**立即作 client 建连、分配 B_cid、发 Initial**(dcid=A_cid 已从转发得),等 A 回 Handshake。
  - 两 case 都在握手完成后向 B 应用抛 `OnNewConnection`。
- `ConnectAttempt`(新):管理多目标子路径、rendezvous_id、A_cid、send state 共享、路由提交与迁移、去重、竞速取消、CONNECT 重发/幂等(§4.2)。
- 打洞/握手定时器:首包 sub-RTO 小突发 + 退避,重传兼作打洞重试。
- **反放大 credit 常量(全局,非 rendezvous-only)**:`kPathValidationSendCredit` 由 **256 改为 `3×MTU`**(§12 M2)。影响直连也影响打洞,但直连侧 3× 项主导、几乎不触发 credit floor,行为近似 no-op。
- **反放大按候选地址跟踪(punch 新增)**:现有 `m_bytesIn/m_bytesOut` 是整连接累计;punch 多候选须**按候选地址分别记收/发字节**、各自独立额度(§12 M2)。直连保持整连接模型。
- **去掉无条件 `SO_REUSEPORT`(`socket/udp.cpp:225`)**:它启用多 socket 同端口负载均衡,破坏 `(IP+端口)` 解复用(C1)。UDP 顺序 rebind 无需该选项;竞态用 `SO_REUSEADDR` 替代。

**新增模块**
- `RendezvousClient`(对 NtrsB 发单 datagram CONNECT + 短 PTO 重发 + 解析回复)。
- NTRS 服务端:keepalive/注册、srflx 观测、转发(rendezvous_id 幂等、对 B 激活去重)、方向判定、限速(源IP/目标pid/全局,§12 M3)、集群 NAT 探测协同。**首期不验 token**(§12);CONNECT 的可选 token 字段 present 才验,留给 crypto spec。
- `CandidateCache`(happy-eyeballs + 0-RTT 缓存)。

**统一 0-RTT API**:见 §9(破坏性变更)。

**首期不做(明确移出范围)**
- **§6.9 的"路径失效后不断线恢复(PathRecovery)"**:现有 keepalive 超预算直接 `abortConnection()`(`connection_impl.cpp:3266`)。首期**保持该失败关闭语义**;不断线恢复(PathRecovery 状态、stream 暂停、恢复时限、新旧路径包号/密钥规则)留后续,详见 §6.9。

**不动**:直连 `ip:port` 建连、现有 Initial/Handshake/0RTT 帧格式、方向 AES-GCM、流可靠性、CID 分配规则。

---

## 12. 安全:反射 / 放大缓解(#4/#5 定稿)

**反射向量**:A→NtrsB 是单包、源地址可伪造。攻击者伪造源=受害者发 CONNECT(dst_pid=真实节点 B),可使 ① NtrsB 回复打向受害者、② NtrsB 转发后 **B 朝受害者打洞**(新向量)、③ 打洞天生要"验证地址前先发包"违反反放大。

**首期不用 DoS token**:反射已被 M1+M2+M3 兜住,DoS token(NTRS 认证节点)边际价值小(不做地址验证、可被盗重放)。**注意区分两种"认证"**:① **home NtrsA 是认证的**(A 验 NtrsA,自签 Ed25519 根,§3 + NTRS 认证 spec);② **跨服务 A→NtrsB 单包不认证**——这条腿 rendezvous_id + 下面 M1/M2/M3 就是它的替代,NtrsB 对 A 呈**开放 rendezvous**。peer 身份/逐请求签名归 crypto spec。

**M1 — 消除 off-NtrsB 放大**
CONNECT 填充到 **≥ NtrsB 回复大小** → 反射 off NtrsB 放大 **≤1**,反射无收益。

**M2 — 按地址有界发送(复用现有 3× 反放大)**
未验证路径上 **`已发字节 ≤ 3 × 已收字节 + credit`**(现有 `connection_impl.cpp:2942` 的 `m_bytesOut ≤ m_bytesIn*3 + kPathValidationSendCredit`,`needPathValidation()` 门控,超限 `UTP_ERR_PATH_VALIDATION_BLOCKED`)。
- **credit 统一 `3 × MTU`(~3840,MTU=1280)**,直连/打洞 ACTIVE/PASSIVE 一个常量(把现有 `kPathValidationSendCredit` 从 256 改为 3×MTU)。够打洞 ACTIVE 发 1 Initial + 2 次重传;直连侧 3× 项主导、几乎不触发 credit floor,近似 no-op。
- **按候选地址独立额度(punch 新增)**:现有 `m_bytesIn/m_bytesOut` 是**整连接累计**(旧版直连单路径,够用);**punch 多候选必须按候选地址分别跟踪收/发字节**,每个未验证候选地址各有独立 `3×收+credit` 额度,避免一个候选的收发额度被用到打向另一个(可能是受害者)候选。直连保持整连接模型。
- **只由"来自该候选地址的可验证回包"(PATH_RESPONSE / 可解密握手)解除**该地址额度;解除后放开激进重传。off-B 反射上限 ≈ 3×MTU/每 CONNECT(攻击者每次先付 ~1280 填充 CONNECT → 放大 ≈3×)。

**M3 — 限速 + 上限(去 token 后的追责层)**
NtrsB 按三键限速 + B 端 pending 上限:
- **按源 IP**:反射攻击**天生要把源伪造成受害者**,所有攻击 CONNECT 落进"源=受害者"同一桶 → **天然按受害者封顶**(叠 M2 → 到受害者总反射有界)。
- **按目标 pid(dst_pid)**:封顶针对某个 B 的总量,无视源怎么变。
- **全局**:兜底海量随机源伪造的 NtrsB 资源耗尽,超额丢弃、NtrsB 不倒。
- **B 端**:每源并发 pending + 全局 pending 上限 + TTL(联动 #7)。

**残余局限(诚实标注,去 token 的代价)**
- 追责只能**按 IP**(可伪造),弱于 pid;但反射按 IP=受害者天然封顶、资源耗尽有全局兜底,够用。
- **NtrsB 开放**:知道 NtrsB 地址 + B 的 pid 者都能触发 B——**不比"B 本身可达、谁都能发 Initial"更糟**,由 M2/M3/上限 + B 的 `OnNewConnection` accept/reject 兜。
- 要"不可伪造身份封禁 / 只服务注册节点 / 抗重放" → 归 **crypto spec 的逐请求签名**。

**前向兼容 hook**:CONNECT 保留一个**可选 token 字段**(present 才验);crypto spec 加认证时不破坏线格式、不需版本变更。

**边界(非本期)**:跨运营商信任、身份认证、逐请求抗重放 → crypto spec / 后续。

---

## 13. 测试与验收

- **NAT 组合矩阵(行为类,§6.3)**:Open / IP限制 / 端口限制 / 对称(同IP) / 对称多线 全组合。重点用例:对称×端口限制走**端口预测(P)**、对称多线×端口限制走 **best-effort(P弱)**、`UDP_BLOCKED` 任一侧走 **NtrsB 早失败(kUdpBlocked)**、双对称走**早失败(同公网IP留 host-local)**、`Unknown` 走 **best-effort 不早失败**。
- **成功率 + P99**:netem(丢包/时延/重排)下打洞成功率与 P99 建连时间作回归门槛(复用 `cpp/test/scripts/netem_*`)。
- **对称可达**:回观测源、提交 prflx 路由;预测外地址被正确归并。
- **cid 归并/去重**:转发 CONNECT 与直连 Initial 归一条;0-RTT 直连+打洞双份不双交付;反向提升只回调一次。
- **0-RTT**:直连命中 / 未命中降级 / 被拒重放 / 加密门控拒绝。
- **指标分离**:建连耗时 vs 会话 RTT 分别记录;日志不含 private key / 票据 / 业务明文。

---

## 14. 边界情形与安全逐条(#1–#12,全部定稿)

设计评审时逐条收敛的 12 个边界/安全问题,均已定稿并链到对应章节:

| # | 问题 | 类别 | 状态 |
|---|---|---|---|
| 1 | 方向判定 + cid 归并 + 反向打洞提升 | 连不上/连错 | ✅ 定稿 §6 |
| 2 | 对称 NAT 可达:回观测源(peer-reflexive) | 连不上 | ✅ 定稿 §7 |
| 3 | 一次 Connect 全程一个 cid,重复数据去重 | 连错 | ✅ 定稿 §8 |
| 4 | NtrsB 反射放大 + B 主动发是新反射向量 | 安全/DoS | ✅ 定稿 §12 |
| 5 | 反放大 vs 主动被动开的冲突:未验证地址前主动发须有界 | 安全/DoS | ✅ 定稿 §12 |
| 6 | 0-RTT early_data 可重放契约:向应用暴露"可重放",抗重放窗口覆盖两路 | 安全 | ✅ 定稿 §9.1 |
| 7 | 取消 / 半开清理:NtrsB 通知 B 停止 punch;B 的被动半开 TTL 超时 | 健壮性 | ✅ 定稿 §6.8 |
| 8 | 握手包 MTU + 候选表大小上限:opener ≤ 保守 MTU,预测端口数设限 | 健壮性(可致连不上) | ✅ 定稿 §6.7 |
| 9 | 加密握手 HandshakeDone 在打洞丢包下的重传;绑提交路由;密钥清零 | 健壮性 | ✅ 定稿 §10.1 |
| 10 | keepalive 间隔 < NAT 映射超时(默认可配) | 运维(连上后掉线) | ✅ 定稿 §6.9 |
| 11 | 版本协商:greenfield 不升版本 + 原子同版本升级;未知帧沿用现有 `UTP_ERR_FRAME_UNEXPECTED`;扩展帧留后续 | 兼容 | ✅ 定稿 §5.1 |
| 12 | 同 NAT / hairpinning:host-local 候选进集合并并发尝试 | 健壮性(可致连不上) | ✅ 定稿 §6.6 |

**12 条全部定稿。** 后续 crypto spec 负责:显式 Finished/双向 key confirmation、Ed25519 身份目录、抗主动 MITM、加密 0-RTT 放行。relay/TURN(双对称、UDP 阻断)另立 spec。

---

## 15. 评审 H3/H4 处置(均已定稿)

- **H3 — token**:讨论后决定**首期直接去掉 token**(反射已由 M1+M2+M3 兜住,token 边际价值小且不做地址验证/可被盗重放)。身份/认证/抗重放统一归 crypto spec 的逐请求签名;CONNECT 保留可选 token 字段作前向兼容 hook。见 §12。
- **H4 — M2 预算量级**:统一 `credit = 3×MTU`,复用现有 3× 反放大。见 §12。

**反推 utp 现有实现后新增的待定项(见 `requirements/utp-00-index.md` §4):**
- **C2 [已定 = A] connected 触发 / HandshakeDone**:普通 Initial/punch 对齐现有代码——**server 收到 client 的 HandshakeDone 帧(ack 匹配)才 promote+connected**,HandshakeDone 前的数据 buffer+回放(§4.3/§10.1)。放弃"任一非 Initial 包即 promote"。加密恢复 0-RTT 是独立例外：按 `utp-10` §10.6，server 验证后回复 early_s2c 加密 `HANDSHAKE_DONE`，客户端验证响应即 connected；该规则后续接入 NTRS 的 0-RTT 路径。`RendezvousPending` 仍复用普通 `PendingIncomingConnection` 模式。
- **C5 [已定 = A] 公共 API 返回值语义**:改**直接返错误码 + 出参**(对齐 c/ 迁移 `ERRORS.md`),废弃 `NormalizePublicStatus` 的 0/-1 归一。**约定:`0` = 成功;所有错误码 **< 0(负值)**;`> 0` 仅用于返值接口(如 `createStream` 返流 ID)。**断连/拒绝等经回调抛出的错误(`OnConnectError` code、`OnClosed`/ConnectionClose reason、`OnNewConnection` 拒绝)也统一为负值**。`errno.h` 全部 `UTP_ERR_*` 需从当前正值(如 `0x0010`)改为负值;连接关闭 reason 的**线上表示可仍为无符号,在抛给应用时映射为负错误码**。属全公共 API 范围改动(utp-12),非仅 punch;punch 的 `connect0Rtt` 等按此返负码 + 出参。
- 已定并已回写:C1(去 SO_REUSEPORT,§6.1/§11)、C3(3×MTU + 按候选地址额度,§12/§11)、C4(MTU floor 1280,§6.7/§12/§16)。

---

## 16. 参数默认值(建议,可调)

散落在各节的计时/上限集中于此;正文数值以本表为准。**除标 TBD 外均为建议默认,实现可调**。

| 参数 | 建议默认 | 出处 |
|---|---|---|
| 握手/打洞包 MTU floor | 1280(置 DF,IPv6 min) | §6.7 |
| M2 反放大 credit(`kPathValidationSendCredit`) | 3×MTU(~3600) | §12 |
| host-local 候选上限 / predicted 端口上限 | 8 / 16 | §6.7 |
| keepalive 间隔 / probes / timeout | ~15s / 3 / 1500ms | §6.9 |
| CONNECT 重发 PTO | ~200ms 起,指数退避,至 attempt 总超时 | §4.2 |
| 首包 sub-RTO 突发 | 2–3 发 @ ~50ms,后指数退避 | §4.3 |
| RendezvousPending TTL | ~5s | §6.8 |
| ConnectAttempt 总超时 | ~3s(现有 `ConnectInfo.timeout`) | §6.8 |
| rendezvous_id 长度 | 128 位 | §6.2 |
| 端口预测:采样数 / 预测数 / 置信阈值 | ~8 / ≤16 / **TBD-调参** | §7 |
| 0-RTT 抗重放窗口 / token 时效 | 10s / 600s(现有) | §9.1 |
