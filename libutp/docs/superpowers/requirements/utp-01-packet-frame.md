# UTP-01 需求文档：包头 + 包类型 + 帧体系（编解码）

> 本文档最初由 C++ 实现（`cpp/`）反推，现作为当前 C 协议规范维护。`docs/` 是当前 ground truth；C++ 仅作基础行为参考，`doc/` 为旧文档。带 `cpp/` 路径的说明记录参考实现历史行为，若与当前线格式表、MUST 规则或 C 实现冲突，以当前规范为准。
> 引用格式：`文件:行号`。数值均取自代码；不确定处标注"待确认"。

---

## 1. 职责与边界

本模块负责 UTP 数据报（UDP payload）在**线上（wire）字节序列**与**内存结构**之间的编解码，具体包括：

- **固定包头** `UTPHeaderProto` 的定义与序列化布局（`cpp/src/proto/proto.h:34`）。
- **包类型** `UTP_TYPE_*`（承载于包头 `types` 字段，`cpp/src/proto/proto.h:20-25`）。
- **帧类型枚举** `FrameType` 与每种帧的线上格式、定长/变长规则、`encode`/`decode`/`frameSize`（`cpp/src/proto/frame.h:28` 及 `cpp/src/proto/frame/*`）。
- **入向包解析** `PacketIn`：从缓冲区解出包头 + 遍历帧、构建帧类型位图（`cpp/src/proto/packet_in.cpp`）。
- **出向包容器** `PacketOut`：承载已编码帧数据、帧元信息、发送/重传状态（`cpp/src/proto/packet_out.h`）。
- **帧类型位图** `PacketFrameTypeBit` 与重传掩码 `UTP_FRAME_RETX_MASK`（`cpp/src/proto/packet_common.h:18-65`）。

**边界（不属于本模块）**：加密/解密（仅暴露 `crypto_type` 与预留 16 字节）、拥塞控制、流状态机、ACK 生成策略、传输参数协商语义。这些通过依赖（§9）引用。

---

## 2. 线上格式 / 数据结构

### 2.1 固定包头 `UTPHeaderProto`

定义：`cpp/src/proto/proto.h:34-41`；固定长度 `UTP_HEADER_SIZE = 20`（`cpp/src/proto/proto.h:18`）。
序列化顺序见 `PacketIn::decode`（`cpp/src/proto/packet_in.cpp:57-69`）。字节序为**大端（big-endian）**（`utils/include/utils/serialize.hpp:30,39,48` 使用 `htobe16/32/64`）。

| 偏移 | 大小 | 字段 | 类型 | 语义 |
|---:|---:|---|---|---|
| 0 | 4B | `scid` | uint32 | source connection ID |
| 4 | 4B | `dcid` | uint32 | destination connection ID |
| 8 | 8B | `pn` | uint64 | packet number（完整 64 位，非截断） |
| 16 | 2B | `payload_length` | uint16 | 负载长度，不含 20 字节固定头 |
| 18 | 1B | `types` | uint8 | 包类型（`UTP_TYPE_*`） |
| 19 | 1B | `reserve` | uint8 | 保留字段 |

- 头部之后紧跟 `payload_length` 字节的帧区（Frames）。
- 注：`proto.h:18` 对 `UTP_HEADER_SIZE` 的注释写作"UDP 头部长度"，实为 UTP 固定头长度（代码值 20 正确，注释文字有误）。

### 2.2 包类型 `UTP_TYPE_*`（`cpp/src/proto/proto.h:20-25`）

| 值 | 宏 | 语义（注释） |
|---:|---|---|
| 0x00 | `UTP_TYPE_NONE` | None |
| 0x01 | `UTP_TYPE_INITIAL` | Client Hello |
| 0x02 | `UTP_TYPE_HANDSHAKE` | Server Hello |
| 0x03 | `UTP_TYPE_0RTT` | 0-RTT Data |
| 0x04 | `UTP_TYPE_CONNECTION_CLOSE` | Connection Close |
| 0x05 | `UTP_TYPE_CTRL` | Control Frame |

协议版本常量 `UTP_PROTOCOL_VERSION = 2`（`cpp/src/proto/proto.h:30`）。

### 2.3 帧类型 `FrameType`（`cpp/src/proto/frame.h:28-52`）

枚举以 0 起顺序赋值（`kFrameInvalid=0`）。类型字节即帧首字节（1 字节）。

