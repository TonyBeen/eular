# 全包轻量混淆与 Padding 方案

> 状态：协议设计草案，当前代码尚未实现。
>
> 适用范围：加密和非加密的 libutp、Rendezvous 和 NTRS 报文。`NAT_PROBE` 保持明文。
>
> 本方案是 wire 编码层的轻量混淆，不是加密、认证或 DPI 绕过承诺。

## 1. 目标

当前报文头和非加密 payload 中存在稳定特征：

- 固定 20 字节包头会暴露 SCID、DCID、Packet Number、Payload Length 和 Packet Type 的位置。
- 非加密连接中，帧类型和固定帧字段仍是明文。
- 只混淆包头会把特征转移到 payload，不能解决整体指纹问题。
- 固定长度的 ACK-only、PING 和其他 CTRL 包仍容易形成包长特征。

因此本方案要求：

1. 对整个 UDP payload 进行连续 XOR，覆盖固定包头、帧头、帧内容、STREAM 数据、AEAD Tag 和 Padding。
2. 加密与混淆保持独立；`CRYPTO` 帧只负责现有密钥交换。
3. 不增加外层头，复用解码前固定偏移的 `Reserve` 字节完成启动。
4. 不依赖 Connection 查找即可恢复包头，Initial、Handshake、0-RTT、CTRL 和 Rendezvous 报文使用同一规则；动态表的切换必须有握手包确认。
5. CTRL 包在不挤占业务数据的前提下增加最多 32 字节的伪随机 Padding，打散小包长度特征。

## 2. 边界与非目标

轻量混淆不提供密码学安全性：

- 内置的 4 组种子（索引 `0..3`）是协议常量，不是秘密密钥。
- 了解实现或提取二进制常量的对手可以恢复明文。
- 非加密模式不提供机密性、完整性或身份认证。
- 本方案只用于消除简单的固定偏移、固定帧值和重复包长特征，不伪装 QUIC、DTLS、DNS 或其他协议。

需要真正的机密性和完整性时，仍必须开启现有 AEAD。

## 3. 线路格式

混淆不改变包长，也不引入新的外层字段：

```text
CanonicalPacket := Header(20B) || Payload
WirePacket      := XOR-Obfuscate(CanonicalPacket)
```

`CanonicalPacket` 仍使用现有包头：

```text
SCID(4) | DCID(4) | PacketNumber(8) | PayloadLength(2) | Type(1) | Reserve(1)
```

关键约束：

- canonical `Reserve` 始终为 0。
- `WirePacket[19]` 不是明文 Reserve，而是选择启动或连接级混淆种子的 `selector`。
- 除 UDP/IP 头和明文 `NAT_PROBE` 外，没有未经混淆的 libutp 包头或帧字节。
- 接收端只是在固定偏移预读 selector，然后将整个数据报交给同一个连续 XOR 转换；不是先解头、再解 payload 的两段协议。

## 4. selector

### 4.1 生成

发送端在确定当前发送尝试的目的 endpoint 和 Packet Number 后生成 selector：

```text
endpoint_bytes = family | address | port | IPv6 scope_id
hash = HASH64(endpoint_bytes || PacketNumber || "utp-wire-selector-v1")
selector = hash mod 4                          // 内置表阶段
selector = 4 + (hash mod 252)                  // 动态表阶段
```

`HASH` 必须作为 wire protocol 的一部分固定，不能使用平台 `rand()`、容器哈希或编译器相关实现。它不负责安全性，只需在所有平台产生完全一致的 8 bit 结果。

这里的 endpoint 是发送时的对端目的地址，不是 Context bind 地址。不要试图让接收端重算 selector：

- NAT 前后发送端的目的 endpoint 与接收端看到的来源 endpoint 不一致。
- 路径迁移、对称 NAT 和多本地地址会使双方观察的地址继续变化。
- selector 的职责只是让接收端在查找 Connection 前选中同一组掩码。

### 4.2 校验

接收端从 `WirePacket[19]` 读出 selector 后，只校验反混淆得到的 canonical 包头：

1. 验证 canonical `Reserve == 0`。
2. 验证 `PayloadLength == UDP payload length - 20`。
3. 验证 Type、Packet Number 和已有协议约束。

反混淆完全依赖 wire 中携带的 selector，不可因 endpoint 不一致而拒绝合法报文。上述校验仍不是完整性认证。

## 5. 全包 XOR 与种子表

### 5.1 种子表

协议内置 4 组固定种子表，编号为 `0..3`，用于无状态启动和握手阶段：

```text
seed = builtin_seed_table[selector]       // selector 为 0..3
```

