# libutp C 重写实现路线图(总纲 / 单一入口)

- 日期:2026-07-27
- 状态:实现前基线冻结。本文是**唯一入口**,串起 需求基线 / punch / NTRS 认证 / 全部设计决策 / c 迁移顺序 / P0–P3 分期。
- 读法:先读本文 §0–§4 建立全局观,实现某模块时再点开 §5 决策表 + 对应需求文档。

---

## 0. 为什么用 C 重写 + 本文定位

**背景**:现有 `cpp/` 是完整、行为正确的传输实现,但其**异常处理 / 资源收尾无法挽救**(RAII + 异常控制流与网络热路径的确定性/有界性目标冲突)。因此决定:

1. **用 C11 在 `c/` 中重写传输核心**(不再改 `cpp/`)。
2. `cpp/` 冻结为**行为 ground truth + 交叉参考**;12 份反推需求文档(utp-01..12)是"C 要复刻的行为蓝图"。
3. C 核心稳定后,**再用 C++ 薄封装**暴露对象式 API(opaque handle 之上)。

**关键推论**:所有"反推需求"不是描述 `cpp/` 要改什么,而是**规定 C 要实现成什么样**;所有"C1–C5 冲突""H1–H5 评审""#1–#12 边界"都是 **C 实现必须内建的设计决策**(§5),不是对 cpp 的补丁。

---

## 1. 目标与约束

**产品目标**(punch spec §1):P2P **快可达优先**(最小 RTT 建连)。明文无身份加密为**核心**,加密/认证为**可选叠加**,不阻塞可达性。P2P **只需弱安全**。

**C11 工程约束**(`c/STYLE.md` + `c/ERRORS.md`,实现时逐条遵守):

| 约束 | 要点 |
|---|---|
| 错误模型 | 公共 API 只返 `utp_status_t`(**负值**,OK=0);私有返 `utp_internal_error_t`(facility 位);POSIX 仅在系统调用边界捕获,公共边界映射。**C5 在 C 里是原生的**。 |
| 内存 | 仅经 `utp_allocator_t`;**包解析 / ACK 处理 / established 收发路径零分配**;所有内存在 Context/Connection 创建期有界。 |
| 句柄 | 公共对象 opaque;每资源一个 create/destroy owner;init 失败留空且可安全 cleanup。 |
| 缓冲 | 外部缓冲一律 `(ptr, len)` + 显式 capacity;不依赖 NUL 结尾。 |
| 边界 | 每个不可信基数集合有配置上限;size 运算前查整数溢出。 |
| 容器 | 不做通用 STL 替代;复用现有 红黑树 / 侵入队列 / 有界 ring / range_set / 单一侵入 hash 表。 |
| 日志 | 栈作用域层级 tag(≤256B),不含密钥/明文/无界 peer 数据;错误传播,日志不重复。 |
| 格式/告警 | clang-format(Google 基,**120 列,4 空格**,Tab 永不,`Standard: Latest`);`-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2`。 |
| 测试门槛 | 解析/分配/crypto/重传/索引改动需:常规 + 随机/差分 + 分配失败覆盖 + ASan/UBSan;热路径容器需 benchmark + 确定性回归。 |

---

## 2. 文档地图(权威来源)

| 类别 | 文档 | 作用 |
|---|---|---|
| **总纲** | `docs/superpowers/00-C-IMPLEMENTATION-ROADMAP.md`(本文) | 单一入口 + 决策汇总 |
| **需求基线** | `docs/superpowers/requirements/utp-00-index.md` | 12 需求索引 + 依赖图 + 决策收口 |
| | `requirements/utp-01..12-*.md` | 各模块行为蓝图(cpp=ground truth,行号可查) |
| **打洞设计** | `specs/2026-07-24-libutp-ntrs-fast-connect-design.md` | punch/fast-connect 16 节定稿 |
| **NTRS 认证** | `specs/2026-07-27-libutp-ntrs-auth-design.md` | node↔home-NtrsA 自签 Ed25519 单向认证 |
| **C 工程规范** | `c/STYLE.md` / `c/ERRORS.md` / `c/README.md` | 强制约束 + 迁移顺序 + 容器策略 |
| **C 数据拷贝策略** | `docs/superpowers/02-C-ZERO-COPY-COPY-REDUCTION.md` | `memcpy/memset` 使用边界、零拷贝演进顺序、当前可删项 |
| **交叉参考** | `cpp/`(冻结) / `doc/`(可能过时,以代码为准) | 行为 ground truth |

