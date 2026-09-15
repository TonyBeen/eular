# 加密与 0-RTT 实现细节

## 1. 密码学组件

`c/src/crypto/crypto.c` 提供 X25519 共享密钥、HKDF 派生、方向隔离的 AES-GCM key/nonce，以及包级加解密。发送和接收方向使用不同的密钥材料；解密失败的包不进入连接状态机。

## 2. SessionToken

`c/src/crypto/token.c` 负责编解码恢复票据和恢复状态。Context 保存恢复根密钥、票据有效期和短窗口重放表。票据校验失败、过期或重放时，0-RTT 请求被拒绝并回到普通握手路径。

## 3. 0-RTT

- 主动端可携带票据和早期数据发送 0-RTT。
- 被动端先校验票据、地址绑定、包号和重放状态，再按 `on_new_connection` 决定是否交付早期数据。
- 服务端 Handshake 响应保留待确认状态；客户端用 HandshakeDone 确认实际收到的响应。
- 响应丢失时由 Context 定时器重发，不要求应用再次调用 connect。
- 早期数据可能重放，应用只能放入幂等或自行去重的数据。
