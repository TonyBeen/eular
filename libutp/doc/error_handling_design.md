# 错误处理机制优化设计与实施计划

## 1. 背景与动机
目前，`libutp` 在一个扁平的 `utp_error_t` 枚举中混合了流级别错误、连接级别错误和 Socket 错误。内部错误传播严重依赖原生的 `int32_t` 返回值，导致 `-UTP_ERR_*` 和 `UTP_ERR_*` 之间存在语义混淆。此外，系统缺乏一个集中且健壮的机制来处理致命的协议错误（即终止连接），并且在边界层尚未完全实现 POSIX API 错误契约（即失败时返回 `-1` 并设置 `GetLastError()`）。借鉴 `lsquic` 的设计思路，本计划旨在重构内部错误处理机制，使其具备强类型、分层化和高健壮性的特点。

## 2. 范围与影响
*   **影响范围:** 
    *   引入一个新的 `Status` 工具类用于内部错误传播。
    *   重构 `ConnectionImpl`，实现集中式的连接终止（Abort）机制。
    *   重构内部函数（例如：数据包解析、流 I/O），将其返回值改为 `Status`。
    *   更新公共 API 边界（`utp.cpp`、`stream.cpp`）以严格执行 POSIX 错误契约。
*   **架构影响:** 修改主要局限于内部组件和公共 API 包装层。底层的二进制帧结构和协议状态机行为保持不变，但错误恢复能力和 API 易用性将得到显著提升。

## 3. 建议方案

### 3.1 错误语义分层 (Error Taxonomy)
在概念和结构上，将错误分为三大类：
1.  **瞬态/阻塞型错误 (Transient/Blocking):** 例如 `UTP_ERR_WOULD_BLOCK`, `UTP_ERR_IN_PROGRESS`。这类错误会立即返回给应用层，连接和流的状态保持不变。
2.  **流级致命错误 (Stream-Fatal):** 例如 `UTP_ERR_STREAM_FLOW_CONTROL`。这类错误仅触发 `RESET_STREAM` 或 `STOP_SENDING`，只关闭特定的流，整个连接保持存活。
3.  **连接级致命错误 (Connection-Fatal):** 例如 `UTP_ERR_FRAME_FORMAT_ERROR`, `UTP_ERR_CRYPTO_DECRYPTION`。这类错误会立即暂停所有 I/O，将状态机切换至 `CLOSING`，发送 `CONNECTION_CLOSE` 帧，并最终销毁连接。

新增细化错误码（减少 `INVALID_STATE/INVALID_PARAM` 语义过载）：
* `UTP_ERR_PATH_VALIDATION_BLOCKED`: 路径校验阶段受 anti-amplification 限制，发送被抑制。
* `UTP_ERR_SESSION_TOKEN_UNAVAILABLE`: 当前连接无可导出的会话票据。
* `UTP_ERR_RESUMPTION_STATE_UNAVAILABLE`: 当前连接无可导出的恢复状态。
* `UTP_ERR_CONTEXT_UNAVAILABLE`: 连接上下文不可用。

### 3.2 集中式连接终止机制 (`ConnectionImpl`)
在 `ConnectionImpl` 中增加 `abortConnection` 方法，确保在遇到致命错误时能干净地清理资源：
```cpp
void ConnectionImpl::abortConnection(utp_error_t localErrCode, uint16_t quicTransportErrCode, const char *reason) {
    if (m_state >= kStateCloseSent) return;

    m_lastErrorCode = localErrCode;
    m_lastErrorReason = reason;
    m_closeErrorCode = quicTransportErrCode;

    m_state = kStateCloseSent; 
    m_sendCtl->flushAndClear(); 
    stopAckTimer();
    m_keepaliveTimer.stop();

    sendConnectionCloseFrame();
    armCloseDrainTimer(); 
    notifyConnectionClosed(localErrCode, reason, false);
}
```

### 3.3 API 边界归一化
在公共边界（例如 `utp.cpp` 和公共 Stream API）强制执行 POSIX 契约：
*   **状态型 API 成功:** 返回 `0`。
*   **值型 API 成功:** 返回正数（例如，读取或写入的字节数）。
*   **失败:** 拦截内部返回的 `Status`，调用 `SetLastErrorV(status.code(), ...)`，并返回 `-1`。

补充约束（实现硬规则）：
*   `SetLastErrorV` 仅允许在对外 API 边界层调用（如 `utp.cpp` 或 Stream/Connection 对外包装入口）。
*   `ConnectionImpl`、`StreamImpl`、`crypto/*` 等内部实现层禁止直接调用 `SetLastErrorV`，内部仅返回错误码/状态并通过日志和错误回调传播。
*   连接关闭语义统一：`error_code == 0` 视为正常关闭，`error_reason = "ok"`；仅当 `error_code != 0` 时才视为异常关闭并使用对应错误原因。

## 4. 分阶段实施计划

*   **第一阶段:** 实现 `src/util/status.h`，并为 `Status` 类编写基础单元测试。
*   **第二阶段:** 在 `ConnectionImpl` 中实现 `abortConnection` 方法，并替换核心数据包处理流程（如 `onUdpPacket`、解密失败）中现有的碎片化错误处理，统一使用新的 abort 机制。
*   **第三阶段:** 重构 `StreamImpl` 中的相关方法，将其返回值从 `int32_t` 改为 `Status`，并确保流级致命错误与连接错误隔离。
*   **第四阶段:** 更新公共 API 包装器（在 `utp.cpp` 和 `stream.cpp` 中），将内部的 `Status` 返回值映射为 `posix-api-error-contract` 规范中要求的 `-1` 和 `GetLastError()` 语义。

## 5. 验证与测试
*   编译项目，确保所有 `Status` 传播和边界归一化的代码修改在语法上正确。
*   运行现有的测试用例集（如 `test_context_integration.cc`, `test_stream_impl.cc` 等）。
*   添加特定的故障注入测试（例如：模拟损坏的数据帧或解密失败），以验证 `ConnectionImpl::abortConnection` 能够被正确触发，并且能在不崩溃的情况下干净地关闭连接。
*   验证公共 API 在失败时是否能通过 `GetLastError()` 正确设置预期的 `UTP_ERR_*` 错误码。

## 6. 迁移与回滚策略
*   **迁移:** 此变更主要集中在内部。公共 API 的行为将遵循新制定的 POSIX 契约。依赖于旧版正负返回值错误判断的用户代码需要适配并改为使用 `GetLastError()`。
*   **回滚:** 如果在集成过程中发现严重问题，可以直接撤销引入 `Status` 类和边界归一化的提交，回退到原有的 `int32_t` 返回值风格。
