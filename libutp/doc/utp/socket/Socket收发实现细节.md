# Socket 收发实现细节

## 1. Socket 所有权

一个 Context 持有一个 UDP socket 和一个本地端口。`c/src/socket/udp.c` 负责创建、绑定、非阻塞收发、地址转换、批量 I/O 和本地目的地址信息；事件注册由 Context 负责。

## 2. 收发路径

- Linux 优先使用 `recvmmsg`/`sendmmsg`，其他平台使用单包接口或平台等价实现。
- UDP socket 接收调用方提供的 buffer；Context 在 `udp.c` 返回后从 PacketIn 池取得对象，保存数据、peer/local endpoint，再交给解复用逻辑。UDP 层本身不拥有 PacketIn 池。
- 出站 PacketOut 携带显式 destination 时按该地址发送，否则使用 Connection 当前 peer。
- 本地地址信息用于多网卡和 IPv6 scope 选择，不能用 endpoint 文本替代完整地址比较。

## 3. 错误处理

`WOULD_BLOCK` 只表示当前 I/O 时机不可用，保留队列并等待统一 writable 事件。`EMSGSIZE` 由 MTU 探测路径消费；其他 socket 致命错误映射为内部错误，由上层统一处理。