| 值(十进制/十六进制) | 枚举名 | 是否有独立帧类 | frameLength 是否解析 |
|---:|---|---|---|
| 0 / 0x00 | `kFrameInvalid` | 否 | 否（default→UNEXPECTED） |
| 1 / 0x01 | `kFrameStream` | 是 | 是 |
| 2 / 0x02 | `kFrameAck` | 是 | 是 |
| 3 / 0x03 | `kFramePadding` | 是 | 是 |
| 4 / 0x04 | `kFrameConnectionClose` | 是 | 是 |
| 5 / 0x05 | `kFramePing` | **否**（无 FramePing 类） | 是（定长 1） |
| 6 / 0x06 | `kFrameResetStream` | 是 | 是 |
| 7 / 0x07 | `kFrameStreamsBlocked` | 是 | 是 |
| 8 / 0x08 | `kFrameMaxStreams` | 是 | 是 |
| 9 / 0x09 | `kFramePathChallenge` | 是 | 是 |
| 10 / 0x0A | `kFramePathResponse` | 是 | 是 |
| 11 / 0x0B | `kFrameCrypto` | 是 | 是 |
| 12 / 0x0C | `kFrameSessionToken` | 是 | 是 |
| 13 / 0x0D | `kFrameAckFrequency` | 是 | 是 |
| 14 / 0x0E | `kFrameVersion` | 是 | 是 |
| 15 / 0x0F | `kFrameHandshakeDone` | 是 | 是 |
| 16 / 0x10 | `kFrameTransportParams` | 是 | 是 |
| 17 / 0x11 | `kFrameHandshakeDelay` | 是 | 是 |
| 18 / 0x12 | `kFrameMaxData` | 是 | 是 |
| 19 / 0x13 | `kFrameMaxStreamData` | 是 | 是 |
| 20 / 0x14 | `kFrameDataBlocked` | 是 | 是 |
| 21 / 0x15 | `kFrameStreamDataBlocked` | 是 | 是 |
| 22 / 0x16 | `StopSending` | 是 | 是 |
| 23 | `kFrameMax` | 哨兵（帧类型上界） | — |

辅助枚举：
- `FrameStreamFlags`（`frame.h:66-69`）：`kFrameStreamFlagNone=0x00`，`kFrameStreamFlagFin=0x01`（bit0，流最后一帧）。宏 `STREAM_IS_FIN`/`STREAM_SET_FIN`（`frame.h:19-20`）。
- `FrameCryptoType`（`frame.h:71-74`）：`kFrameCryptoAESGCM128=0`，`kFrameCryptoAESGCM256=1`。

### 2.4 每种帧的线上格式（字段顺序 = 线上顺序，大端）

固定头长度宏与总长度均引用代码。以下"HDR"指定长头部，变长帧总长 = HDR + 可变体。