12 需求模块速查:01 包/帧 · 02 连接生命周期/CID/HandshakeDone · 03 流 · 04 可靠性/ACK · 05 流控 · 06 拥塞(BBR/CUBIC) · 07 路径验证/抗放大 · 08 keepalive · 09 MTU/PLPMTUD · 10 加密/0-RTT · 11 socket · 12 公共 API/配置/错误码。

---

## 3. C 迁移顺序 × 需求模块映射

来源:`c/README.md` 的 6 步迁移顺序。现状据 `c/src/` 实际文件。

| 步 | 迁移顺序(README) | 对应需求 | 现状 |
|---:|---|---|---|
| 1 | 公共 status / 配置 / opaque handle / 回调契约 | utp-12 | **部分**:`status.h` 负值枚举、`log.h` 已在;config/handle/回调待补 |
| 2 | 小值模块:地址、时间、**包头、帧编解码**、ACK 范围、容器 | utp-01, 04(range) | 容器**已完成**(allocator/buffer/hash/ring/range_set/error/log);**包头+帧编解码=立即下一步**(§7);地址/时间待补 |
| 3 | crypto 封装 + key schedule + 显式清零 | utp-10 | 未开始 |
| 4 | socket / 事件循环 / 内存管理 / 拥塞 / 调度 | utp-06,07,09,11 | 未开始 |
| 5 | stream / connection / context 状态机 | utp-02,03,05,08 | 未开始 |
| 6 | C API 对等测试 / 集成 / sanitizer / 性能 | 全部 | 未开始 |

**现有 `c/` 资产**(第 1–2 步容器部分,已带测试):`allocator` `buffer` `hash`(侵入表)`ring` `range_set` `error`(内部错误+POSIX 映射)`log`(层级 tag)`status`。

---

## 4. 分期(P0–P3)与依赖

分期是**产品维度**;它跨越 §3 的迁移步骤。**必须自底向上:先有传输核心(P0),punch(P1)才有依附点。**

| 期 | 内容 | 覆盖迁移步 | 依赖 | 验收 |
|---|---|---|---|---|
| **P0** | **C 传输核心**(明文可靠传输):proto → 地址/时间 → crypto 封装 → socket/事件循环/拥塞/MTU → stream/connection/context;直连 Initial/Handshake 建连、流可靠性、ACK、流控、keepalive、路径验证 | 2→3→4→5 | 现有容器 | 直连回环收发 + 与 cpp 行为对等测试;ASan/UBSan 绿 |
| **P1** | **punch 明文可达**:CONNECT 包类型(0x06)+ FrameConnect + RendezvousClient + RendezvousPending + ConnectAttempt + 方向判定 + 抗放大按候选地址 + demux;NTRS 服务端(keepalive/注册/srflx/转发/限速) | 加于 5 之上 | P0 | NAT 组合矩阵 + 成功率/P99 回归(§13) |
| **P2** | **NTRS 认证**:node↔home-NtrsA 自签 Ed25519 单向认证(NodeCertificate + 签名握手 + 双向 Finished + 根轮换) | 叠于 P0 crypto + P1 NTRS | P0,P1 | 信任根/MITM/Finished/降级/轮换/DNS 投毒 用例(auth spec §9) |
| **P3** | **punch 加密 + 0-RTT**:加密握手叠加打洞、统一 `connect0Rtt`(加密/非加密)、加密 0-RTT 放行、抗重放覆盖双路 | 叠于 P0 crypto | P0,P1 | 0-RTT 命中/降级/拒绝重放/加密门控(§13) |

> **后续另立 spec(不在 P0–P3)**:crypto spec(peer↔peer 身份/抗主动 MITM、全包加密 + opaque CID 混淆);relay/TURN(双对称、UDP 阻断兜底转发)。

**今晚额度限制说明**:P0 单独就是数周量级(从零实现可靠传输)。本路线图冻结后,实现按 §7 的 proto 模块里程碑起步,逐模块推进。

---

## 5. 设计决策汇总(实现必须内建)—— 单一权威表

> 所有决策已定稿。实现时**照此内建**,不要回到"冲突/待商榷"语境。出处指向 punch spec 或需求 index。

### 5.1 C1–C5(反推 cpp 后的收口 → C 内建)

