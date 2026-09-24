# 公共 API 与错误码

公共头文件位于 `utp/include/utp/`，句柄类型是不透明指针，Context 持有 Connection，Connection 持有 Stream。除回调执行期间的只读视图外，应用不得保存内部缓冲或在对象回收后继续使用借用指针。

## Context 入口

- `utp_context_create()` / `utp_context_destroy()`：创建和销毁 Context；event base 由调用方拥有。
- `utp_context_bind()`：绑定一个 UDP socket，可指定地址、端口和网卡。
- `utp_context_connect()`：异步主动建连，目标 `address`、`port`、`target_peer_id` 必填。
- `utp_context_accept()`：放行 `utp_on_new_connection_fn` 接受的 pending 被动连接。
- `utp_context_probe_nat()`、`utp_context_register_ntrs()`、`utp_context_unregister_ntrs()`：分别管理 NAT 探测和 NTRS 关联。

所有 API 返回 `utp_status_t`。同步参数/状态错误由返回值报告；已创建连接后的异步握手或连接错误通过 Context 回调报告。`utp_context_destroy()` 不隐式反注册，也不触发建连失败回调。

## Connection 和 Stream

`utp_connection_create_stream()` 创建由 Connection 持有的流；`utp_connection_get_stream()`、`utp_stream_*` 返回的指针均为借用指针。应用使用 `utp_connection_set_on_incoming_stream()` 和 `utp_stream_set_on_readable/writable/closed()` 接收事件。

## 状态码分组

错误码定义在 `utp/include/utp/status.h`，包括：

- 通用：`INVALID_ARGUMENT`、`NOMEM`、`LIMIT`、`STATE`、`WOULD_BLOCK`、`TIMEOUT`、`CLOSED`、`CANCELLED`；
- Socket/Context：`SOCKET_*`、`CONTEXT_*`；
- Stream/帧：`STREAM_*`、`FRAME_FORMAT`、`FRAME_UNEXPECTED`；
- Crypto：`CRYPTO_*`、`AUTH`、`RANDOM_GENERATION`；
- Connection/Rendezvous：`CONNECTION_*`、`RENDEZVOUS_*`。

用 `utp_status_string()` 获取稳定的诊断文本；不要依赖内部 `utp_internal_error_t` 数值。
