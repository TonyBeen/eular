# 核心传输模块

核心模块均位于 `c/src/`，公共接口位于 `c/include/utp/`。

| 模块 | 代码目录 | 主要职责 |
|---|---|---|
| Context | `c/src/context/` | 事件循环、连接索引、握手期任务和统一写事件 |
| Public API | `c/include/utp/` | Context、Connection、Stream、状态码和回调契约 |
| Proto | `c/src/proto/` | 包头、帧、PacketIn、PacketOut、ACK 编解码 |
| Stream | `c/src/connection/stream.c` | 流缓冲、乱序重组、FIN/RESET 和流控 |
| Reliability | `c/src/context/send_control.c` | ACK、账本、丢包检测、PTO 和重传 |
| Congestion | `c/src/congestion/` | BBR、CUBIC、Pacer、RTT 和带宽采样 |
| Path | `c/src/connection/connection.c`、`c/src/mtu/` | 路径验证、迁移、抗放大和 MTU 探测 |
| Crypto | `c/src/crypto/` | X25519、HKDF、AEAD、票据和 0-RTT |
| Socket | `c/src/socket/` | UDP socket、地址、批量收发和本地地址信息 |
| Rendezvous | `c/src/rendezvous/` | NTRS 消息、候选地址、attempt_id 和打洞握手关联 |

所有模块都遵循 `c/STYLE.md` 和 `c/ERRORS.md` 的 C11、显式所有权和错误码约束。

详细入口：

- [公共 API 与错误码](api/公共API与错误码.md)
- [事件循环与统一写事件](event/事件循环与统一写事件.md)
- [加密与 0-RTT 实现细节](crypto/加密与0-RTT实现细节.md)
- [全包加密与 CID 混淆方案（未来设计，未落地）](crypto/全包加密与无状态可验证CID混淆方案.md)
- [Rendezvous 与握手关联](rendezvous/Rendezvous与握手关联.md)
