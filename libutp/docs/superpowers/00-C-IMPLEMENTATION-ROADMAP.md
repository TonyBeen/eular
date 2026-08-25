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
| **NTRS 半连接与打洞** | `specs/2026-08-22-libutp-ntrs-rendezvous-half-association.md` | 注册、保活、校准、地址样本、Rendezvous、开洞与多 worker 模型 |
| **NTRS 服务拆分** | `specs/2026-08-18-libutp-ntrs-service-requirements.md` | NAT 服务与打洞服务隔离、Context 显式 NAT 探测、Node/Hub 部署与协同 |
| **NTRS 认证** | 待专项设计 | 基于半连接的服务端认证与凭据保护 |
| **C 工程规范** | `c/STYLE.md` / `c/ERRORS.md` / `c/README.md` | 强制约束 + 迁移顺序 + 容器策略 |
| **C 数据拷贝策略** | `docs/superpowers/02-C-ZERO-COPY-COPY-REDUCTION.md` | `memcpy/memset` 使用边界、零拷贝演进顺序、当前可删项 |
| **C 发送/关闭决策** | `docs/superpowers/specs/2026-07-31-c-send-composition-and-close.md` | close 屏障、transient strip、ACK+STREAM 合包、frame priority |
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
| **P1** | **punch 明文可达**:先实现 NAT 服务与打洞服务隔离、Context 显式探测/注册，再实现 RENDEZVOUS 包类型(0x06)+ FrameRendezvous 半连接(PING/PONG) + RendezvousClient + RendezvousPending + ConnectAttempt + CandidatePlan/零 CID 单次开洞 + 固定 A 发起握手 + demux;打洞服务端(节点注册/保活/srflx/预测/转发/限速) | 加于 5 之上 | P0 | NAT 组合矩阵 + 成功率/P99 回归(§13) |
| **P2** | **NTRS 认证**:基于半连接重新冻结服务端认证、凭据保护与根密钥轮换协议 | 叠于 P0 crypto + P1 NTRS | P0,P1 | 信任根/MITM/降级/轮换/DNS 投毒 用例 |
| **P3** | **punch 加密 + 0-RTT**:加密握手叠加打洞、统一 `connect0Rtt`(加密/非加密)、加密 0-RTT 放行、抗重放覆盖双路 | 叠于 P0 crypto | P0,P1 | 0-RTT 命中/降级/拒绝重放/加密门控(§13) |

> **后续另立 spec(不在 P0–P3)**:crypto spec(peer↔peer 身份/抗主动 MITM、全包加密 + opaque CID 混淆);relay/TURN(双对称、UDP 阻断兜底转发)。

**今晚额度限制说明**:P0 单独就是数周量级(从零实现可靠传输)。本路线图冻结后,实现按 §7 的 proto 模块里程碑起步,逐模块推进。

---

## 5. 设计决策汇总(实现必须内建)—— 单一权威表

> 所有决策已定稿。实现时**照此内建**,不要回到"冲突/待商榷"语境。出处指向 punch spec 或需求 index。

### 5.1 C1–C5(反推 cpp 后的收口 → C 内建)

| 编号 | 决策(C 内建) | 落点 C 模块 | 出处 |
|---|---|---|---|
| **C1** | **不设无条件 `SO_REUSEPORT`**。UDP 顺序 rebind 只需 `SO_REUSEADDR` 处理竞态。REUSEPORT 的多 socket 同端口负载均衡会破坏 `(IP+端口+scid)` 解复用；NTRS server 多 worker 是受控例外。 | socket(步4) | 半连接规格 §8;index C1 |
| **C2** | **普通 1-RTT** 保持 server 收到 client 的 HandshakeDone 帧(ack 匹配)才 promote + connected；**加密恢复 0-RTT** 例外：server 验证后以 early_s2c 加密 `HANDSHAKE_DONE`，客户端验证该响应即 connected，server 成功写出即 connected。两者都不得由任意非 Initial 包 promote。 | connection/context(步5) | utp-10 §10.6;index C2 |
| **C3** | 抗放大 credit 常量 **`3×MTU`(≈3840)**；多候选地址分别跟踪收/发字节并独立计算额度，直连保持整连接模型。 | path-validation(步4)+ NTRS(步5) | index C3 |
| **C4** | **握手/打洞/RENDEZVOUS 包 MTU floor = 1280**(置 DF,IPv6 min);连接后 PLPMTUD 从 `mtu_base` 经 `{1380,1450,1492,1500}` 梯队后继续二分至配置的 `mtu_max`。`1500` 是默认值和梯队节点，不是 C 端硬上限。 | proto(步2)+ mtu(步4) | 半连接规格 §5;index C4 |
| **C5** | **公共 API 直接返错误码 + 出参**:`0`=成功;**所有错误 < 0**;`>0` 仅返值接口(如 createStream 返流 ID)。断连/拒绝经回调抛出的错误也为负。**C 里原生如此**(`utp_status_t` 已是负值),无需 cpp 的 0/-1 归一。 | 全公共 API(步1) | utp-12;`c/ERRORS.md` |