| 编号 | 决策(C 内建) | 落点 C 模块 | 出处 |
|---|---|---|---|
| **C1** | **不设无条件 `SO_REUSEPORT`**。UDP 顺序 rebind 只需 `SO_REUSEADDR` 处理竞态。REUSEPORT 的多 socket 同端口负载均衡会破坏 `(IP+端口+scid)` 解复用。 | socket(步4) | punch §6.1/§11;index C1 |
| **C2** | **server 收到 client 的 HandshakeDone 帧(ack 匹配)才 promote + connected**;HandshakeDone 前数据 buffer,promote 时回放。放弃"任一非 Initial 包即 promote"。`RendezvousPending` 复用此 `PendingIncomingConnection` 模式。 | connection/context(步5) | punch §4.3/§15;index C2 |
| **C3** | 抗放大 credit 常量 **`3×MTU`(≈3840)**;**punch 多候选按候选地址分别跟踪收/发字节**、各自独立 `3×收+credit` 额度;直连保持整连接模型;仅"来自该候选的可验证回包"解除该地址额度。 | path-validation(步4)+ punch(步5) | punch §12/§16;index C3 |
| **C4** | **握手/打洞/CONNECT 包 MTU floor = 1280**(置 DF,IPv6 min);连接后 PLPMTUD 1280→1400→1500。统一口径,消除 1200/1260/1280/1400 四数分歧。 | proto(步2)+ mtu(步4) | punch §6.7/§16;index C4 |
| **C5** | **公共 API 直接返错误码 + 出参**:`0`=成功;**所有错误 < 0**;`>0` 仅返值接口(如 createStream 返流 ID)。断连/拒绝经回调抛出的错误也为负。**C 里原生如此**(`utp_status_t` 已是负值),无需 cpp 的 0/-1 归一。 | 全公共 API(步1)utp-12 | punch §15;index C5;`c/ERRORS.md` |

### 5.2 H1–H5(评审高风险处置)

| 编号 | 决策 | 出处 |
|---|---|---|
| **H1** | 角色/方向举例必须清晰 → 由 §6.4 两 case(Normal / Reverse)流程 + 明确举例满足。 | punch §6.4 |
| **H2** | 平级(NAT 对等)tiebreak:**谁发起 Connect 谁是 ACTIVE**(caller=ACTIVE)。 | punch §6.3/§6.4 |
| **H3** | **去掉 DoS token**(反射已由 M1+M2+M3 兜住;token 边际价值小、不做地址验证、可被盗重放)。CONNECT 留可选 token 字段作前向兼容 hook,身份/抗重放归 crypto spec。 | punch §12/§15 |
| **H4** | M2 反放大 credit = **`3×MTU`**(复用现有 3× 机制)。 | punch §12/§15 |
| **H5** | 转发 CONNECT 与直连 Initial 的**归并键 = `rendezvous_id`(128 位)**,CID 只做 transport demux,不做匹配键。 | punch §6.2 |

### 5.3 边界/安全 #1–#12(全部定稿)

| # | 决策要点 | 落点 | 出处 |
|---|---|---|---|
| 1 | 方向判定 + cid 归并 + 反向打洞提升(两 case) | punch/connection | §6 |
| 2 | 对称 NAT:回观测源(peer-reflexive)+ 端口预测,提交 prflx 路由 | punch/path | §7 |
| 3 | 一次 Connect 全程一个 transport cid + 一个 rendezvous_id,重复数据去重 | connection | §8 |
| 4 | NtrsB 反射/放大 + "B 主动发"新向量 → M1(填充≥回复)/M2/M3 | punch/NTRS | §12 |
| 5 | 未验证地址前主动发**必须有界**(3×收+credit,按候选地址) | path/punch | §12 |
| 6 | 0-RTT early_data **向应用暴露"可重放"**;抗重放窗口覆盖直连+打洞两路 | crypto/0rtt(P3) | §9.1 |
| 7 | 取消/半开清理:NtrsB 通知 B 停 punch;被动半开 TTL(~5s)超时回收 | punch/NTRS | §6.8 |
| 8 | opener 包 ≤ 保守 MTU(1280);host-local 候选 ≤8、predicted 端口 ≤16 | proto/punch | §6.7 |
| 9 | 加密握手 HandshakeDone 打洞丢包下重传;绑提交路由;密钥清零 | crypto/connection(P3) | §10.1 |
| 10 | keepalive 间隔 < NAT 映射超时(~15s/3/1500ms 可配) | keepalive/punch | §6.9 |
| 11 | greenfield **不升版本**(仍 2);未知帧沿用 `FRAME_UNEXPECTED`;扩展帧留后续 | proto | §5.1 |
| 12 | 同 NAT / hairpinning:host-local 候选进集合并并发尝试;同公网 IP 走局域网建连 | punch | §6.6 |

---

## 6. 线格式冻结要点(C 内建,proto 模块直接照做)