种子表必须：

- 由离线工具一次生成后以常量形式提交。
- 成为协议版本的一部分，所有 libutp 端和 `ntrs` 使用完全相同的 4 组顺序和数值。
- 不在运行时修改，不被当作用户密钥或安全配置。

此表与 NAT 探测协议无关：`NAT_PROBE` 不选种子、不执行 XOR，`ntrs_natc` 也不需要链接或加载此表。`libutp` 与 `ntrs` 只共用这份启动表；连接建立后的动态表属于单条连接，不是 NTRS 全局配置。

### 5.2 掩码规则

掩码是无状态的。给定 selector 和字节偏移，它的输出唯一确定，不读取前一个字节、不保存滚动状态。

```text
for i = 0 .. WireLength - 1:
    mask = EXPAND_BYTE(active_seed_table[selector], i)
    if i == 19:
        mask = selector

    wire_byte = canonical_byte XOR mask
```

因为 canonical `Reserve == 0`，所以：

```text
WirePacket[19] = 0 XOR selector = selector
```

接收端先保存 `WirePacket[19]`，然后对整个报文使用同一组掩码：

```text
selector = WirePacket[19]

for i = 0 .. WireLength - 1:
    mask = EXPAND_BYTE(active_seed_table[selector], i)
    if i == 19:
        mask = selector

    packet[i] = packet[i] XOR mask
```


`EXPAND_BYTE` 从对应种子和偏移生成一个字节。它必须满足：

- 只使用固定宽度无符号整数，溢出按模 `2^N` 定义。
- 字节序、旋转和常量在协议中写死，不依赖主机端序。
- 不读取 packet 内容、前一字节或 endpoint；同一 `selector + offset` 总是得到同一值。
- 发送与接收共用同一函数，偶数偏移和奇数偏移都需有测试向量。
- 偏移 19 是唯一的 selector 启动特例；该偏移的掩码必须等于 selector。

共有 256 个 selector，其中 `0..3` 是内置表，`4..255` 是当前连接的动态表。按 endpoint 和包号选索引的目的是让连续报文分散在各组掩码中，不是提供密码学强度。

### 5.3 为什么不从 payload 推导 selector

只混淆包头时，可以从未修改的 payload 采样并推导索引。全包 XOR 后 payload 也是未知明文，接收端会陷入“需要 selector 才能恢复 payload，又需要 payload 才能得到 selector”的循环依赖。

使用 wire offset 19 作为启动字节，是不增加外层字段时最简单的 O(1) 解码方式。接收端只需读取此字节，不需要了解发送端用来选种子的 endpoint。

### 5.4 连接建立后的动态种子表

内置表用于初始化、Initial、Handshake 和动态表尚未安装时的报文。主动端在连接尝试开始时生成 `seed_root`，随 Initial 中的 `OBFUSCATION` 帧发送给被动端：

```text
OBFUSCATION := type(1) | flags(1) | epoch(4) | seed_root(16)
```

`epoch` 是动态种子表的生成号，第一张表为 1，后续轮换递增。`seed_root` 是本次连接尝试的根数据，同一尝试的 Initial 重传必须复用它。不再设计 `direction` 字段；两个方向共用同一张动态表。

`seed_root` 不直接作为整个种子表，而是按 selector 延迟派生：

```text
prk = HKDF-Extract(
    salt = "libutp-obf-table-v1",
    ikm  = seed_root || epoch_be32
)
dynamic_seed[selector] = HKDF-Expand(
    prk,
    info   = "entry" || uint8(selector - 4),
    length = 16
)
```

各字段的含义：

- `salt` ：固定协议域标识，不在线传输，用于避免与其他派生用途冲突。
- `seed_root` ：主动端为一次连接尝试生成的 16 字节根，重传时不改变。
- `epoch_be32` ：以大端序编码的 4 字节表版本，用于区分不同动态表。
- `selector - 4` ：动态表内部序号，`selector=4` 对应第 0 项。
- `length=16` ：每项动态种子的固定长度，不需要额外编码。

种子索引空间统一为 `0..255`：`0..3` 永远指向内置表，`4..255` 指向当前连接的动态表。因此报文无需增加“使用哪张表”的额外标志；看到 selector 即可知道是启动表还是动态表。动态表仅为 `4..255` 生成条目，不覆盖 `0..3`。

`active_seed_table[selector]` 不是另一个实体表：`selector < 4` 时它是 `builtin_seed_table[selector]`；`selector >= 4` 时它是按上述 HKDF 懒惰派生的 `dynamic_seed[selector]`。