### 5.2 H1–H5(评审高风险处置)

> 本节只保留仍有效的高风险处置；Rendezvous 具体流程以
> [`2026-08-22-libutp-ntrs-rendezvous-half-association.md`](specs/2026-08-22-libutp-ntrs-rendezvous-half-association.md)
> 为准。

| 编号 | 决策 | 出处 |
|---|---|---|
| **H1** | 握手方向固定：调用 `utp_context_connect()` 的 A 始终发 `INITIAL`/`0RTT`，B 始终响应；NAT 类型不参与角色交换。 | 2026-08-22 半连接规格 §1/§6 |
| **H2** | A/B 都按 CandidatePlan 向对端每个候选 endpoint 单次发送零 CID `PATH_CHALLENGE` 开洞包；不等待响应、不周期重发。 | 2026-08-22 半连接规格 §5 |
| **H3** | `registration_token` 仅标识目标 B 与 NTRS 的注册关联；请求归并使用 `rendezvous_id`。 | 2026-08-22 半连接规格 §3/§6 |
| **H4** | 未验证地址的发送额度遵循 **`3×MTU`** 反放大限制。 | C3 |
| **H5** | FORWARD/REDIRECT 与直连 Initial 的**归并键 = `rendezvous_id`(128 位)**,CID 只做 transport demux,不做匹配键。 | 2026-08-22 半连接规格 §3/§6 |

### 5.3 边界/安全 #1–#12(全部定稿)

| # | 决策要点 | 落点 | 出处 |
|---|---|---|---|
| 1 | `rendezvous_id` 归并 REQUEST/FORWARD/REDIRECT/INTRODUCTION；握手方向固定为 A 发起、B 响应。 | NTRS/connection | 2026-08-22 §1/§6 |
| 2 | 对称 NAT 的公网端口预测仅由 NTRS 完成；Peer 按 CandidatePlan 发送。 | NTRS | 2026-08-22 §4/§5 |
| 3 | 一次打洞 attempt 以一个 `rendezvous_id` 归并；最终 Initial/0RTT 的 CID 由正常握手分配，重复逻辑消息按其请求标识去重。 | connection | 2026-08-22 §2/§6 |
| 4 | NTRS 不转发业务数据；Rendezvous 的未知、非法或无匹配关联报文静默丢弃。 | NTRS | 2026-08-22 §1/§9 |
| 5 | 未验证地址前主动发**必须有界**(3×收+credit,按候选地址)。 | path/NTRS | C3 |
| 6 | 0-RTT early_data **向应用暴露"可重放"**;抗重放窗口覆盖直连+打洞两路 | crypto/0rtt(P3) | §9.1 |
| 7 | 半连接关联、注册记录和 `rendezvous_id` pending 均必须有本地超时回收；重复 REQUEST/FORWARD 使用既有记录重投，不重复创建。 | NTRS | 2026-08-22 §3/§6 |
| 8 | opener 包 ≤ 保守 MTU(1280)；每个 CandidatePlan 的 local candidate ≤4，公网候选端口默认 ≤4 且服务端可配置。 | proto/NTRS | 2026-08-22 §5 |
| 9 | 加密握手 HandshakeDone 打洞丢包下重传;绑提交路由;密钥清零 | crypto/connection(P3) | §10.1 |
| 10 | Peer-to-peer connection 使用既有 `PING + ACK`；Peer-NTRS 半连接使用 `FrameRendezvous(PING/PONG)`，两侧各自配置保活周期和超时。 | keepalive/NTRS | 2026-08-22 §1/§3 |
| 11 | greenfield **不升版本**(仍 2);未知帧沿用 `FRAME_UNEXPECTED`;扩展帧留后续 | proto | §5.1 |
| 12 | 同 NAT / hairpinning 时将 local candidates 加入 CandidatePlan 并尝试局域网建连。 | NTRS | 2026-08-22 §5 |

