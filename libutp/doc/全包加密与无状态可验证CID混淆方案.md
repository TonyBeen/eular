# libutp 全包加密与无状态可验证 CID 混淆方案

> 更新时间：2026-04-09  
> 状态：设计文档，未落地到当前代码  
> 目标：为 libutp 增加“最小明文外层 + 内层全包 AEAD + 16 字节无状态可验证 CID”的数据面混淆方案，降低协议指纹暴露，避免按连接遍历试解密。

---

## 1. 文档目的

本文档定义一套可直接指导实现的混淆方案，解决以下问题：

1. 当前协议固定 20 字节明文头部容易暴露协议特征。
2. 仅对 payload 加密，SCID、DCID、Packet Number、Payload Length、Type 等字段仍可被 DPI 利用。
3. 如果把整包完全隐藏，又会遇到“收包后不知道属于哪个 Connection”的 demux 问题。
4. 在连接数量增多时，不能采用“遍历所有连接逐个尝试解密”的做法。

本文档给出的方案重点解决的是：

1. 如何在不暴露真实连接标识的前提下完成快速分流。
2. 如何让旧的 libutp 固定头不再裸奔。
3. 如何把握手期、1-RTT 数据期、CID 轮换和密钥轮换串成一套完整机制。

本文档不承诺以下目标：

1. 不承诺绝对绕过所有 QoS 或 DPI。
2. 不承诺在行为学识别、目的地址信誉、UDP 全局限速等策略下完全隐藏。
3. 不尝试伪装成完整的 QUIC、DTLS 或 HTTPS 语义。

---

## 2. 当前实现现状

当前仓库中的基础事实如下：

1. 现行协议头为固定 20 字节。
2. 头部字段包括：SCID、DCID、Packet Number、Payload Length、Types、Reserve。
3. 当前包级 AES-GCM 只保护 payload，头部仍保持明文。
4. 当前 Connection demux 依赖明文 CID 与固定头解析。
5. 当前本地/远端 CID 生成仍以 32 bit 为主，偏向“内部连接标识”，而非“对外 opaque token”。

因此，当前实现更接近：

1. 明文 uTP 固定头
2. 明文 demux
3. payload 机密性保护

而本文档拟引入的方案将调整为：

1. 最小明文外层用于 demux
2. 内层完整保护原 uTP 头与 Frames
3. 对外不再直接暴露 SCID/DCID 等固定字段

---

## 3. 设计目标与非目标

### 3.1 设计目标

1. 收到 UDP 包后，必须能在常数时间内定位到候选连接或候选 worker。
2. 不允许按 Connection 全量遍历并逐个试解密。
3. 真实 uTP 头和 Frames 在数据期内应由 AEAD 整体保护。
4. 对外暴露的连接标识必须是 opaque 的，不直接泄露真实连接关系。
5. 连接标识必须支持轮换，避免长期稳定关联。
6. 握手期与数据期应使用统一的分层包格式思想。
7. 保持与现有发送控制、ACK、重传、路径迁移机制兼容。

### 3.2 非目标

1. 不追求“UDP 负载 100% 全字节不可见”。
2. 不追求“完全 mimic 某现有协议”。
3. 不在 16 字节 CID 内同时塞入“全局唯一连接身份 + 完整业务语义 + 可长期稳定追踪的固定标识”。
4. 不把 Opaque CID 设计为替代整个 Connection 表。

---

## 4. 总体思路

### 4.1 分层原则

本方案将 UDP payload 拆成两层：

1. 外层：最小明文字段，用于快速 demux。
2. 内层：被 AEAD 保护的真实 libutp 包。

这里的“全包加密”指的是：

1. 原有固定 20 字节 libutp 头部
2. 原有 Frames
3. 可选内层 padding

三者作为一个整体进入 AEAD 明文区，由会话密钥加密并认证。

因此，本方案不是“连 demux 外层也完全隐藏”。
如果 demux 外层也完全隐藏，接收端就无法在连接数增大时高效确定解密上下文。

### 4.2 关键结论

1. 必须保留最小外层锚点。
2. 这个锚点不能是明文 DCID 复刻。
3. 它应是可验证、可轮换、对外不可读的 Opaque CID。
4. 真正的 uTP 头留在内层，由 AEAD 一起保护。

---

## 5. 新包格式

### 5.1 外层 UDP 负载格式

建议的数据期包格式如下：

```text
UDP Payload := OuterHeader || Ciphertext || Tag || OptionalPadding

OuterHeader := Flags(1B) || OpaqueCID(16B) || TruncatedPN(2B)
Ciphertext  := AEAD_Encrypt(InnerPacket, aad=OuterHeader)
Tag         := 16B
OptionalPadding := 0..N bytes
```

