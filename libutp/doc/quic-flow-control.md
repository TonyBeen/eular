# libutp QUIC 风格流量控制实施计划

## 1. 背景与动机
目前，`libutp` 依赖拥塞控制（BBR），它能有效地管理网络路径限制，但无法保护接收端应用层的缓冲区。如果发送端的传输速度超过接收端应用层的消费（`read()`）速度，接收端缓冲区将会出现膨胀（Buffer Bloat），导致内存耗尽或触发被动丢包。为了解决这一问题，我们将实施严格的、类似于 QUIC 的基于信用的流量控制机制。

## 2. 范围与影响
*   **协议层 (Protocol Layer)：** 引入 4 种新的控制帧：`MAX_DATA`、`MAX_STREAM_DATA`、`DATA_BLOCKED` 和 `STREAM_DATA_BLOCKED`。
*   **状态机 (State Machine)：** 对于连接（Connection）和流（Stream），从基于相对序号的跟踪转变为基于**绝对字节偏移量 (Absolute Byte Offset)** 的跟踪。
*   **发送控制器 (Send Controller)：** 传出的 Payload 数据将同时受到拥塞窗口（`cwnd`）和对端宣告的绝对字节限制（接收窗口）的**双重限制**。

## 3. 建议方案
我们将采用“严格 QUIC 风格（连接级 + 流级限制）”方案：
1.  **双层限制：** 连接拥有一个全局最大绝对字节限制（`MAX_DATA`），每个多路复用的流也有各自的独立限制（`MAX_STREAM_DATA`）。
2.  **发送端关卡：** 在发送 `STREAM` 帧之前，发送端必须验证该传输既不会超过流限制，也不会超过连接限制。
3.  **阻塞通知：** 如果传输受限于流量控制（而拥塞控制允许发送），发送端将发出 `DATA_BLOCKED` 或 `STREAM_DATA_BLOCKED` 帧，用于诊断和触发更新。
4.  **接收端窗口更新：** 当接收端应用层消费数据时，接收端会计算释放的缓冲区空间，并通过发送新的 `MAX_DATA` / `MAX_STREAM_DATA` 帧来宣告更高的限制。

## 4. 备选方案考虑
*   **仅连接级流量控制：** 已否决。虽然实现较简单，但会遇到队头阻塞（Head-of-Line blocking）问题，即单个消费缓慢的流可能会占满整个全局缓冲区，导致连接上的其他活跃流被饿死。

## 5. 分阶段实施计划

### 第一阶段：协议帧定义 (Protocol Frame Definitions)
*   在 `libutp/src/proto/frame/` 中创建以下头文件和源文件：
    *   `max_data.h` / `.cpp`
    *   `max_stream_data.h` / `.cpp`
    *   `data_blocked.h` / `.cpp`
    *   `stream_data_blocked.h` / `.cpp`
*   为每个帧实现 `SerializeTo` 和 `DeserializeFrom`，确保正确处理绝对偏移量字段（`uint64_t`）。

### 第二阶段：状态追踪升级 (State Tracking Additions)
*   **`Connection` 类：** 添加对 `m_tx_max_data`（对端限制）、`m_tx_absolute_offset`（累计发送总计）、`m_rx_max_data`（给对端的限制）和 `m_rx_absolute_offset`（累计接收总计）的追踪。
*   **`Stream` 类：** 添加类似的追踪变量：`m_tx_max_stream_data`、`m_tx_absolute_offset`、`m_rx_max_stream_data`、`m_rx_absolute_offset`。
*   **配置：** 添加初始窗口大小配置参数（例如 `initial_max_data`）。

### 第三阶段：发送端关卡与阻塞逻辑 (Sender Gatekeeping & Blocked Logic)
*   修改发送循环（例如在 `SendCtl` / `try_send` 逻辑内部），以评估剩余的流量控制配额：
    *   `conn_quota = m_tx_max_data - m_tx_absolute_offset`
    *   `stream_quota = m_tx_max_stream_data - m_tx_absolute_offset`
*   如果 `quota == 0`，则将流/连接转换为阻塞状态，并生成相应的 `*_BLOCKED` 帧。

### 第四阶段：接收端窗口更新逻辑 (Receiver Window Update Logic)
*   在流的 `read()` API 中埋点，捕获应用层消费缓冲字节的行为。
*   重新计算接收窗口。如果可用缓冲区超过阈值（例如 50% 为空），则排队发送 `MAX_DATA` 或 `MAX_STREAM_DATA` 帧以通知发送端。

## 6. 验证与测试
*   **单元测试：** 验证新帧的序列化/反序列化。测试绝对偏移量的边界数学计算。
*   **集成测试：** 修改 `netem_test.sh` 以模拟“慢速读取者 (Slow Reader)”场景（服务器在 `read()` 调用之间进行 `sleep()`）。验证发送端是否能准确暂停发送、发出 `BLOCKED` 帧，并在收到窗口更新后干净利落地恢复。

## 7. 迁移与回滚
*   如果需要严格向后兼容，初始连接握手必须协商流量控制能力。如果对端不支持，则回退到旧行为或终止连接。
*   所有更改将隔离在协议和状态层中，如果发生系统性回归，可以通过 Git 轻松回滚代码。