这样每条连接只需保存很小的根和 epoch，不需要分配 256 组大表。HKDF 只用于定义良好、跨平台一致的种子扩展，不改变非加密模式不提供安全性的边界。

交换使用 Initial 与 Handshake 的现有握手时序，不再新增方向字段或独立 ACK 帧：

1. 主动端在连接尝试开始时生成 `seed_root`，随 Initial 中的 `OBFUSCATION` 帧发送。Initial 使用 selector `0..3` 的内置表，因此被动端可在获得动态根之前解码。
2. 被动端收到并校验 Initial 后立即安装动态表，并用内置表返回 Handshake 确认。重复 Initial 必须幂等，不重新生成 `seed_root`。
3. 主动端只在收到 Handshake 后允许发送 selector `4..255` 的动态包；在此之前，包括 0-RTT 和重传，使用内置表。
4. 被动端在收到 Initial 后可以使用动态表回包，但 Handshake 本身使用内置表，以保证主动端可以获得切换确认。
5. 切换期同时保留内置表和动态表；接收时根据 selector 选择表，不做所有 Connection 遍历。迟到的内置表包在切换窗口内仍然有效。

如果动态索引包先于其携带种子的 Initial 到达被动端，被动端必须静默丢弃该包，不能将其误判为协议错误。主动端等待 Handshake 后才发动态包，这个规则使此情形在正常发送中不会发生。

`OBFUSCATION` 帧作为 Initial 中的控制帧，使用内置表混淆，不能用尚未交换的动态表发送。如果开启 AEAD，`seed_root` 随已认证的 Initial 一起受保护；非加密时它可被观察和篡改，因此只能提供混淆多样性，不能提供安全性。

## 6. 发送顺序

### 6.1 非加密包

```text
1. 构造 canonical Header，Reserve = 0
2. 编码帧和可选 Padding
3. 确定 PayloadLength 和最终 wire 长度
4. 生成 selector
5. 将包整并到 wire scratch
6. 对 wire scratch 全长度执行一次连续 XOR
7. 发送 WirePacket
```

### 6.2 加密包

```text
1. 构造 canonical Header/Frames/Padding，Reserve = 0
2. 设置包含 AEAD Tag 的最终 PayloadLength
3. 用 canonical Header 作为 AAD，加密 payload
4. 生成 selector
5. 对 Header || Ciphertext || Tag 执行全长度连续 XOR
6. 发送 WirePacket
```

必须先 AEAD、后混淆。否则接收端无法用 canonical Header 重建相同 AAD。

## 7. 接收顺序

```text
1. 拒绝小于 20 字节或大于本地报文上限的 UDP payload
2. 先对原始字节做完整 NAT_PROBE 结构校验；命中才按明文 NAT 探测处理
3. 其他报文读取 WirePacket[19] 作为 selector
4. 对整个报文执行同一次无状态 XOR
5. 验证 Reserve、PayloadLength、Type 和 Packet Number
6. 按 DCID/握手阶段规则定位 Connection 或 pending
7. 加密包先用 canonical Header 作为 AAD 完成 AEAD 认证和解密
8. 扫描和处理帧
```

明文 NAT 探测与混淆包可共用一个 UDP socket，但不能只看 raw `Type == NAT_PROBE` 就旁路。必须同时校验明文 Header、固定包长、NAT 请求/响应字段、token 和当前探测任务关联（或 NAT 服务端角色）。任一项不成立都应回到普通的混淆报文路径，而不是把报文当作错误 NAT 包丢弃。

## 8. CTRL Padding

### 8.1 编码

PADDING 帧保持现有格式：

```text
type(1) | padding_length(2) | padding_bytes[padding_length]
```

但 `padding_bytes` 改为伪随机内容，不再要求全 0。解码端只校验帧边界并跳过内容。

对普通 `UTP_PACKET_TYPE_CTRL` 包：

- `padding_length` 范围为 `0..32`，0 表示本包不添加 PADDING 帧。
- 32 是 Padding 内容上限，不含 3 字节帧头；最大线路开销为 35 字节。
- 长度和内容使用不同 domain 的固定 mixer 派生。

```text
padding_length = MIX(endpoint || packet_number || "padding-length") mod 33
padding_bytes  = EXPAND(endpoint || packet_number || "padding-content", padding_length)
```

endpoint 序列化必须包含 `family | address | port | IPv6 scope_id`。接收端不重算 Padding，因此 NAT 前后 endpoint 视图不同不影响解码。

### 8.2 组包优先级

```text
ACK
可靠控制帧
STREAM header
尽可能多的 STREAM data
剩余空间允许时的 PADDING
```