默认推荐：

1. Flags：1 字节
2. Opaque CID：16 字节
3. Truncated Packet Number：2 字节
4. AEAD Tag：16 字节

则最小外层开销为：

1. 1 + 16 + 2 + 16 = 35 字节

### 5.2 Flags 布局建议

Flags 建议只承载最少的解析信息：

```text
bit 7..6 : outer_version       2 bits
bit 5    : handshake_packet    1 bit
bit 4    : key_phase           1 bit
bit 3..2 : pn_len_code         2 bits
bit 1..0 : grease/random       2 bits
```

说明：

1. outer_version 用于包格式升级。
2. handshake_packet 表示内层明文语义属于握手空间还是 1-RTT 数据空间。
3. key_phase 允许当前密钥与上一代密钥共存验证。
4. pn_len_code 当前建议固定为 2 字节截断 PN，但预留扩展位。
5. grease/random 用于减少 flags 固定值特征。

### 5.3 InnerPacket 定义

InnerPacket 即当前固定 20 字节 libutp 头加原有帧集合：

```text
InnerPacket := UTPHeaderProto(20B) || Frames || InnerPadding(optional)
```

内层保留现有语义：

1. SCID
2. DCID
3. Packet Number
4. Payload Length
5. Types
6. Reserve
7. Frames

区别仅在于：

1. 这些字段不再作为外层明文字段出现。
2. 它们在 1-RTT 数据期统一进入 AEAD 明文区。

---

## 6. Opaque CID 的职责边界

### 6.1 它解决什么问题

Opaque CID 只解决一件核心事情：

1. 在不暴露真实连接身份的前提下，把包快速分流到正确的处理上下文。

它不负责：

1. 替代整个 Connection 状态表。
2. 单独承载全部连接身份信息。
3. 直接还原内层真实 SCID/DCID。

### 6.2 为什么不能直接用固定 DCID

固定 DCID 的问题在于：

1. 值长期稳定，容易被跨时间关联。
2. 位置固定，很容易被 DPI 直接规则匹配。
3. 即使再加 4 字节随机数，也只是形成新的固定头模板。
4. 若 DCID 长期不变，就只是把“uTP 明文头”换了个位置，不能从根本上解决问题。

Opaque CID 的改进点在于：

1. 对外不可读。
2. 可以轮换。
3. 仅暴露分流所需最小信息。
4. 无法从中直接读出真实连接 ID。

---

## 7. 16 字节无状态可验证 CID 设计

### 7.1 定义

“无状态可验证 CID”指：

1. 接收端拿到 CID 后，不需要先查一张“签发表”才能判断它是否合法。
2. 只靠本地服务端密钥和 CID 自身，就能验证它是不是本端签发的。
3. 验证通过后，可以从中恢复少量路由信息。
4. 再利用这些路由信息，把包交给正确的 worker 或连接分片。

它并不是“完全不需要连接状态”。
它只是把“是否为本端签发、应交给哪个分片”这一步做成了无状态。

### 7.2 16 字节结构

推荐结构：

```text
OpaqueCID := salt(2B) || body_masked(6B) || tag(8B)
```

总计 16 字节，128 bit。

字段含义：

1. salt：16 bit，公开随机数。
2. body_masked：48 bit，被掩码后的路由信息。
3. tag：64 bit，完整性校验值。

### 7.3 body_plain 的位分配

body_plain 仅放最必要的路由元数据，但在 16 字节预算下可以额外放一个分片内路由标签，建议如下：

```text
body_plain (48 bits)

bit 47..38 : worker_id     10 bits
bit 37..24 : issue_epoch   14 bits
bit 23..8  : route_tag     16 bits
bit 7..0   : cid_flags      8 bits
```

字段说明：

1. worker_id
   1. 指示包应进入哪个 worker、分片或连接桶。
   2. 10 bit 最多支持 1024 个分片。

2. issue_epoch
   1. 表示该 CID 签发时所属时间片。
   2. 用于过期判断与轮换窗口判断。
   3. 14 bit 可表示 16384 个时间片。

3. route_tag
   1. 表示 worker 内的分片路由标签，不直接等于真实 Connection ID。
   2. 可用于把候选范围从“整个 worker 的连接表”进一步缩小到一个桶或一小组候选连接。
   3. 该字段允许与当前连接状态表建立短生命周期映射，但对外不可读。

4. cid_flags
   1. 用于格式版本、路径类别、保留位等。
   2. 不放真实连接业务语义。

### 7.4 密钥派生

服务端需维护一个长期 master secret，并派生两把 CID 专用密钥：

```text
K_cid_mask = HKDF(master_secret, "cid-mask")
K_cid_tag  = HKDF(master_secret, "cid-tag")
```