| 帧 | 头/总长宏 | 值(B) | 线上字段（偏移相对帧首） |
|---|---|---:|---|
| Stream | `FRAME_STREAM_HDR_SIZE` (`stream.h:14`) | 16 | type(1) @0, stream_flag(1) @1, stream_data_length(2) @2, stream_id(4) @4, stream_offset(8) @8, data(stream_data_length) @16 |
| Ack | `FRAME_ACK_HDR_SIZE` (`ack.h:17`) | 16 (+8/range) | type(1), range_count(1), ack_delay(2), first_ack_range(4), largest_acked(8), 之后 range_count 个 {gap(4), ack_range_length(4)}，每组 `FRAME_ACK_RANGE_SIZE`=8 (`ack.h:18`) |
| Padding | `FRAME_PADDING_HDR_SIZE` (`padding.h:13`) | 3 | type(1), padding_length(2), zeros(padding_length) |
| ConnectionClose | `FRAME_CONNECTION_CLOSE_HDR_SIZE` (`connection_close.h:15`) | 5 | type(1), error_code(2), reason_length(2), reason(reason_length) |
| Ping | 无宏（frameLength 内定长） | 1 | type(1) |
| ResetStream | `FRAME_RESET_STREAM_SIZE` (`reset_stream.h:13`) | 15 | type(1), error_code(2), stream_id(4), final_size(8) |
| StreamsBlocked/MaxStreams | `UTP_FRAME_STREAMS_LIMIT_SIZE` | 4 | type(1), stream_type(1), stream_limit(2) |
| StopSending | `UTP_FRAME_STOP_SENDING_SIZE` | 7 | type(1), error_code(2), stream_id(4) |
| PathChallenge/PathResponse | `FRAME_PATH_FRAME_SIZE` (`path.h:17`) | 9 | type(1), data(8) (`FRAME_PATH_DATA_SIZE`=8) |
| Crypto | `FRAME_CRYPTO_SIZE` (`crypto.h:17`) | 35 | type(1), crypto_type(1), reserved(1,必须0), eph_pubkey(32, `FRAME_CRYPTO_EPH_PUBKEY_SIZE`) |
| SessionToken | `FRAME_SESSION_TOKEN_HDR_SIZE` (`session_token.h:15`) | 4 | type(1), token_size(1), token_validity_period(2, 秒), token(token_size) |
| AckFrequency | `FRAME_ACK_FREQUENCY_SIZE` (`ack_frequency.h:14`) | 7 | type(1), ack_eliciting_threshold(1), reordering_threshold(1), max_ack_delay_ms(4) |
| Version | `FRAME_VERSION_SIZE` (`version.h:13`) | 5 | type(1), version(4) |
| HandshakeDone | `FRAME_HANDSHAKE_DONE_SIZE` (`frame.h:22`) | 9 | type(1), ack_handshake_pn(8) |
| TransportParams | `FRAME_TRANSPORT_PARAMS_SIZE` (`transport_params.h:14`) | 38 | type(1), flags(2), max_idle_timeout(4), handshake_timeout(2), init_max_streams_bidi(2), init_max_streams_uni(2), ack_delay_exponent(1), initial_max_data(8), initial_max_stream_data_bidi_local(8), initial_max_stream_data_bidi_remote(8) |
| HandshakeDelay | `FRAME_HANDSHAKE_DELAY_SIZE` (`frame.h:23`) | 5 | type(1), delay_time_us(4) |
| MaxData | `FRAME_MAX_DATA_SIZE` (`max_data.h:13`) | 9 | type(1), maximum_data(8) |
| MaxStreamData | `FRAME_MAX_STREAM_DATA_SIZE` (`max_stream_data.h:13`) | 13 | type(1), stream_id(4), maximum_stream_data(8) |
| DataBlocked | `FRAME_DATA_BLOCKED_SIZE` (`data_blocked.h:13`) | 9 | type(1), data_limit(8) |
| StreamDataBlocked | `FRAME_STREAM_DATA_BLOCKED_SIZE` (`stream_data_blocked.h:13`) | 13 | type(1), stream_id(4), stream_data_limit(8) |

> C 版加密恢复 0-RTT 是后续协议目标，不等同于本节反推的 C++ 现状：其 `SESSION_TOKEN` 线格式、双向 early AEAD 与两消息收敛规则以 `utp-10` §10 为准。服务端完成响应复用 header `types = UTP_TYPE_HANDSHAKE`，但不携带旧 `HandshakeDone` 帧；由客户端 pending 尝试类型选择解密与解析路径。

### 2.5 帧类型位图与重传掩码

- `PacketFrameTypeBit`（`packet_common.h:18-41`）：`kFTBitX = 1 << kFrameX`，供 `PacketIn::frame_types` / `PacketOut::frame_types` 使用（每帧类型占 1 位）。
- `UTP_FRAME_RETX_MASK`（`packet_common.h:43-65`）：需重传帧位掩码。**明确排除**（不重传）：`kFTBitAck`、`kFTBitPadding`、`kFTBitPing`（源码注释掉，见 `packet_common.h:45-48`）。其余全部包含（Stream、ConnectionClose、ResetStream、StreamsBlocked、MaxStreams、PathChallenge/Response、Crypto、SessionToken、AckFrequency、Version、HandshakeDone、TransportParams、HandshakeDelay、MaxData、MaxStreamData、DataBlocked、StreamDataBlocked）。

### 2.6 `PacketIn` / `PacketOut` 结构

- `PacketIn`（`packet_in.h:18-48`）：`header`、`payload`/`payload_size`、`frame_types`（位图）、`raw_data`/`raw_size`。`valid()` 要求 `raw_data!=nullptr && raw_size>=UTP_HEADER_SIZE`。
- `PacketOut`（`packet_out.h:100-142`）：`packno`、`frame_types`（`PacketFrameTypeBit` 位图）、`po_flags`（`PacketOutFlags`）、`local_flags`（`PacketOutLocalFlags`）、`slices[8]`（`PACKET_OUT_MAX_SLICES`=8）、`frame_meta[8]`（`PACKET_OUT_MAX_FRAMES`=8）、加密预留（注释 `packet_out.h:127` "加密时需要预留16字节"）。相关标志枚举：`PacketOutFlags`（`packet_out.h:24-35`）、`PacketOutLocalFlags`（37-42）、`FrameMetaFlags`（44-50）。

