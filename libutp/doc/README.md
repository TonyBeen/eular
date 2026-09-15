# libutp 实现文档

实现细节按模块组织。当前 C11 实现、公共 API 和线格式以 `c/include/utp/`、`c/src/` 及本文档目录为准；`docs/superpowers/` 保留为历史设计和迁移参考，不应覆盖当前 C11 代码语义。

## 核心传输模块

- [Context 与连接](utp/context/连接与异常处理流程.md)
- [包与帧](utp/proto/包与帧实现细节.md)
- [Stream 与流控](utp/stream/流与流控实现细节.md)
- [ACK 与重传](utp/reliability/ACK与重传实现细节.md)
- [拥塞控制](utp/congestion/bbr.md)、[CUBIC](utp/congestion/cubic.md)
- [路径验证与 MTU](utp/path/路径验证与MTU实现细节.md)
- [加密与 0-RTT](utp/crypto/加密与0-RTT实现细节.md)
- [Socket 收发](utp/socket/Socket收发实现细节.md)

## 服务与工程专题

- [NAT 探测](nat/NAT探测实现细节.md)
- [NTRS 服务](ntrs/NTRS服务实现细节.md)
- [内存池与对象生命周期](memory/内存池与对象生命周期.md)
- [读写链路内存流转](memory/读写链路内存流转分析.md)
- [有序分片与 PacketIn 引用](memory/有序分片与PacketIn引用模型.md)
- [默认值与调参](config/默认值与调参指南.md)
- [测试计划](testing/测试计划.md)
- [当前实现状态](设计实现文档.md)
