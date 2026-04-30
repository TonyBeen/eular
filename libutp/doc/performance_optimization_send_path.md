# libutp 发送路径性能优化方案：基于 BoringSSL 流式加密与 RingBuffer 的减拷贝设计

## 1. 背景与目标

### 1.1 现状分析 (现状：2-3 次拷贝)
目前 `libutp` 在发送加密 Stream 数据时存在多次内存拷贝：
1. **用户到 RingBuffer**：`User Buffer` -> `StreamImpl::m_sendBuffer` (memcpy)。
2. **明文拼接**：`RingBuffer` + `Frame Header` -> `m_payloadScratch` (std::vector 拼接，memcpy)。
3. **加密到 Packet**：`m_payloadScratch` -> `PacketOut::raw_data` (AES-GCM 加密并拷贝)。

### 1.2 优化目标
实现“一次性搬运”：
- **加密流量**：缩减为 **1 次大块拷贝**（数据在加密的同时，直接从 `RingBuffer` 写入 `PacketOut`）。
- **非加密流量**：配合 `sendmsg` (iovec)，实现 **0 次大块拷贝**（直接引用 `RingBuffer` 内存发送）。

---

## 2. 核心设计：三段式 RingBuffer 与帧元数据管理

### 2.1 三段式 RingBuffer 指针
为了支持 **MTU 动态重组** 和 **可靠重传**，将 RingBuffer 视为数据的“持久化层 (Source of Truth)”：
1. **`Read_Ptr` (Cumulative Acked)**：已确认数据末尾，此时才真正释放 RingBuffer 空间。
2. **`Send_Ptr` (In-Flight)**：逻辑已发出偏移，`[Read_Ptr, Send_Ptr]` 之间为已发待确认明文。
3. **`Write_Ptr` (App Data)**：应用层已提交的数据末尾。

### 2.2 PacketOut 的瘦身：从“字节缓存”到“帧元数据容器”
为了解决重传时非持久化帧（如 ACK）的剔除问题，并降低内存压力：
- **逻辑模型**：`PacketOut` 不再常驻持有加密后的密文 `raw_data`。
- **存储内容**：`PacketOut` 仅记录其包含的帧清单（`FrameMetaInfo`），包括每个帧在 `RingBuffer` 中的 Offset、Length 以及帧类型。
- **发送流程**：
  1. **初次发送**：利用流式加密，从 `RingBuffer` 拉取明文，即时生成密文并发出。发出后，**立即释放 `PacketOut` 的密文缓冲区**。
  2. **重传触发**：当检测到丢包，调度器扫描 `PacketOut` 的帧清单，剔除失效帧（如过时的 ACK），保留必要帧（如 STREAM）。
  3. **动态重构**：使用**全新的 Packet Number**，利用流式加密重新从 `RingBuffer` 读取明文并构造新的 UDP 包。

---

## 3. 详细设计变更

### 3.1 基于流式加密的封装 (AesGcmContext)
利用 BoringSSL 的 `EVP_EncryptUpdate` 实现“边拼接边加密”：
1. `encryptStart`: 处理 AAD (UTP 头部)。
2. `encryptUpdate(FrameHeader)`: 加密帧头并写入临时 UDP 缓冲区。
3. `encryptUpdate(RingBufferData)`: 直接从 `RingBuffer` 视图加密并写入临时 UDP 缓冲区。
4. `encryptFinal`: 生成 Tag。

### 3.2 鲁棒性改进
- **MTU 适配**：重传时若发现 MTU 变小，可将原 `PacketOut` 中的多个帧拆分到多个新的物理包中，因为明文在 `RingBuffer` 中始终可用。
- **内存极致优化**：未确认队列中不再存储重复的密文字节，对于大窗口高吞吐场景，内存占用可降低 90% 以上。

---

## 4. 关键漏洞防范

- **ACK 歧义消除**：由于重传使用新 PN 并重新加密，解决了传统重传无法区分 RTT 的问题。
- **SACK 逻辑**：`Read_Ptr` 依然严格遵守累积确认。被 SACK 的片段仅标记为已送达，但不从 `RingBuffer` 物理释放，直到空洞补齐。
- **Nonce 安全**：每次重传生成的密文包对应唯一的 PN，完全符合 AES-GCM 的安全规范。

---

## 5. 实施步骤

1. **Phase 1**: 在 `AesGcmContext` 中实现基于 `EVP_CIPHER_CTX` 的流式加密封装。
2. **Phase 2**: 修改 `StreamImpl::RingBuffer`，实现延迟消费逻辑及三段指针管理。
3. **Phase 3**: 重构 `PacketOut` 与 `unacked_queue` 逻辑，支持基于帧清单的动态重写。
4. **Phase 4**: 修改 `ConnectionImpl` 发送路径，接入流式加密并移除所有 `scratch` 缓冲区。
5. **Phase 5**: 优化非加密路径，通过 `PacketOut` 的元数据直接通过 `iovec` 引用 `RingBuffer` 内存实现 0 拷贝发送。