要求：

1. mask 与 tag 使用独立派生标签。
2. 不要直接复用包加密密钥。
3. master secret 需要支持轮换，并保留上一代用于宽限验证。

### 7.5 生成算法

设：

1. `worker_id` 为该连接归属的 worker 或分片号。
2. `issue_epoch` 为当前时间片编号，例如 `floor(now / 60s) mod 16384`。
3. `route_tag` 为该连接当前的分片内路由标签，可随机生成并在 CID 生命周期内有效。
4. `cid_flags` 为版本和预留标志。
5. `salt` 为 16 bit 随机值。

生成步骤：

1. 组装明文路由体：

```text
body_plain = (worker_id << 38) | (issue_epoch << 24) | (route_tag << 8) | cid_flags
```

2. 计算 48 bit 掩码：

```text
mask48 = Trunc48(SipHash(K_cid_mask, salt))
```

3. 得到被掩码路由体：

```text
body_masked = body_plain XOR mask48
```

4. 计算 64 bit 校验标签：

```text
tag64 = Trunc64(SipHash(K_cid_tag, salt || body_masked))
```

5. 输出 CID：

```text
OpaqueCID = salt || body_masked || tag64
```

### 7.6 验证算法

收到 16 字节 Opaque CID 后，按以下步骤验证：

1. 拆出：
   1. `salt`
   2. `body_masked`
   3. `tag`

2. 先使用当前代密钥验证：

```text
expected = Trunc64(SipHash(K_cid_tag_current, salt || body_masked))
```

3. 若不匹配，再使用上一代密钥验证。

4. 如果当前代与上一代都不匹配，则该包直接丢弃。

5. 若匹配，按对应密钥恢复 body_plain：

```text
mask48 = Trunc48(SipHash(K_cid_mask_matched, salt))
body_plain = body_masked XOR mask48
```

6. 解析出：
   1. `worker_id`
   2. `issue_epoch`
   3. `route_tag`
   4. `cid_flags`

7. 判断 `issue_epoch` 是否在可接受时间窗口内。

8. 验证通过后，将包路由至 `worker_id` 对应的处理分片。

### 7.7 为什么这是“无状态可验证”

因为在上面的验证过程中：

1. 不需要事先保存 `OpaqueCID -> worker` 的映射表。
2. 也不需要保存“某个随机 token 是否曾经下发过”。
3. 只要本地持有 `K_cid_mask` 与 `K_cid_tag`，就能验证并恢复路由信息。

这里的“无状态”只针对：

1. CID 合法性验证
2. 路由信息恢复

并不代表：

1. 连接表可以删除
2. 包可直接跳过 Connection 状态机处理

### 7.8 64 bit tag 的取舍

在 16 字节预算下，64 bit tag 是一个更稳妥的工程折中。

优点：

1. 伪造难度比 48 bit 进一步提高。
2. 能为 body 留出完整 48 bit 路由位图。
3. 可以在不过度放大头部开销的前提下，提高主动攻击下的稳健性。

局限：

1. 它仍然不是长期全局唯一身份，不应被当作永久主键。
2. 如果未来需要跨集群编码更复杂的路由信息，仍可能需要更长 CID 或额外握手协商。

---

## 8. 为什么 16 字节 CID 仍不直接编码具体 Connection

这是本设计最容易误解的地方。

16 字节空间有 128 bit，但仍要同时满足：

1. 外部看起来像随机值。
2. 内部可恢复路由线索。
3. 有防伪造能力。
4. 可轮换。

在这个长度预算下，虽然已经比 12 字节宽松很多，但仍不建议把“真实 Connection ID + 永久稳定身份 + 完整路由语义”直接裸编码进去。

因此本方案的正确职责分层是：

1. Opaque CID 负责把包稳定路由到正确 worker 或连接桶。
2. worker 内部再用本地 Connection 表做精确 demux。
3. 精确 demux 仍然依赖会话状态，例如：
   1. 当前活跃连接集合
   2. 当前与上一代 Opaque CID 绑定
   3. 当前与上一代 key phase
   4. PN 重建窗口
   5. `route_tag` 到具体连接对象的短生命周期映射

这样做的好处是：

1. 外层不需要暴露真实 DCID。
2. 无需全量遍历连接。
3. 只在候选分片内进行极小候选集验证。

---

## 9. Connection 级 Opaque CID 管理

### 9.1 每连接维护的外层标识

每个连接建议维护以下状态：

1. `cid_current`
2. `cid_previous`
3. `cid_next`（可选）

用途：

1. `cid_current` 用于当前发包。
2. `cid_previous` 用于接收在途旧包。
3. `cid_next` 用于平滑预切换。