来源:utp-01 需求 + cpp `proto/`。**greenfield 不升版本**(§5.1/#11)。

- **固定头 20B,大端**,字段序:`scid(u32) dcid(u32) pn(u64) payload_length(u16) types(u8) reserve(u8)`。`UTP_HEADER_SIZE=20`,`UTP_PROTOCOL_VERSION=2`。
- **包类型**:`NONE=0x00 INITIAL=0x01 HANDSHAKE=0x02 0RTT=0x03 CONNECTION_CLOSE=0x04 CTRL=0x05`;**punch 新增 `CONNECT=0x06`**。
- **开洞包**:`scid==0 && dcid==0` 的包**静默丢弃,绝不回 Reset**。
- **帧目录**(0..21,`kFrameMax=22`):Invalid/Stream/Ack/Padding/ConnectionClose/Ping/ResetStream/StreamsBlocked/MaxStreams/PathChallenge/PathResponse/Crypto/SessionToken/AckFrequency/Version/HandshakeDone/TransportParams/HandshakeDelay/MaxData/MaxStreamData/DataBlocked/StreamDataBlocked。逐帧线格式见 utp-01 §2.4;不变量见 utp-01 §4。
  - 现状缺口(C 实现时决定补否):Ping 无独立帧类(仅定长 1);StreamsBlocked(7)/MaxStreams(8) 有枚举无编解码(收到即 UNEXPECTED)。
- **punch 新增 `FrameConnect`**:含 128 位 `rendezvous_id` + `src_pid/dst_pid/src_transport_cid/nat_type/candidates/direction/expiry` + 可选 `[eph_pubkey,nonce]` + 可选 `[token]`(前向兼容 hook)。线格式见 punch §5.2。
- **MTU floor 1280**(C4);**反放大 credit 3×MTU**(C3);**PN space 单一 App 语义**(utp-04:Init/Hsk/App 分离**未落地**,C 复刻单空间)。

---

## 7. 当前 c/ 状态 + 立即下一步:proto 模块里程碑

**下一步 = 迁移第 2 步的"包头 + 帧编解码"**(纯值模块、解析零分配、可单测,最适合独立推进)。已冻结的子任务(task #13–#19):

1. **wire 底座**:`src/internal/wire.h` 有界大端 read/write u8/u16/u32/u64(游标 + capacity 检查,溢出返 `INTERNAL_ERROR_OVERFLOW`)。
2. **包头 + 常量**:`src/internal/proto.h` + `src/proto.c`:头 encode/decode、包类型(含 CONNECT)、版本、packno 上限、MTU floor 1280。
3. **帧类型 + 包布局解析**:`src/internal/frame.h` + `src/frame.c`:帧枚举/位图/重传掩码;布局校验(frameLength 等价)+ 逐帧游标 + 位图;零分配。
4. **定长帧编解码**:Ping/Path*/HandshakeDone/HandshakeDelay/Max*/DataBlocked/ResetStream/Version/AckFrequency(normalize)/Crypto(reserved==0)。
5. **变长帧编解码**:Stream/Padding/ConnectionClose/SessionToken/TransportParams(限值校验)。
6. **Ack 帧**:largest/first_range/range_count/ack_delay(exp 位移)/附加 range;逐条不变量。
7. **fuzz + 接线 + 提交**:随机/差分解析 fuzz(≥10 万混合输入);接入 `c/CMakeLists.txt`;warnings-as-errors 绿;clang-format;分阶段提交。

**验收**:每帧 roundtrip + 类型不匹配/越界/不变量拒绝;整包铺满校验 + 未知帧拒绝;差分 fuzz 对照参考模型。

---

## 8. 未决 / 后续

- **crypto spec(独立)**:peer↔peer 身份、抗主动 MITM、显式 Finished/双向 key confirmation、加密 0-RTT 放行、`doc/全包加密与无状态可验证CID混淆方案.md` 的全包加密 + opaque CID(SipHash mask/tag)—— 均**未实现**,是目标架构。
- **relay/TURN spec(独立)**:双对称 NAT、UDP 阻断兜底转发。
- **调参 TBD**:端口预测置信阈值(punch §16)。
- **NTRS 客户端认证**:首期单向(节点验 NtrsA);NtrsA 认证节点靠 access 凭据,mTLS 式客户端签名留后续(auth spec §8)。
- **清理项**(实现时顺带,index §5):AckFrequency 两套默认(10/3/150 vs 5/3/25)、`abortConnection` vs `close()` 收敛不一致、`IP_PKTINFO` 从未启用、stream_id u32(非 doc 的 u64)。

---

*本文由 punch spec §11–§16、utp-00 index、utp-01、utp-10、NTRS auth spec、c/STYLE.md、c/ERRORS.md、c/README.md 汇总冻结。后续决策变更须先改本表再改实现。*
