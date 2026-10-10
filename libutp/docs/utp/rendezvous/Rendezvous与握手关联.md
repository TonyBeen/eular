# Rendezvous、attempt_id 与握手关联

## 1. 连接尝试

每次 `utp_context_connect()` 或 `utp_context_connect_0rtt()` 都生成随机 16 字节 `attempt_id` 和本地 CID，并将目标 peer-id 写入 Rendezvous REQUEST。普通直连也使用同一连接尝试模型；只是没有 NTRS 控制面时，候选地址直接来自调用参数。

## 2. 候选计划与 NTRS 路径

REQUEST -> REDIRECT -> FORWARD -> PUNCH 是协调顺序。REDIRECT 和 FORWARD 只提供候选及 token，不承载业务数据。收到合法 PUNCH 后，主动端向触发它的 endpoint 重发保留的 Initial，并以该 PUNCH 的本地目的地址作为 UDP 源地址；之后按普通 Initial/Handshake/HandshakeDone 处理。

CandidatePlan 同时含本地候选和公网候选。公网候选是不可拆分的精确 `IP:port` 对，不能把多个地址与端口交叉组合；两类候选各最多四项。候选先按完整 endpoint 去重，IPv6 比较包含 scope，因此端口或 scope 不同仍是不同候选。地址族严格隔离，不跨 IPv4/IPv6 组合。

NTRS 收到 REQUEST 后，先向请求方发 REDIRECT，再向已注册目标发送 FORWARD。重复 REQUEST 保持同一 `attempt_id` 和 punch token，以幂等方式重发 REDIRECT，避免重复创建下行事务。目标不在线、候选地址族不匹配或没有可行候选时，NTRS 静默丢弃 REQUEST，避免 peer-id 在线状态被枚举。

收到 REDIRECT 的主动端向每个去重候选各发一次 PUNCH 和原始 Initial/0-RTT；收到 FORWARD 的被动端向每个去重候选各发一次 PUNCH。PUNCH 使用零 CID Rendezvous 包，消息体只有 8 字节 token；匹配 token 的接收端不回复 PUNCH，也不创建 Connection，只把来源记为路径反馈并向该来源重发保留的 Initial/0-RTT。获得合法 PUNCH 或握手反馈后，后续握手 PTO 只使用反馈路径，不再全候选 fan-out。未知 PUNCH 不缓存，直接静默丢弃。

握手阶段使用 `attempt_id`、本地 active CID 和候选 endpoint 验证来源；同一 Context 可以并发向多个 peer 建连。连接成功后，数据面主要按 DCID/SCID 解复用，attempt_id 仅用于握手完成、0-RTT 和 Rendezvous 短期清理。

## 3. 握手归并与 Endpoint 规则

收到哪个 endpoint 的控制包，就向该 endpoint 回包；候选列表仅用于尚未获得路径反馈时的 fan-out。完整 endpoint 比较包含地址族、地址、端口和 IPv6 scope；不同 endpoint 的迟到 Handshake 必须通过 CID、包号和加密校验后才能被接受。

被动侧 pending 握手按 `(attempt_id, active_scid)` 归并：相同 attempt 的多条候选路径复用同一个 pending、local CID、密钥和接收缓存，不重复通知应用；相同 attempt_id 携带不同 active SCID 直接丢弃。首个合法 Initial 决定握手参数，后到副本不以 endpoint 或重复的 TransportParams 覆盖已有状态。

每份合法 Initial 的 Handshake 都回到触发它的 `peer/local` 路径。若 UDP 暂不可写，待发响应必须保留该路径，不能被后到 Initial 覆盖。首个通过 DCID、SCID、已发送 Handshake 包号及适用 AEAD 校验的 HandshakeDone 以其来源路径完成晋升；另一候选上的迟到握手包不触发路径迁移，正常迁移仍走 PATH_CHALLENGE/PATH_RESPONSE。

## 4. 清理

握手超时、显式取消、连接成功或对端关闭后，Context 清理对应 attempt 索引和候选状态。连接晋升后，数据面只按 CID 解复用；Context 短期保留已完成 `(attempt_id, active_scid)`，用于抑制迟到的零 DCID Initial，但不会把 attempt_id 作为已建立 Connection 的长期身份。