### 9.2 轮换策略

建议两种触发条件并存：

1. 时间触发：每 30 到 120 秒随机抖动轮换一次。
2. 包量触发：每发送 K 个包后轮换一次，可选默认 2^15 或 2^16 量级。

轮换步骤：

1. 生成新 `cid_current`。
2. 旧 `cid_current` 下沉为 `cid_previous`。
3. 原 `cid_previous` 在宽限期到期后回收。

### 9.3 接收宽限窗口

为兼容乱序、重传和网络抖动，建议保留上一代 CID 一段时间。

推荐窗口：

1. `max(2 * smoothed_rtt, 3s)`
2. 上限可设为 10s

如果连接正处于路径迁移或显著乱序期，可适当延长到 15s。

---

## 10. 密钥体系

### 10.1 密钥类型

建议区分四类密钥：

1. `K_cid_mask`：CID 掩码密钥
2. `K_cid_tag`：CID 校验密钥
3. `K_pkt_send`：数据面 AEAD 发送密钥
4. `K_pkt_recv`：数据面 AEAD 接收密钥

### 10.2 派生来源

握手完成后，双方从握手主秘密派生双向数据面密钥：

```text
K_pkt_c2s = HKDF(handshake_secret, "pkt-c2s")
K_pkt_s2c = HKDF(handshake_secret, "pkt-s2c")
```

客户端：

1. 发送使用 `K_pkt_c2s`
2. 接收使用 `K_pkt_s2c`

服务端相反。

### 10.3 key phase

为支持平滑更新数据面密钥，建议引入 key phase 位：

1. 当前 phase
2. 上一 phase

接收端最多尝试：

1. 当前 phase 的接收密钥
2. 上一 phase 的接收密钥

这是常数级开销，不会随着连接数增长。

---

## 11. Nonce 与 Packet Number

### 11.1 Nonce 原则

数据面 AEAD 必须满足：

1. 同一方向同一密钥下 nonce 唯一。
2. 不能复用。

建议使用：

```text
nonce = nonce_prefix(4B) || full_packet_number(8B)
```

其中：

1. `nonce_prefix` 由握手密钥派生。
2. `full_packet_number` 使用完整 64 bit 包号。

### 11.2 为什么外层只放截断 PN

外层只放 2 字节截断 PN 的理由：

1. 降低外层显式模式长度。
2. 利于节省字节开销。
3. 接收端在获得候选连接后可基于已知最大包号恢复完整 PN。

恢复方式参考 QUIC 的 PN reconstruction 思想：

1. 已知 largest_received_pn
2. 已知 truncated_pn
3. 在最接近 largest_received_pn 的窗口中恢复 full_pn

### 11.3 为什么外层 PN 不能完全去掉

因为数据面 AEAD 需要 nonce，而 nonce 恰好依赖 full PN。
如果外层完全不给 PN，接收端在解密前就无法恢复 nonce。

因此：

1. 外层至少要暴露一个短 PN 片段。
2. 该片段本身不泄露真实 uTP 头语义。
3. 它只是解密辅助字段，不等于明文旧头部。

---

## 12. AEAD 保护范围

### 12.1 AAD 与明文区

建议：

1. OuterHeader 作为 AAD。
2. InnerPacket 作为 AEAD 明文区。

即：

```text
ciphertext, tag = AEAD_Encrypt(
    key = K_pkt_send,
    nonce = build_nonce(full_pn),
    aad = OuterHeader,
    plaintext = InnerPacket
)
```

好处：

1. 外层字段虽然明文，但受完整性绑定。
2. 攻击者篡改外层的 flags、CID、truncated_pn 会导致解密失败。
3. 内层真实头和帧获得机密性与完整性。

### 12.2 AEAD 算法建议

推荐优先级：

1. ChaCha20-Poly1305
2. AES-128-GCM
3. AES-256-GCM

若考虑当前仓库已存在 AES-GCM 实现，第一阶段可先沿用 AES-GCM 完成架构改造。
后续如需更好的非 AES 硬件环境表现，可再抽象为 AEAD 接口并增加 ChaCha20-Poly1305。

---

## 13. 收包路径

### 13.1 总体流程

收到一个 UDP 包后的处理流程如下：

1. 解析 OuterHeader。
2. 校验 outer_version、pn_len_code 等基本格式。
3. 验证 Opaque CID，恢复 `worker_id`、`issue_epoch`、`route_tag`、`cid_flags`。
4. 把包路由到对应 worker 或连接桶。
5. 在该 worker 内，根据 `cid_current` / `cid_previous` 命中候选连接。
6. 基于候选连接恢复 full PN。
7. 用当前 key phase 的接收密钥进行 AEAD 解密。
8. 若失败，再用上一 key phase 的接收密钥尝试一次。
9. 解密成功后得到 InnerPacket。
10. 再按现有 libutp 解析流程处理内层 UTPHeaderProto 与 Frames。