不得为 Padding 预留空间，也不得为 Padding 缩减本可放入的 STREAM 数据。剩余空间小于 `3 + 1` 字节时不添加。

Padding 保持非语义性：

- 不使 ACK-only 包变为 ack-eliciting。
- 不单独进入重传。
- 不占用可靠控制帧槽位。
- 不改变拥塞分类和 in-flight 语义。
- 所有精确比较 `frame_types == ACK` 的代码都必须忽略 PADDING 位。

MTU Probe 继续使用现有专用 PING + PADDING 规则，不受 32 字节通用上限约束。

## 9. PacketOut、重传和路径

- PacketOut 保存 canonical 头、帧元数据和 STREAM slice，不就地修改为混淆形式。
- selector 使用该次发送尝试的目的 endpoint 和包号选择；Padding 和 wire 字节也在此时固定。
- socket `WOULD_BLOCK` 后的同一尝试必须重用完全相同的 wire 字节，不得重新选 selector 或 Padding。
- 丢包重传使用新包号重新组包，因此生成新 selector、Padding、AEAD 密文和混淆 wire 字节。
- 候选路径变化时，需要为新 endpoint 创建新发送尝试；不得在相同包号的 AEAD 包上只改 Padding。

这里的“canonical 数据”是 PacketOut 的逻辑源数据，可能由 `raw_data` 和外部
STREAM slice 组成；“wire 数据”是已经完成 AEAD、Padding 和 XOR 后、可以直接交给
UDP 的连续字节。两者不能因为一次发送成功就混为同一生命周期对象：同一发送尝试在
`WOULD_BLOCK` 重试时必须继续保留稳定的 wire 数据，而重传或重新分片时必须能够重新
构造 canonical 数据。

## 10. 内存与零拷贝影响

全包 XOR 与当前非加密 STREAM 外部 slice 的直接发送不兼容。原因是应用缓冲区只读且由外部所有，libutp 不能为发送而就地 XOR，也不能在异步 `sendmmsg` 后安全恢复。

实现要求：

- PacketOut 应有一块统一的 wire 输出缓冲。实现上可以先将现有
  `encrypt_data` 重命名为 `wire_data`，将 `encrypt_data_size` 重命名为
  `wire_data_size`；这两个字段描述的是“可直接发送的连续 wire 字节”，不再暗示
  只有加密包才会使用它。
- 非加密包从 PacketOut 缓冲池取得 MTU 级 wire 缓冲。遍历 `raw_data` 和外部
  STREAM slice 时直接执行 `wire[i] = source[i] ^ mask(i)`，把整并和 XOR 合并为
  一次写入，不需要先复制 canonical 数据再复制一次混淆数据。
- 加密包先把 canonical 包编码到 wire 缓冲并完成 AEAD，然后在同一缓冲区内原地
  XOR。这样加密和混淆不会额外增加第二块 MTU 级 payload 缓冲。
- 只有在 canonical 数据不再需要被重新编码、且当前发送尝试的 wire 字节已经固定时，
  才允许让 wire 缓冲与原有 PacketOut 内联缓冲复用同一分配。若后续操作仍需要读取
  canonical header，必须使用独立的 wire 缓冲，不能覆盖 `raw_data`。
- wire 缓冲的生命期要覆盖批量发送、`WOULD_BLOCK` 和 writable 重试；不得使用一个
  Context 临时数组覆盖多个排队中的 PacketOut。

当前代码中的字段事实需要特别区分：`utp_packet_out_pool_acquire()` 会把
`encrypt_data` 初始化为与 `raw_data` 相同的池缓冲地址，因此非加密 PacketOut 中该
指针通常也不为 NULL。是否存在有效加密输出由 `UTP_PO_ENCRYPTED` 和
`encrypt_data_size` 判断，不能用指针是否为空判断。实现混淆时应以长度和状态判断
wire 缓冲是否已生成；重命名为 `wire_data` 后可避免继续传播这一歧义。

因此，非加密模式每个实际发送包仍会增加一次 MTU 以内的内存写入，但可以与混淆过程
合并为一次；加密模式可以复用 AEAD 输出缓冲并原地混淆。这保留了应用层外部
STREAM 缓冲区零拷贝接口，但不应在文档或 API 中宣称混淆路径是端到端零拷贝。

## 11. 编译开关与兼容性

使用单一编译期开关，例如：

```c
#define UTP_ENABLE_WIRE_OBFUSCATION 1
```

不增加运行时 `utp_obfuscation_mode`，也不使用 Connection option 协商是否开启。`OBFUSCATION` 只是 Initial 中的连接级种子传递帧，不是开关协商帧。