---

## 3. 状态机 / 流程

本模块无长期状态机；核心是两条编解码流水线。

### 3.1 入向解码 `PacketIn::decode`（`packet_in.cpp:45-100`）

1. 校验 `buffer!=nullptr && size>=UTP_HEADER_SIZE`，否则 `UTP_ERR_OVERFLOW`（`:46-48`）。
2. 按 §2.1 顺序反序列化 6 个头字段；任一失败 → `UTP_ERR_OVERFLOW`（`:57-69`）。
3. 校验 `left >= payload_length`，否则 `UTP_ERR_OVERFLOW`（"payload truncated"，`:71-74`）。
4. 令 `payload=offset`，`payload_size=payload_length`。
5. 遍历帧区：对每帧调 `frameLength()` 得 `(frameType, frameLen)`；若 `!ok || frameLen==0 || frameLen>剩余` → `UTP_ERR_FRAME_FORMAT_ERROR`（`:80-87`）。
6. `frameType>=kFrameMax` → `UTP_ERR_FRAME_FORMAT_ERROR`（`:90-93`）。
7. 置位 `frame_types |= 1<<frameType`，`iter += frameLen`，直至遍历完 `payload_size`（`:95-96`）。
   - 注：`decode` 阶段仅**验证布局 + 建位图**，不解出各帧字段内容。

### 3.2 逐帧游标 `PacketIn::nextFrame`（`packet_in.cpp:102-118`）

- 输入 `offset`，返回该帧 `frameType`、`frameData` 指针、`frameLen`，并推进 `offset`。
- `offset>=payload_size` 或包无效 → `UTP_ERR_OVERFLOW` 返回 -1；帧非法 → `UTP_ERR_FRAME_FORMAT_ERROR` 返回 -1。

### 3.3 帧长计算 `PacketIn::frameLength`（`packet_in.cpp:120-208`）

- 读首字节为 `frameType`，按类型返回定长或"HDR + 变长头字段"。
- 变长帧读取长度字段用局部 `ReadBE16`（大端，`packet_in.cpp:38`）：Padding 读 `+1`、Stream 读 `+2`、ConnectionClose 读 `+3`；SessionToken 读 `frameData[1]`；Ack 读 `frameData[1]*8`。
- 变长帧先校验 `payloadLeft >= HDR`，否则 `UTP_ERR_OVERFLOW`。
- 末尾统一校验 `frameLen<=payloadLeft`，否则 `UTP_ERR_OVERFLOW`（`:203-205`）。
- **default 分支**（未知/未处理帧类型，含 `kFrameInvalid`、`kFrameStreamsBlocked`、`kFrameMaxStreams`）→ `UTP_ERR_FRAME_UNEXPECTED`（`:199-201`）。

### 3.4 单帧 `encode`/`decode`（各 `frame/*.cpp`）

统一约定：
- `encode(buffer, size, status)`：先校验 `size >= frameSize()`（不足 → `UTP_ERR_OVERFLOW` 返回 -1），按字段顺序 `SerializeTo`，返回写入字节数。
- `decode(buffer, size, status)`：先校验最小长度；反序列化 type 字节后**必须等于本帧类型**，否则 `UTP_ERR_FRAME_UNEXPECTED` 返回 -1；返回消费字节数。

---

## 4. 不变量与规则（MUST / MUST NOT，可测）

