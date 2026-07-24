# libutp + NTRS 快可达打洞建连设计

- 日期:2026-07-24
- 状态:进行中(#1/#2/#3 已定稿;#4–#12 见文末未决清单)
- 范围:libutp(现有传输,cpp/)+ NTRS(rendezvous/中继服务)的**明文快可达核心**

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
- 热路径(缓存候选命中):`0 信令 + RTT_AB`
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
- NTRS 只做 **rendezvous 中转 + 方向调度**,**不参与 cid 分配,也不参与密钥派生**,只转发。

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
- 探测走 NTRS 集群多机协同(多 IP 观测)→ 得到 `{srflx, NAT 类型}`。
- 探完**上报给 home Ntrs**(A→NtrsA)。每个 Ntrs 缓存其名下节点的 `{srflx, NAT 类型}`。
- 结论:NAT 探测结果**缓存复用**,不在每次连接的关键路径上。

### 4.2 子系统二:Rendezvous(单包 CONNECT)

```
① A → NtrsB   CONNECT 包(单包,无 keepalive)
     header: types=CONNECT, scid=A_cid, dcid=0
     payload: FrameConnect{ src_pid=A, dst_pid=B, cid=A_cid,
                            nat_type=NatA, candidates=[host-local, srflx],
                            [eph_pubkey, nonce] 仅加密 }

② NtrsB 判方向(NatA × NatB;NatB 来自 B 的上报)
     → 转发给 B(走 B↔NtrsB keepalive 连接):
         header: Ntrs↔B 的 cid;payload: FrameConnect{A 的信息 + direction 已定}
     → 回 A:
         { role(ACTIVE/PASSIVE), need_active_mapping(bool),
           B 的候选, NatB, [B 的 eph_pubkey, nonce] 仅加密 }
```

要点:
- **A 的 cid 与全部 rendezvous 信息都在 FrameConnect 帧里**,因为转发那一腿 header 属于 Ntrs↔B 连接,A 的 cid 只有放帧里才能存活到 B。
- 转发的 CONNECT 先到 B,B 就拿到了 A 的 cid + srflx +(加密时)临时公钥,**不必等 A 的直连 Initial 就能开始回 Handshake**。

### 4.3 子系统三:融合的打洞 + 握手

```
③ ACTIVE 方发 Initial(= 打洞包);PASSIVE 方发 PATH_CHALLENGE 开洞,收到 Initial 后回 Handshake
   B 按 A_cid 去重(转发 CONNECT 与直连 Initial 归一条)
   Initial/Handshake 的重传 = 打洞重试(同一套定时器,首包 sub-RTO 小突发再退避)
   收到任一有效回包(含 PATH_CHALLENGE)→ 由半连接进入连接态;起 keepalive 维持映射
```

**角色与开洞包**
- **ACTIVE / PASSIVE 只决定握手角色**:ACTIVE=client(发 Initial=ClientHello),PASSIVE=server(回 Handshake=ServerHello),由 NtrsB 按 NAT 封闭度判定(见 §6.3)。
- **开洞**:PASSIVE 先发一个 **PATH_CHALLENGE** 开自己过滤(带最小 `FrameConnect{cid=A_cid, dst_pid}`,不带握手/数据);**ACTIVE 的 Initial 本身就是它的开洞包**,不另发 PATH_CHALLENGE。不分 NAT 类型统一如此,**不用 need_active_mapping 条件字段**。
- **ACTIVE 收到 PASSIVE 的 PATH_CHALLENGE 时仍在 InitialSent、未连接** → 只回 PATH_RESPONSE,**不当作建连进度**(client 要收到 Handshake 才进入连接态)。对端分不清映射包/真探测,一律回 PATH_RESPONSE,这没问题。

**半连接→连接 与 split-brain**
- **PASSIVE 发出 Handshake 后进入半连接**,收到对端有效回包(Handshake 的 ACK / 数据 / 也可能是 PATH_RESPONSE)→ 进入连接态。
- 握手丢包由**现有可靠层收敛**:Handshake 有包号、要 ACK,未确认就重传;Initial 丢则 ACTIVE 重传。
- **唯一要守的不变量:`connected` 不得清空未确认 Handshake 的重传队列**。这样即使 PASSIVE 因收到 PATH_RESPONSE 提前 connected 而其 Handshake 恰好丢了,也会持续重传直到 ACTIVE 补齐,两端一致,不 split-brain;始终无 ACK/无进展则半连接超时干净失败。

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

- 握手仍复用 `INITIAL/HANDSHAKE/0RTT`。
- 打洞探测 / 路径验证复用现有 `PATH_CHALLENGE / PATH_RESPONSE` 帧(等价于 STUN 连通性检查),不新增探测包类型。
- `UTP_PROTOCOL_VERSION` 由 2 升至 3;对端不识别新类型时优雅降级(直连路径不受影响)。

### 5.2 FrameConnect(新增帧)

承载一个节点的 rendezvous 信息。**一份定义、三处复用**:A→NtrsB 的 CONNECT、NtrsB→B 的转发、以及搭在打洞 Initial/0RTT 里做 demux 键与身份提示。

```
FrameConnect {
  src_pid              // 发起方 pid;A 自报,未认证提示(真身份见 crypto spec)
  dst_pid              // 目标方 pid;NtrsB 的转发路由键;打洞包里兼作"发给我的吗"合法性检查
  cid                  // 该节点为这次连接生成的 client cid(转发后靠它对号、去重)
  nat_type             // 供 NtrsB 判方向
  candidates[]         // host-local + srflx(对称 NAT 追加预测端口)
  direction            // ACTIVE / PASSIVE;A→Ntrs 时为请求,Ntrs 转发时写入已定值
  [eph_pubkey, nonce]  // 仅加密模式
}
```

各字段用在哪条腿:

| 字段 | A→NtrsB CONNECT | NtrsB→B 转发 | 打洞 Initial/0RTT | 备注 |
|---|---|---|---|---|
| `src_pid` | A 自报 | 透传给 B(应用层"谁在连") | 带(0-RTT 直连先到时供 B 识别) | **未认证提示**,不做安全判断 |
| `dst_pid` | =B,路由用 | =B | =本端 pid,合法性检查 | 不能省(NtrsB 路由靠它) |
| `cid` | =A_cid | 透传(转发后靠它存活) | header `scid`=A_cid,帧内一致 | rendezvous 匹配/去重键 |
| `nat_type` | A 的类型 | 透传 | 可省 | 判方向 |
| `candidates` | A 的候选 | 透传 | 可省 | host-local+srflx(+预测端口) |
| `direction` | 请求/未定 | 已定值 | 已定值 | ACTIVE/PASSIVE |
| `eph_pubkey,nonce` | 仅加密 | 透传(B 可靠拿到公钥) | 直连份用于交叉校验 | 仅加密模式 |
| `same_public_ip` | —— | NtrsB 置位 | —— | NtrsB 判 A/B srflx IP 相同时提示优先 host-local(§6.6) |

### 5.3 demux 规则(区分直连与打洞,零冲突)

**判据 = 包内有没有 `FrameConnect`**,贯穿 INITIAL 与 0RTT:
- **有 `FrameConnect`** → rendezvous/打洞分支(按 cid 匹配/提升/去重)。
- **无 `FrameConnect`** → 现有直连分支,**一字不改**(`isPassiveInitial` 等逻辑保持)。

---

## 6. #1 定稿:方向 + cid 归并 + 反向提升

### 6.1 libutp 的 cid 模型(前提,不改)

1. **cid 由接收/被动侧(server)分配**,防止其众多连接间冲突。
2. **dcid=0 表示"还不知道你的 cid"**(握手/0-RTT 引导态);握手成功后 dcid 有值。
3. **client 连多个 server 时,scid 由本端控制**,用来区分是哪个 server。

因此:**B 为 A↔B 连接新分配的 cid,NtrsB 事先拿不到,A 只能从 B 的应答里学到,dcid=0 引导期保留。**

### 6.2 匹配键 = A 的 cid(不是 session_id)

- NTRS 是纯中转,不碰 cid。A 在 `Connect()` 时生成 **A_cid**,放进 CONNECT 的 `FrameConnect.cid`。
- **A_cid 是 rendezvous 的关联/去重键**:
  - 转发来的 CONNECT 里 `FrameConnect.cid` == 直连 Initial 头部 `scid` → 同一条连接。
- B 侧维护被动连接表,**键 = A_cid**;先查表,已存在就复用,**只分配一个 B_cid**。

### 6.3 方向判定(由 NtrsB 定)

**越封闭的一侧越要主动先发;对称 NAT 最封闭 → 对称侧永远 ACTIVE,开放侧 PASSIVE 回 Handshake。**

| A × B | ACTIVE(发 Initial) | 结果 |
|---|---|---|
| 对称 × FullCone | 对称侧 | 通 |
| 对称 × 端口限制 | 对称侧 | 通 |
| 端口限制 × 端口限制 | 都发(PASSIVE 侧靠转发触发) | 通 |
| 对称 × 对称 | —— | **连不通**(需 relay,非本期) |

理由:对称 NAT 对每个目标用不同且不可预测的端口,该端口**只有它自己发包时才暴露**;让它先发,开放侧从**收到包的源地址**即得真实端口,直接回,无需预测。

### 6.4 匹配/提升规则

A(逻辑发起方)`Connect()` 登记 A_cid。收到入站包:
- 带 `FrameConnect` 且 **`FrameConnect.cid`/`scid` == 自己登记的 A_cid** → **提升为本次 Connect 的结果**;从包 `scid` 学到 B_cid。**dcid 是 0 还是有值都不影响匹配**。
- 带 `FrameConnect` 但无对应 attempt → 被动/accept 路径。
- 无 `FrameConnect` → 现有直连逻辑。

**反向提升**:当方向判定把 ACTIVE 给了非逻辑发起方(如 A 调 `Connect(B)` 但 B 是对称 → B 先发),B 的首包到 A,A 按 A_cid 认出并提升为自己那次 Connect,**只触发一次 `Connected`**,不走 `OnNewConnection`。

### 6.5 同时开不重复 / 迟到去重

- 两端各建连接对象但都引用同一 A_cid;交叉包按 A_cid 归一条,不新建。
- 提交路由后,同 A_cid 的迟到包 → 当迁移探测(PATH 验证,不更优不切),**绝不新建连接**;attempt 已终态则丢弃。

### 6.6 #12 定稿:host-local 候选与 hairpinning

同一 NAT 后的两节点(同 LAN / 同 CGNAT)打对方 srflx 常因 NAT 不支持 hairpin 失败,必须靠 host-local 内网直连。

1. **候选采集**(进 `candidates[]`):host-local(枚举本机接口地址,排除 loopback;含 **IPv4 私网** + **IPv6 GUA**)、srflx(NTRS 观测)、对称时加 predicted 端口(§7)。
2. **默认包含 host-local(含私网),不默认过滤**——hairpinning 靠它;并发竞速下不可达私网路径快速失败、不阻塞 srflx。
3. **同 NAT 检测 + 优先级**:NtrsB 比较 A/B 的 srflx IP,相同则在转发/回复里置 **`same_public_ip` 提示**;收到提示的一端**优先、更积极试 host-local**,但**仍并发试 srflx 兜底**。⚠️ `same_public_ip` **不等于可达**(同 CGNAT 不同住户共享公网 IP 但内网不互通)→ 提示只是优先级启发,双路都不通则该连接失败(需 relay,非本期)。
4. **并发竞速**(复用 §6.5):host-local + srflx(+predicted)同一轮并发,首个完成握手的路由胜出、其余取消。**附带救回同 LAN 的双对称**——内网两端间无 NAT,公网判"双对称连不通",内网 host-local 照样通。
5. **误投安全**:私网地址可能撞本地别的设备;`FrameConnect{cid, dst_pid}` 绑定让错设备拒收(`kDCIDMiss`/无匹配),**不建假连接**,仅少量杂散包。
6. **上限(接 #8)**:候选数设上限(主网卡 host-local > srflx > 有限 predicted),保证 CONNECT/握手包 ≤ 保守 MTU、并发扇出受控。
7. **IPv6 直连**:双方均有 GUA 时,IPv6 常是无 NAT、最快路径,作高优先候选;**同协议族配对**。
8. **隐私**:交换 host-local 泄露内网地址;可选抑制(类 WebRTC mDNS),**本期不做,标注**。

`same_public_ip` 是 NtrsB 写入转发/回复 FrameConnect 的提示字段(见 §5.2,仅 rendezvous 判定后出现)。

---

## 7. #2 定稿:对称 NAT 可达(回观测源)

**接收方回 Handshake,一律回到它实际收到那个包的源地址(peer-reflexive),不是回候选表里预填的地址。**

- 候选表只是"先往哪些地址试"的起点。
- 对称侧真实端口不在任何候选表里,只能从收到的包学到 → **以观测源为准**。
- A 收到 B 从"候选表外的地址"回来的 Handshake,照样按 A_cid 认出同一条连接,并**把该真实地址提交为路由**。
- **双对称显式失败**(返回明确错误码,不伪装成功;留 relay 后续)。

---

## 8. #3 定稿:一次 Connect 全程一个 cid,重复数据去重

- **规则:一次 `Connect` 从头到尾只有一个 cid,直连/打洞/0-RTT/1-RTT 全共用**——是同一条连接,不是多条。
- **实现约束(必须写死)**:竞速的直连分支与打洞分支,是**同一个 ConnectAttempt 下的多个目标地址(子路径)**,**共用一个 A_cid 和一份 send state**;**绝不能实现成两个独立 Connect / 两个 cid**,否则 B 看成两条 → 数据双交付。
- **重复数据靠现有两层去重,收到相同按一份处理**:
  - 包号重放过滤(同 pn 去重)。
  - 流 offset 重组(同一段数据不管从哪条路来,按 offset 只交付一份)。

---

## 9. 0-RTT 重连叠加(接 §8)

- **判据统一到 0RTT**:`0RTT` **无** `FrameConnect` = 直连 0-RTT(打缓存候选);**有** `FrameConnect` = 打洞 0-RTT(打新鲜候选,B 靠 cid 认 rendezvous)。
- **happy-eyeballs 并行**:命中缓存时,直连 0-RTT 与新鲜 rendezvous 并行,同一 A_cid,谁先通谁赢,重复按 §8 去重。
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

---

## 10. 加密叠加(punch 层加密无关)

- punch 层做成**加密无关**:明文/加密走同一套打洞流程,加密只是把临时公钥搭进消息。
- **临时公钥走可靠中介路径**解决打洞丢包:
  - `FrameConnect` 在加密模式带 `eph_pubkey, nonce`;NtrsB 转发给 B → **B 从可靠转发路径拿到 A 的公钥**,即使 A 直连 Initial 丢也能派生。
  - NtrsB→A 回复带 B 的 `eph_pubkey, nonce` → A 同样从可靠路径拿到。
  - 双方在打洞包发出前即可派生密钥,**加密 0 额外 RTT**。
  - 直连 Initial 里也带公钥用于**一致性交叉校验**(relay 份与直连份须相等,不等即拒)。
- **安全边界(诚实声明)**:临时公钥经 NtrsB 中转**不泄密**(被动 NtrsB 无法从两个公钥推出共享密钥,DH 困难性),携带公钥零机密性代价;NtrsB 能做的只有**主动替换公钥 MITM**,而这正是"无身份加密不抗主动 MITM"的既有边界,与是否经中继无关。抗主动 MITM 需身份签名,属独立 crypto spec。

---

## 11. 对现有代码的改动清单

**改(仅 rendezvous 模式生效,直连路径不动)**
- `src/proto/proto.h`:新增 `UTP_TYPE_CONNECT 0x06`;`UTP_PROTOCOL_VERSION` 2→3。
- 新增 `FrameConnect` 帧编解码。
- `connection_impl` 入站分派:在 `isPassiveInitial` 之外,新增"包内含 `FrameConnect` → 按 `A_cid` 匹配 attempt/被动表"的分支;命中→提升,未命中→被动,无帧→现状。
- `initPassive`:新增"由转发的 CONNECT 触发、预置对端 cid + 观测源、并分配本端 cid"的入口(受限 NAT 主动开洞的唯一状态机改动)。
- `ConnectAttempt`(新):管理多目标子路径、A_cid、send state 共享、路由提交与迁移、去重、竞速取消。
- 打洞/握手定时器:首包 sub-RTO 小突发 + 退避,重传兼作打洞重试。

**新增模块**
- `RendezvousClient`(对 NtrsB 单包 CONNECT + 解析回复)。
- NTRS 服务端:keepalive/注册、srflx 观测、按 pid 转发、方向判定、集群 NAT 探测协同。
- `CandidateCache`(happy-eyeballs + 0-RTT 缓存)。

**统一 0-RTT API**:见 §9(破坏性变更)。

**不动**:直连 `ip:port` 建连、现有 Initial/Handshake/0RTT 帧格式、方向 AES-GCM、流可靠性、CID 分配规则。

---

## 12. 测试与验收

- **NAT 组合矩阵**:FullCone / 地址限制 / 端口限制 / 对称 全组合;双对称走失败路径。
- **成功率 + P99**:netem(丢包/时延/重排)下打洞成功率与 P99 建连时间作回归门槛(复用 `cpp/test/scripts/netem_*`)。
- **对称可达**:回观测源、提交 prflx 路由;预测外地址被正确归并。
- **cid 归并/去重**:转发 CONNECT 与直连 Initial 归一条;0-RTT 直连+打洞双份不双交付;反向提升只回调一次。
- **0-RTT**:直连命中 / 未命中降级 / 被拒重放 / 加密门控拒绝。
- **指标分离**:建连耗时 vs 会话 RTT 分别记录;日志不含 private key / 票据 / 业务明文。

---

## 13. 未决问题(按"连不上优先"逐条解决)

已定稿:**#1(方向/cid 归并/反向提升)、#2(对称/回观测源)、#3(单 cid 去重)、#12(host-local/hairpinning,见 §6.6)**。

待解决:

| # | 问题 | 类别 | 状态 |
|---|---|---|---|
| 4 | NtrsB 反射放大 + B 主动发是新反射向量:需地址弱回址校验 + 有界小包 + 限速 | 安全/DoS | 待解 |
| 5 | 反放大 vs 主动被动开的冲突:PASSIVE 未验证地址前主动发须有界 | 安全/DoS | 待解 |
| 6 | 0-RTT early_data 可重放契约:向应用暴露"可重放",抗重放窗口覆盖两路 | 安全 | 待解 |
| 7 | 取消 / 半开清理:NtrsB 通知 B 停止 punch;B 的被动半开 TTL 超时 | 健壮性 | 待解 |
| 8 | 握手包 MTU + 候选表大小上限:opener ≤ 保守 MTU,预测端口数设限 | 健壮性(可致连不上) | 待解 |
| 9 | 加密 Finished 在打洞丢包下的重传;双方都发时绑提交路由 | 健壮性 | 待解 |
| 10 | keepalive 间隔 < NAT 映射超时(默认可配) | 运维(连上后掉线) | 待解 |
| 11 | 版本协商:新类型/帧不识别时优雅降级 | 兼容 | 待解 |

下一步优先:**#8(候选表/MTU 上限,与 #12 候选并发直接相关)**,随后安全组 #4/#5,再 #6/#7/#9/#10/#11。