这会产生两种不兼容的 wire profile：

- 关闭：现有明文包头格式。
- 开启：本文定义的全包 XOR 格式。

实现时必须将开启 profile 的非 NAT 协议版本从 v3 提升到 v4，并确保同一部署中的 libutp、`ntrs` 和测试工具使用相同 profile。`NAT_PROBE` 保持现有明文 wire 格式，`natd_hub`、`natd_node` 与 `ntrs_natc` 不受该 profile 影响。由于版本帧本身也在混淆包内，两种非 NAT profile 无法在不增加额外外层标识的情况下自动协商。

## 12. 实现责任边界

建议新增独立的 `proto/obfuscation.c` 和共享内部头文件，只提供：

- 基于发送目的 endpoint 和包号的 selector 生成。
- 原地全包混淆/反混淆。
- Padding 长度和内容派生所需的无状态字节扩展。

种子表的定义与发送/接收函数必须由 libutp 与 NTRS 共享，不允许在 `ntrs/` 再维护一份类似但不同的种子表。`NAT_PROBE` 编解码不调用此模块。

不把混淆逻辑放入：

- `crypto/`：避免将 XOR 误表述为加密。
- `CRYPTO` 帧：不改变密钥交换语义。
- Stream：Stream 只管理明文字节和流控。
- Socket：Socket 只发送已准备好的 wire 字节。

Context/Connection 的 wire 边界负责按本文顺序组合帧编码、AEAD、整并和混淆。

## 13. 测试清单

### 13.1 算法向量

- 256 个 selector 全覆盖。
- IPv4、IPv6（含 scope_id）目的 endpoint 与不同 Packet Number 生成稳定 selector。
- 包长 20、21、1280、1492、1500 和协议允许的上限。
- 全 0、全 `0xff`、递增字节和真实报文向量。
- 原地 `decode(encode(packet)) == packet`。
- x86_64、AArch64、Windows、Linux、macOS 输出完全一致。

### 13.2 协议路径

- Initial、Handshake、0-RTT、CTRL、CONNECTION_CLOSE 和 RENDEZVOUS 的混淆编解码。
- Initial 中 `OBFUSCATION` 帧的重传幂等性、Handshake 确认后的动态切换、动态包与 Initial 乱序及迟到的内置包。
- `NAT_PROBE` 在开启 profile 时仍保持明文，且与混淆报文共用同一 UDP socket 时路由正确。
- 加密与非加密连接。
- direct、NTRS 打洞、多候选 endpoint 和路径迁移。
- 包头恢复后的 CID demux，以及 pending incoming 查找。
- AES-GCM AAD 在混淆前后保持一致。
- `WOULD_BLOCK`、部分批量发送和 writable 重试不改变 wire 字节。
- 丢包重传使用新包号和新 wire 字节。
- 同步更新 `utp.lua` dissector；旧 v3 明文 profile 和 v4 混淆 profile 由用户显式选择，不靠抓包启发自动判断。
- 动态种子派生、Handshake 确认、epoch 轮换和乱序包处理。

### 13.3 Padding

- `padding_length` 覆盖 0、1、32 和剩余 MTU 空间边界。
- Padding 内容不是全 0，解码端不检查内容值。
- 没有为 Padding 减少 STREAM data。
- ACK + PADDING 仍然是 ACK-only 语义。
- MTU Probe 仍可填充到目标探测长度。

### 13.4 恶意和错误输入

- 小于 20 字节、超大 UDP payload、错误 Reserve 和错误 PayloadLength。
- raw 包头偶然伪装成 `NAT_PROBE` 但 NAT 完整校验失败时，仍进入混淆解码路径。
- 随机 UDP 数据不得创建 Connection 或 pending。
- 加密包反混淆后 AEAD 失败时静默丢弃。
- fuzz 覆盖 selector 预读、分段恢复和帧扫描。

## 14. 与全包 AEAD 草案的关系

[全包加密与无状态可验证 CID 混淆方案](../crypto/全包加密与无状态可验证CID混淆方案.md) 是另一个更重的未来方案：它引入新外层、Opaque CID 和内层全包 AEAD。

两份文档是候选 wire profile，不是需要同时实现的两层：

- 本文优先保持当前 20 字节头、32 bit CID 和 Connection 查找架构，改动小，但只提供特征混淆。
- 全包 AEAD 草案会重构 demux 和 CID 体系，安全边界更强，代价也更高。
- 如果未来选择全包 AEAD 外层，应删除或显式升级本文的 wire profile，不能在新外层上无条件叠加另一层固定 XOR。