1. **[MUST]** 包总长 ≥ `UTP_HEADER_SIZE`(20)，否则 `decode` 返回 `UTP_ERR_OVERFLOW`（`packet_in.cpp:46`）。
2. **[MUST]** 头部布局固定 20 字节、大端、字段顺序 scid,dcid,pn,payload_length,types,reserve（§2.1）。
3. **[MUST]** `payload_length` ≤ 头后剩余字节，否则 `UTP_ERR_OVERFLOW`（`packet_in.cpp:71`）。
4. **[MUST]** 帧区必须能被完整切分：所有帧 `frameLen` 之和恰好铺满 `payload_size`，且每个 `frameLen∈(0, 剩余]`（`packet_in.cpp:84`）。
5. **[MUST]** 解出的帧类型 < `UTP_FRAME_TYPE_MAX`(23)。
6. **[MUST NOT]** 帧首字节不得为 `UTP_FRAME_TYPE_INVALID`、`UTP_FRAME_TYPE_MAX` 或更大的未知类型；否则按协议错误拒绝。
7. **[MUST]** 单帧 `decode` 时类型字节必须与目标帧一致，否则 `UTP_ERR_FRAME_UNEXPECTED`（所有 `frame/*.cpp` 的 decode）。
8. **[MUST]** Crypto 帧 `reserved` 字节必须为 0，且 `crypto_type∈{0,1}`，否则 `UTP_ERR_INVALID_PARAM`（`crypto.cpp:98-101`）。
9. **[MUST]** Ack：`first_ack_range != 0`；`range_count < ackInfo.ack_ranges.size()`；`largest_acked >= first_ack_range-1`；各 gap 满足 `lastAcked > gap`；各 `ack_range_length != 0`——否则 `UTP_ERR_INVALID_PARAM`（`ack.cpp:118,144,155,175,186`）。`range_count` 表示**附加**范围数（不含 first_ack_range），`ackInfo.range_size = range_count+1`（`ack.cpp:117,129`）。
10. **[MUST]** Ack `ack_delay` 编解码用 `ack_delay_exponent` 位移：编码 `delay >> exp`、解码 `delay << exp`；`exp` 不得 > `kMaxAckDelayExponent=20`，否则 `UTP_ERR_INVALID_PARAM`（`ack.cpp:32,88,136,61`）。
11. **[MUST]** ConnectionClose/SessionToken/Stream 编码时长度字段与实际数据长度一致（`connection_close.cpp:34`、`session_token.cpp:34`）；Stream 当 `stream_data_length>0` 时 `stream_data` 不得为空（`stream.cpp:27`）。
12. **[MUST]** TransportParams `decode`：`flags` 不得含 `~kDefaultFlags` 的未知位；`ack_delay_exponent<=20`；三个 flow-control 值 `<= kMaxFlowControlValue=(2^60-1)`——否则 `UTP_ERR_INVALID_PARAM`（`transport_params.cpp:97-110`）。
13. **[MUST]** 包号有效性：`packno <= UTP_MAX_PACKNO=(2^62-1)`（`IsValidPackNo`，`packet_common.h:67`；`UTP_INVALID_PACKNO = 2^62`，`:14`）。
14. **[MUST]** 重传时按 `UTP_FRAME_RETX_MASK` 判定，ACK/Padding/Ping 帧 MUST NOT 重传（§2.5）。

---

## 5. 参数与默认值（确切值 + 变量名，来自代码）

### 5.1 尺寸/协议常量

| 名称 | 值 | 位置 |
|---|---:|---|
| `UTP_HEADER_SIZE` | 20 | `proto.h:18` |
| `UTP_PROTOCOL_VERSION` | 2 | `proto.h:30` |
| `UTP_MAX_PACKNO` | 2^62-1 | `packet_common.h:13` |
| `UTP_INVALID_PACKNO` | 2^62 | `packet_common.h:14` |
| `FRAME_CRYPTO_EPH_PUBKEY_SIZE` | 32 | `crypto.h:15` |
| `FRAME_PATH_DATA_SIZE` | 8 | `path.h:15` |
| `FRAME_ACK_RANGE_SIZE` | 8 | `ack.h:18` |
| 各 `FRAME_*_SIZE` | 见 §2.4 | 各帧头 |

### 5.2 ACK/延迟默认阈值（`proto.h:27-29`）

| 名称 | 值 |
|---|---:|
| `UTP_DEFAULT_ACK_THRESHOLD` | 5 |
| `UTP_DEFAULT_MAX_ACK_DELAY_MS` | 25 |
| `UTP_DEFAULT_REORDER_THRESHOLD` | 3 |

### 5.3 AckFrequency 帧字段默认/上限（`ack_frequency.h:21-39`）

| 名称 | 值 |
|---|---:|
| `kDefaultAckElicitingThreshold` | = `UTP_DEFAULT_ACK_THRESHOLD`(5) |
| `kDefaultReorderingThreshold` | = `UTP_DEFAULT_REORDER_THRESHOLD`(3) |
| `kDefaultMaxAckDelayMs` | = `UTP_DEFAULT_MAX_ACK_DELAY_MS`(25) |
| `kMaxAckElicitingThreshold` | 64 |
| `kMaxReorderingThreshold` | 32 |
| `kMaxAckDelayMsClamp` | 1000 |
| 成员默认 `ack_eliciting_threshold` | 10 |
| 成员默认 `reordering_threshold` | 3 |
| 成员默认 `max_ack_delay_ms` | 150 |