### 13.2 为什么不是遍历所有连接

因为有两层收敛：

1. 第一层：Opaque CID 无状态验证后直接收敛到 worker 或连接分片。
2. 第二层：在该分片内只检查极少数候选连接，例如：
   1. 当前 cid 对应连接
   2. 上一代 cid 对应连接
   3. 当前和上一 key phase

因此解密尝试次数是常数级，而不是和连接数线性相关。

### 13.3 未命中处理

如果 Opaque CID 验证失败：

1. 直接丢弃。

如果 Opaque CID 合法，但该 worker 内找不到对应候选连接：

1. 若 `handshake_packet=1`，进入握手空间处理。
2. 若是数据期包，直接丢弃并记统计。

---

## 14. 发包路径

### 14.1 数据期发包

发送一个数据期包时，流程如下：

1. 生成或复用当前 `cid_current`。
2. 分配新的 full PN。
3. 根据 full PN 构造 nonce。
4. 构造 InnerPacket：
   1. 内层 20 字节 UTP 头
   2. Frames
   3. 可选内层 padding
5. 生成 OuterHeader：
   1. flags
   2. Opaque CID
   3. truncated PN
6. 以 OuterHeader 为 AAD，对 InnerPacket 做 AEAD 加密。
7. 拼接为 UDP payload 发送。

### 14.2 重传包

重传包必须保持以下约束：

1. full PN 不复用旧值，而应分配新的数据面 PN。
2. nonce 随新的 PN 自动变化。
3. 内层语义可以表示对同一逻辑数据的重传。

这与当前 libutp“新包号重传”方向一致，不应逆向退回“重传复用原 PN”。

---

## 15. 握手期设计

### 15.1 问题边界

在无 PSK 场景下，握手首包通常不能做到“从第一个字节起全部保密”。
原因是接收端还没有会话密钥，无法直接解开一个完全密文的首包。

因此握手期建议分成两段：

1. 初始握手空间：最小可解析外层 + 握手载荷
2. 数据空间：握手完成后切换到内层全包 AEAD

### 15.2 初始握手包建议

无 PSK 场景下，握手首包建议格式仍复用同一外层，但 `handshake_packet=1`：

```text
OuterHeader := Flags || InitialCID || TruncatedPN
HandshakePayload := X25519 临时公钥 || 握手随机数 || token/cookie || 握手帧
```

说明：

1. `InitialCID` 可继续使用同样的 16 字节无状态可验证结构。
2. 其主要用途是快速路由和抗滥用控制。
3. 握手首包的载荷机密性可以较弱，但要尽快协商出会话密钥。

### 15.3 握手期间无法加密时如何处理

当握手双方尚未得到共享会话密钥时，不应强行追求“首包全密文”。更稳妥的做法是采用“最小明文外层 + 最小明文握手载荷 + 快速切换到 1-RTT”的策略。

建议规则如下：

1. 只有 OuterHeader 始终保持明文，用于格式识别、分流和抗滥用控制。
2. 握手载荷只保留建立密钥所必需的信息，典型包括：
   1. X25519 临时公钥
   2. 握手随机数
   3. 协议版本和能力协商位
   4. 地址验证 token 或 cookie
3. 除上述必需项外，不要在握手首包携带可选业务数据。
4. 收到合法握手响应后，必须尽快派生 `handshake_secret` 与 1-RTT 数据面密钥。
5. 一旦 1-RTT 密钥可用，后续包立即切换到“OuterHeader 明文 + InnerPacket AEAD”的数据期格式。

这意味着握手期的目标不是“保密所有内容”，而是：

1. 不暴露固定的 libutp 20 字节头。
2. 不暴露稳定的真实连接标识。
3. 把明文字段压缩到建链所必需的最小集合。

### 15.4 握手期间 InitialCID 如何生成

握手期的 `InitialCID` 与数据期的 `OpaqueCID` 使用同一种 16 字节结构，但其字段语义应更偏向“无状态接入控制”，而不是“已建立连接的精确 demux”。

建议握手期的 `body_plain` 使用如下语义：

```text
initial_body_plain (48 bits)

bit 47..38 : worker_id        10 bits
bit 37..24 : issue_epoch      14 bits
bit 23..8  : retry_token_id   16 bits
bit 7..0   : cid_flags         8 bits
```

字段建议：

1. `worker_id`
   1. 表示首包应交给哪个 worker 或监听分片。
2. `issue_epoch`
   1. 用于时间窗验证与密钥轮换。