---

## 6. 线格式冻结要点(C 内建,proto 模块直接照做)

来源:utp-01 需求 + cpp `proto/`。**greenfield 不升版本**(§5.1/#11)。

- **固定头 20B,大端**,字段序:`scid(u32) dcid(u32) pn(u64) payload_length(u16) types(u8) reserve(u8)`。`UTP_HEADER_SIZE=20`,`UTP_PROTOCOL_VERSION=2`。
- **包类型**:`NONE=0x00 INITIAL=0x01 HANDSHAKE=0x02 0RTT=0x03 CONNECTION_CLOSE=0x04 CTRL=0x05`;**punch 新增 `RENDEZVOUS=0x06`**。
- **开洞包**:`scid==0 && dcid==0` 的包**静默丢弃,绝不回 Reset**。
- **帧目录**(0..22,`UTP_FRAME_TYPE_MAX=23`):Invalid/Stream/Ack/Padding/ConnectionClose/Ping/ResetStream/StreamsBlocked/MaxStreams/PathChallenge/PathResponse/Crypto/SessionToken/AckFrequency/Version/HandshakeDone/TransportParams/HandshakeDelay/MaxData/MaxStreamData/DataBlocked/StreamDataBlocked/StopSending。逐帧线格式见 utp-01 §2.4;不变量见 utp-01 §4。
  - C 版已实现 StreamsBlocked(7)、MaxStreams(8) 和 StopSending(22)；Ping 采用定长 1 字节直接构造。
- **punch 新增 `FrameRendezvous`**:用于注册、PING/PONG 与协调；打洞 attempt 的归并键为 128 位 `rendezvous_id`，具体线格式以 `2026-08-22-libutp-ntrs-rendezvous-half-association.md` 为准。
- **MTU floor 1280**(C4);**反放大 credit 3×MTU**(C3);**PN space 单一 App 语义**(utp-04:Init/Hsk/App 分离**未落地**,C 复刻单空间)。

---

## 7. 当前 c/ 状态 + 立即下一步:proto 模块里程碑

**下一步 = 迁移第 2 步的"包头 + 帧编解码"**(纯值模块、解析零分配、可单测,最适合独立推进)。已冻结的子任务(task #13–#19):

> 2026-08-01 状态补丁：`c/` 已越过 proto 里程碑，进入 connection / stream 核心闭环实现。当前已具备 PacketIn 池化接收、PacketOut scatter/gather STREAM 发送、connection/context 基础建连、ACK/retransmission 基线、stream ring send buffer、PacketIn-backed recv fragment、连接级/流级 MAX_DATA 更新接收，以及 C 侧连接级/流级字节流控校验与应用消费后的 MAX_DATA / MAX_STREAM_DATA 排包。主动建连已支持 `timeout_ms` 驱动的握手期限、`retries` 驱动的新 CID 重试及握手期 close 的失败回调。发送侧已具备 Strict/DRR 多流调度，Context 固定模式，priority `0..7`，Strict 同级轮转与等待提升、DRR 权重量子/deficit 限制均已落地。路径验证已具备保守 active/candidate 双路径、候选地址 PATH_CHALLENGE/PATH_RESPONSE、单包目的地址、候选流量隔离、1500ms 三次重试和按候选地址的 `3*received + 3*MTU` 抗放大门控。Keepalive 已按默认 C++ 参数接入：活跃路径收包重置 30 秒空闲期，单帧 Ping 经 ACK 判活，1.5 秒间隔最多 3 次探测，超限本地中止。MTU 已接入 `PING + PADDING` 阶梯/二分探测、ACK/丢失/超时回灌和 Context 定时器；`mtu_max` 在 C 端可配置至 `65535`。UDP 在 Linux、Windows 与 macOS 均强制不分片；Darwin C11 严格模式须在包含 `<netinet/in.h>` 前定义 `__APPLE_USE_RFC_3542`，以暴露并使用 `IPV6_DONTFRAG`。单次 probe 丢失会退休旧 PacketOut、由状态机重新构造相同大小的新 probe；`mtu_probe_retries` 默认 `1`，本地 `EMSGSIZE`/`WSAEMSGSIZE` 不重试而立即收窄上界。黑洞会先把业务 MTU 降至 `mtu_min`，冷却后先验证 `mtu_base`：成功即恢复 base 并向上探测，最终失败仅在 `[mtu_min, mtu_base-1]` 二分。主动 `close` 不回调；对端 close 与本地传输异常经 `on_connection_error` 仅通知一次，对端关闭码 `0` 表示正常关闭，reason 使用回调期零拷贝视图。未来 MTU/路径探测仅在判定无可用路径并终止连接时触发此回调，单次探测丢失、MTU 降级和候选路径失败不触发。后续继续按 `docs/superpowers/requirements/` 的 03/04/05/06/09/11/12 补齐，不以 `doc/` 旧文档为准。

1. **wire 底座**:`src/internal/wire.h` 有界大端 read/write u8/u16/u32/u64(游标 + capacity 检查,溢出返 `INTERNAL_ERROR_OVERFLOW`)。
2. **包头 + 常量**:`src/internal/proto.h` + `src/proto.c`:头 encode/decode、包类型(含 RENDEZVOUS)、版本、packno 上限、MTU floor 1280。
3. **帧类型 + 包布局解析**:`src/internal/frame.h` + `src/frame.c`:帧枚举/位图/重传掩码;布局校验(frameLength 等价)+ 逐帧游标 + 位图;零分配。
4. **定长帧编解码**:Ping/Path*/HandshakeDone/HandshakeDelay/Max*/DataBlocked/ResetStream/Version/AckFrequency(normalize)/Crypto(reserved==0)。
5. **变长帧编解码**:Stream/Padding/ConnectionClose/SessionToken/TransportParams(限值校验)。
6. **Ack 帧**:largest/first_range/range_count/ack_delay(exp 位移)/附加 range;逐条不变量。
7. **fuzz + 接线 + 提交**:随机/差分解析 fuzz(≥10 万混合输入);接入 `c/CMakeLists.txt`;warnings-as-errors 绿;clang-format;分阶段提交。

**验收**:每帧 roundtrip + 类型不匹配/越界/不变量拒绝;整包铺满校验 + 未知帧拒绝;差分 fuzz 对照参考模型。

---

## 8. 未决 / 后续

- **crypto spec(独立)**:peer↔peer 身份、抗主动 MITM、显式 Finished/双向 key confirmation、`doc/全包加密与无状态可验证CID混淆方案.md` 的全包加密 + opaque CID(SipHash mask/tag)—— 均**未实现**,是目标架构。加密恢复 0-RTT 的两消息规则已在 `utp-10` §10 另行确定。
- **relay/TURN spec(独立)**:双对称 NAT、UDP 阻断兜底转发。
- **调参 TBD**:端口预测置信阈值。
- **NTRS 认证**:基于半连接专项冻结服务端认证、凭据保护和根密钥轮换协议。
- **清理项**(实现时顺带,index §5):AckFrequency 两套默认(10/3/150 vs 5/3/25)、`abortConnection` vs `close()` 收敛不一致、`IP_PKTINFO` 从未启用、stream_id u32(非 doc 的 u64)。

---

*本文由 NTRS 半连接规格、utp-00 index、utp-01、utp-10、c/STYLE.md、c/ERRORS.md、c/README.md 汇总冻结。后续决策变更须先改本表再改实现。*