> 注：`normalize()`（`ack_frequency.cpp:86-105`）在编解码时把 0 值替换为默认、非 0 值 clamp 到上限。**成员构造默认值（10/3/150）与 `normalize` 后的默认值（5/3/25）不一致**，见 §8。

### 5.4 TransportParams 默认与限值（`transport_param.h:15-88`）——本模块通过 TransportParams 帧承载

| 名称 | 值 |
|---|---:|
| `kMaxAckDelayExponent` | 20 |
| `kMaxFlowControlValue` | 2^60-1 |
| `kDefaultFlags` | 8 位全 1（`kMaxNumeric=8` 个参数） |
| `max_idle_timeout` | 600000 (ms) |
| `handshake_timeout` | 5000 (ms) |
| `init_max_streams_bidi` | 64 |
| `init_max_streams_uni` | 32 |
| `ack_delay_exponent` | 3 |
| `initial_max_data` | 64 MiB |
| `initial_max_stream_data_bidi_local` | 16 MiB |
| `initial_max_stream_data_bidi_remote` | 16 MiB |

### 5.5 PacketOut 容量常量

`PACKET_OUT_MAX_SLICES=8`、`PACKET_OUT_MAX_FRAMES=8`（`packet_out.h:97-98`）。

---

## 6. 对外接口

- `struct UTPHeaderProto`（`proto.h:34`）。
- `PacketIn::decode / valid / hasFrame / nextFrame`（`packet_in.h:20-31`）；私有 `frameLength`。
- `PacketOut::reset / initForReuse / addSendAttempt / clearSendAttempts`（`packet_out.h:101-104`，实现 `packet_out.cpp`）。
- 每种帧类：`FrameX::encode(void*,size_t,Status&) const` / `decode(const void*,size_t,Status&)` / `frameSize() const`（Path 帧无 `frameSize`）。均返回 `int32_t`（成功=字节数，失败=-1，错误经 `Status&` 出参）。
- `std::string FrameTypeToString(uint32_t type)`（`frame.cpp:14`）——把**帧类型位图**转为 `A|B|...` 文本（注意入参是位图，非枚举值）。
- 握手帧构造辅助：`BuildHandshakeDoneFrame` / `BuildHandshakeDelayFrame` / `BuildHandshakeTrailer`（`handshake_helper.h:18-32`）；`BuildHandshakeTrailer` 先写 HandshakeDone 再紧跟 HandshakeDelay（`handshake_helper.cpp:57-80`）。
- `FrameBase`（`frame.h:56-64`）：所有帧基类，含 `FrameType type`。

---

## 7. 当前实现边界（已实现 / 部分 / 预留 / TODO）

> 规范优先级：`docs/` 是当前 C 协议实现依据；C++ 实现只作为基础行为参考，`doc/` 为旧文档。三者冲突时以本目录文档为准。

**已实现（完整 encode+decode+frameLength）**：Stream、Ack、Padding、ConnectionClose、ResetStream、StopSending、StreamsBlocked、MaxStreams、PathChallenge、PathResponse、Crypto、SessionToken、AckFrequency、Version、HandshakeDone、TransportParams、HandshakeDelay、MaxData、MaxStreamData、DataBlocked、StreamDataBlocked。

**部分实现**：
- **Ping**：仅在 `frameLength` 中定长 1（`packet_in.cpp:129-131`），**无 `FramePing` 类**、无 encode/decode 文件。构造 Ping 需在别处直接写 1 字节（待确认）。

**C++ 参考实现缺口（不限制当前 C 规范）**：
- C++ 的 `kFrameStreamsBlocked`(7)、`kFrameMaxStreams`(8) 仅有枚举，未在旧 `frameLength` 中处理；C 版已实现其编解码和流额度状态机。
- C++ 尚无 `StopSending`；C 版使用 0x16，并将 `UTP_FRAME_TYPE_MAX` 更新为 0x17。
- `FrameStream`（空占位）：`frame_stream.h:18-21` 定义空结构体 `struct FrameStream {}`，与真正的 `frame/stream.h::FrameStream` 同名但无内容，疑似残留/未用（待确认）。