3. `retry_token_id`
   1. 表示本次握手的短生命周期接入标签。
   2. 可对应一枚无状态 token、cookie 编号，或一个本地短窗口 lookup 键。
   3. 它不是正式连接 ID，也不是后续数据期的 `route_tag`。
4. `cid_flags`
   1. 用于声明该 CID 处于初始握手空间、协议版本或是否带 token。

生成步骤与数据期一致，只是 `body_plain` 的字段含义不同：

1. 选择 `worker_id`。
2. 生成当前 `issue_epoch`。
3. 生成 `retry_token_id`：
   1. 若服务端采用完全无状态接入控制，可由 token 校验逻辑派生一个短标签。
   2. 若服务端允许短生命周期 pending 表，也可随机生成并在很短窗口内缓存。
4. 组装 `initial_body_plain`。
5. 计算 `mask48 = Trunc48(SipHash(K_cid_mask, salt))`。
6. 计算 `body_masked = initial_body_plain XOR mask48`。
7. 计算 `tag64 = Trunc64(SipHash(K_cid_tag, salt || body_masked))`。
8. 输出 `InitialCID = salt || body_masked || tag64`。

服务端收到首包后：

1. 先验证 `InitialCID` 的 tag 与时间窗。
2. 恢复 `worker_id`、`issue_epoch`、`retry_token_id`、`cid_flags`。
3. 把包交给对应 worker 的握手空间处理。
4. 再结合 token/cookie、源地址和握手公钥做进一步接入判断。

这样做的目的不是在握手首包就精确命中某个已存在 Connection，而是：

1. 保证首包可快速路由。
2. 防止暴露稳定明文 DCID。
3. 为后续 Retry、HelloVerify 或 pending 建链流程提供一个短生命周期 opaque 入口。

### 15.5 0-RTT 无法加密时如何处理

0-RTT 分两种情况，必须区分处理：

1. 有可恢复的 early secret
   1. 例如客户端持有有效 session ticket、PSK 或恢复态。
   2. 此时 0-RTT 数据应使用 early data key 加密，而不是明文发送。
   3. 外层仍然使用明文 OuterHeader，内层则使用 `K_early` 对 0-RTT InnerPacket 做 AEAD。

2. 没有可恢复的 early secret
   1. 这种场景本质上不应发送真正的 0-RTT 业务数据。
   2. 只能发送“握手辅助信息”，不能发送需要保密的应用数据。
   3. 更稳妥的策略是把所谓 0-RTT 降级为“握手期首包附带最少控制信息”，待 1-RTT 密钥建立后再发业务数据。

建议约束：

1. 没有 PSK、ticket 或恢复态时，禁止应用数据明文 0-RTT。
2. 如果业务上强行要求“未建密钥先发数据”，必须把这些数据视为公开信息，而不是混淆层的一部分安全承诺。
3. 0-RTT 一旦采用 early data key，必须配套 anti-replay 机制，例如：
   1. ticket 过期时间
   2. `(ticket_id, packet_number)` 去重窗口
   3. 单连接或单票据的接受上限

### 15.6 握手完成后的切换点

一旦双方完成 X25519 并派生出数据面密钥：

1. 后续包立刻切换到数据期格式。
2. 原固定 uTP 头不再明文发送。
3. 真实内层 SCID/DCID 仅保留在 AEAD 明文区。

---

## 16. 与现有 libutp 模块的映射

### 16.1 需要新增或调整的模块

建议新增：

1. `src/obfs/opaque_cid.h/.cpp`
   1. CID 生成
   2. CID 验证
   3. CID 轮换辅助

2. `src/crypto/aead_packet_context.*`
   1. 统一 AEAD 接口
   2. 支持“OuterHeader 作为 AAD，InnerPacket 作为明文区”

3. `src/proto/outer_header.h/.cpp`
   1. 外层头定义
   2. 编解码

建议调整：

1. `src/proto/proto.h`
   1. 保留内层旧头定义
   2. 不再假定网络上直接发送该头

2. `src/crypto/aes_gcm_context.cpp`
   1. 从“只加密 payload”扩展为“可加密整个 InnerPacket”

3. `src/context/*` 与 `src/socket/*`
   1. 收包入口先解析 OuterHeader
   2. 再路由至连接上下文

4. `src/util/util.*`
   1. 现有 32 bit CID 生成逻辑保留给内部兼容用途
   2. 新增 Opaque CID 生成接口，避免语义混用

### 16.2 建议的连接状态新增字段

每个连接建议增加：

1. `outer_cid_current[16]`
2. `outer_cid_previous[16]`
3. `outer_cid_expire_at`
4. `recv_largest_pn`
5. `key_phase_current`
6. `key_phase_previous`
7. `aead_send_ctx`
8. `aead_recv_ctx`