**TODO（代码内注释）**：
- `packet_out.h:131-132`：`MSG_ZEROCOPY` 发送完成跟踪字段（cookie/完成状态/完成区间）待补。
- `PacketOut::addSendAttempt`（`packet_out.cpp:18-32`）：`packetNo`/`sentTime` 入参被忽略（`(void)`），仅自增 `attempts_count`，未真正建链表节点。`attempts_head/tail` 恒为 nullptr。

**行为细节**：`PacketIn::decode` 把 `frameLength` 返回的 `UTP_ERR_FRAME_UNEXPECTED` 统一改写为 `UTP_ERR_FRAME_FORMAT_ERROR` 对外返回（`packet_in.cpp:84-87`），故未知帧在整包解析层表现为 FORMAT_ERROR；`UTP_ERR_FRAME_UNEXPECTED` 仅由单帧 `decode` 类型不匹配时直接抛出。

---

## 8. 与 doc/ 的差异

| 项 | 代码 | doc | 判定 |
|---|---|---|---|
| 帧类型值 | 枚举 0 起：Stream=1…StreamDataBlocked=21、StopSending=22（§2.3） | `doc/协议设计文档.md:167-187` 列 0x01…0x15 | C 版新增 StopSending=0x16；旧 doc 未列该帧 |
| 包类型 | 含 `UTP_TYPE_NONE=0x00` | `doc/协议设计文档.md:143-149` 仅列 0x01–0x05 | 代码多 `NONE=0x00`；其余一致 |
| Padding 用途 | 见 §2.4 | `doc/frame/帧重传.md:18` "填充 Initial 到 **1260** 字节" | doc 数值 1260，代码未定义此常量（待确认，属其他模块） |
| Ping 帧 | 无独立帧类，仅定长 1 | `doc/协议设计文档.md:171` 列为正式帧 | 代码实现不完整（§7） |
| StreamsBlocked/MaxStreams | C 版已完整解析并处理 | doc 列为正式帧 | C 版已实现；C++ 参考实现仍缺失 |
| AckFrequency 默认 | 成员默认 10/3/150，normalize 后 5/3/25 | doc 未明确成员默认 | 代码内部两套默认不一致（§5.3） |
| 头部 `UTP_HEADER_SIZE` 注释 | 注释误写"UDP 头部长度" | doc 明确为 UTP 20 字节固定头 | 代码注释文字瑕疵，值正确 |
| 重传掩码 | ACK/Padding/Ping 不重传 | `doc/frame/帧重传.md` 说明一致 | 一致 |

---

## 9. 依赖

- **序列化库** `utils/serialize.hpp` + `utils/endian.hpp`（`htobe16/32/64`、大端）：`SerializeTo`/`DeserializeFrom` 是所有编解码的底座（`frame.h:14`）。
- **错误码** `utp/errno.h`：`UTP_ERR_OK=0x0000`、`UTP_ERR_INVALID_PARAM`、`UTP_ERR_OVERFLOW`、`UTP_ERR_FRAME_FORMAT_ERROR=0x0060`、`UTP_ERR_FRAME_UNEXPECTED(=0x0061)`。
- **Status** `util/status.h`（`Status::Error`/`ErrorLiteral`/`OK`）。
- **TransportParams** `util/transport_param.h`（TransportParams 帧承载，限值/默认值来源，§5.4）。
- **Ack 相关**：`util/ack_info.h`（`AckInfo`，含定长 `ack_ranges` 数组）、`util/receive_history.h`（`ReceiveHistory`，编码 ACK 来源）、`utp/config.h`（`Config::max_ack_range_size`，`ack.cpp:219`）。
- **Crypto**：`crypto/aes_gcm_context.h`（`PacketOut::reset` 释放加密缓冲，`packet_out.cpp:72`）；Crypto 帧承载 x25519 公钥。
- **Socket 层**：`socket/packet.h`（`PacketMetaInfo`，`packet_in.h:47`）。
- **PacketOut**：`queue.h`（TAILQ 宏）、`utp/types.h`（`utp_packno_t`/`utp_time_t`）、`congestion/bw_sampler.h`（`BWPacketState`）、`util/malo.hpp`。

---

*生成依据：`cpp/src/proto/*` 与 `cpp/src/proto/frame/*` 全量；交叉参考 `doc/协议设计文档.md`、`doc/frame/帧重传.md`。*