---

## 17. 兼容性与迁移策略

### 17.1 协议版本策略

建议引入独立的 `outer_version`，不要直接复用当前 `UTP_PROTOCOL_VERSION`。

原因：

1. 外层包格式变化不等于内层 uTP 语义变化。
2. 便于灰度启用混淆层，而不影响内层协议演进。

### 17.2 加密开关与调试模式

当前代码已经存在“加密”和“非加密”两种运行模式，其中非加密主要用于调试。对于本文档的新设计，不建议只保留“开或关”两个粗粒度选项，而应显式区分三种模式：

1. `legacy_plaintext`
   1. 继续沿用当前老路径：网络上直接发送明文 uTP 固定头。
   2. 不使用 OuterHeader，不使用 Opaque CID，不使用 InnerPacket 封装。
   3. 这是最容易抓包、最容易定位协议问题的调试模式。

2. `obfs_debug_plaintext`
   1. 使用新的 OuterHeader 与 Opaque CID。
   2. 仍走新的 demux 和外层分流路径。
   3. InnerPacket 不做 AEAD 加密，直接明文承载，必要时可附带一个轻量校验值用于快速发现格式破坏。
   4. 这个模式只用于调试“新包格式、CID、收包分流、PN 恢复、路径切换”，不用于生产。

3. `protected`
   1. 使用新的 OuterHeader 与 Opaque CID。
   2. InnerPacket 走完整 AEAD。
   3. 这是生产模式。

推荐默认策略：

1. 对已有用户和现有调试工具，保留 `legacy_plaintext`。
2. 对新设计的联调和问题定位，再增加 `obfs_debug_plaintext`。
3. 生产环境只允许 `protected`。

### 17.3 非加密模式下 Opaque CID 如何处理

Opaque CID 的生成与验证不依赖数据面 AEAD，因此即使关闭 InnerPacket 加密，也仍然可以使用 Opaque CID。

也就是说，下面两件事应当解耦：

1. 是否使用 Opaque CID 做外层分流。
2. 是否对 InnerPacket 做 AEAD。

因此在 `obfs_debug_plaintext` 模式下建议如下：

1. OuterHeader 保持不变：`Flags + OpaqueCID + TruncatedPN`。
2. Opaque CID 仍按本文档的 16 字节无状态可验证规则生成。
3. 只是不对 InnerPacket 执行 AEAD，而是直接发送：

```text
UDP Payload := OuterHeader || InnerPacket || OptionalDebugChecksum || OptionalPadding
```

4. 接收端仍先解析 OuterHeader、验证 Opaque CID、恢复 worker 路由，再直接解析 InnerPacket。

这样做的好处是：

1. 调试时可以验证新 demux 设计是否正确。
2. 可以抓到明文 InnerPacket，便于比对老协议字段。
3. 不会因为关闭加密就退回旧的连接分流模型。

### 17.4 为什么不建议“非加密也强行模拟 AEAD”

如果为了调试把 AEAD 关闭后仍伪造一个看似完整的加密包，通常会带来两个问题：

1. 实现复杂度上升，但没有真实安全收益。
2. 抓包时反而看不到明文 InnerPacket，降低调试价值。

因此对调试模式更好的策略是：

1. 要么完全使用 `legacy_plaintext`，走老路径。
2. 要么使用 `obfs_debug_plaintext`，只验证新外层和 demux，不做 AEAD。

### 17.5 双栈兼容期

建议支持一段时间的双栈：

1. 旧路径：明文 uTP 头 + payload AEAD
2. 新路径：OuterHeader + InnerPacket AEAD

握手协商时可通过握手参数声明：

1. 是否支持 outer obfs
2. 支持的 AEAD 算法
3. 是否支持 CID rotation

如果需要显式协商运行模式，可增加一个模式枚举，例如：

1. `mode=legacy_plaintext`
2. `mode=obfs_debug_plaintext`
3. `mode=protected`

其中：

1. `legacy_plaintext` 只用于本地调试与兼容旧节点。
2. `obfs_debug_plaintext` 只用于调试新分层结构。
3. `protected` 才是对外发布的正式模式。

### 17.6 灰度顺序

建议分三阶段上线：

1. 阶段一：先引入 `obfs_debug_plaintext`，验证 OuterHeader、Opaque CID、PN 恢复和收包 demux。
2. 阶段二：把现有固定 20 字节头并入 AEAD 明文区，落地 `protected` 模式。
3. 阶段三：加入 CID 轮换、key phase 轮换、padding 策略和统计观测。

---

## 18. Padding 与流量整形

### 18.1 目的

即使头部与 payload 都被保护，DPI 仍可利用以下特征：

1. 包长分布
2. 发送突发节奏
3. 上下行比例
4. 握手后首批数据的典型尺寸

因此建议配合轻量级 padding：

1. 内层 padding：作为 InnerPacket 的一部分被加密。
2. 外层 padding：作为纯噪声追加在 tag 后，是否保留取决于实现复杂度。

优先推荐内层 padding，因为它同时具备机密性和完整性保护。

### 18.2 不建议做的事

1. 不建议为了“看起来像别的协议”而硬编码固定魔数。
2. 不建议固定 padding 模板。
3. 不建议让每个包都填充到同一长度，这会形成新的明显模式。

---

## 19. 安全性分析

### 19.1 该方案能提升什么

1. 固定 20 字节 uTP 头不再明文可见。
2. SCID、DCID、Type、Payload Length 等字段不再直接暴露。
3. Opaque CID 不泄露真实连接身份。
4. Opaque CID 支持轮换，降低长期关联能力。
5. 外层明文字段通过 AAD 获得篡改保护。

### 19.2 仍然暴露什么

1. UDP 五元组
2. 包长
3. 发送时序
4. 收发方向比
5. 会话持续时间

因此，本方案属于：

1. 显著降低协议指纹暴露
2. 提高识别成本
3. 但不能保证完全不可识别

### 19.3 为什么比“Header XOR + Payload 加密”更可靠

因为 XOR 头部存在以下问题：

1. 线性变换，长期统计上更容易被分析。
2. 缺少完整性保护。
3. 若掩码复用，容易出现模式泄露。
4. 对主动篡改非常脆弱。

AEAD 方案则同时提供：

1. 机密性
2. 完整性
3. 每包唯一 nonce 带来的去模式化能力

---

## 20. 风险与工程注意事项

### 20.1 不要把 Opaque CID 和真实连接 ID 混用

Opaque CID 是对外 token，不是内部连接主键。
内部连接仍应保留稳定的上下文标识。

### 20.2 不要让同一连接永久使用同一个 Opaque CID

如果 Opaque CID 不轮换，它最终仍会退化为“固定 DCID 的另一种写法”。

### 20.3 不要复用 nonce

AEAD 下 nonce 复用会直接破坏安全性。
必须坚持“每方向、每密钥、每包号唯一”。

### 20.4 不要在收包路径按连接全量试解密

如果设计最终又退回“遍历所有连接逐个试 AEAD”，说明外层 demux 设计失败。

### 20.5 不要把握手期问题转嫁到数据期方案里

无 PSK 场景下，首包不可能凭空完全密文。
应接受“握手期最小可解析外层 + 快速派生会话密钥”的现实边界。

---

## 21. 推荐默认参数

建议默认值如下：

1. Opaque CID 长度：16 字节
2. Opaque CID salt：2 字节
3. Opaque CID body：6 字节
4. Opaque CID tag：8 字节
5. truncated PN：2 字节
6. AEAD tag：16 字节
7. CID 轮换周期：30 到 120 秒随机抖动
8. CID 宽限窗口：`max(2 * RTT, 3s)`，上限 10s
9. key phase 宽限：当前 + 上一代
10. issue_epoch 时间粒度：60 秒

---

## 22. 实施建议

### 22.1 第一阶段

先完成架构改造，不追求一次性做完所有对抗特性：

1. 增加 OuterHeader 编解码。
2. 增加 Opaque CID 生成与验证。
3. 增加收包路径中的“先外层 demux，再内层解密”。
4. 将当前固定 20 字节头整体并入 AEAD 明文区。

### 22.2 第二阶段

补齐轮换与观测：

1. cid_current / cid_previous 管理
2. key phase 管理
3. unknown CID、tag failure、phase fallback 等指标

### 22.3 第三阶段

补齐流量特征弱化：

1. 内层 padding
2. 发送节奏抖动
3. 包长分布整形

---

## 23. 总结

本方案的核心不是“把旧 uTP 头挪到别处”，而是把协议拆成两层：

1. 外层只保留最小、可验证、可轮换、不可读的分流标识。
2. 内层承载真实 libutp 包，并由 AEAD 整体保护。

16 字节无状态可验证 CID 的正确定位是：

1. 它是一个对外 opaque 的路由令牌。
2. 它负责在常数时间内把包收敛到正确 worker 或连接桶。
3. 它不取代连接状态表，也不应承担全部连接身份语义。

只要坚持这三个边界：

1. 最小明文外层
2. 内层全包 AEAD
3. Opaque CID 轮换与无状态验证

就可以在不遍历所有连接的前提下，把当前“固定 20 字节明文头 + payload 加密”的模型升级为更稳妥的混淆数据面。