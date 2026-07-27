# UTP-11 需求文档：Socket 收发 / 地址 / 批量收发（mmsg）

> 本文档由现有 C++ 实现反推。**代码为唯一 ground truth**（`cpp/src/socket/`），doc/ 仅交叉参考，不一致处以代码为准并在 §8 标注。所有引用格式为 `文件:行号`，数值均来自代码，无法确定处标"待确认"。
>
> 覆盖文件：`cpp/src/socket/{udp,mmsg,address,packet,util}.{cpp,h}`。

---

## 1. 职责与边界

本模块是 UTP 协议栈的**最底层网络 I/O 抽象**，封装单个非阻塞 UDP socket 的生命周期、地址表示与批量收发。

**职责：**
- UDP socket 的创建、选项设置、绑定、关闭（`UdpSocket`，`cpp/src/socket/udp.cpp:191`；`Socket` 静态工具，`cpp/src/socket/util.cpp:432`）。
- 数据报收发：单包 / 批量（`sendmmsg`/`recvmmsg`），支持分片（iovec）零系统调用拼包（`cpp/src/socket/udp.cpp:343`、`:483`）。
- 地址表示与转换（`Address`，v4/v6 双栈，`cpp/src/socket/address.{h,cpp}`）。
- 从内核错误队列读取 ICMP 错误消息（仅 Linux，`cpp/src/socket/udp.cpp:280`）。
- 提取每包元信息 `MsgMetaInfo`（peer 地址、local 地址、fd）。

**边界（不负责）：**
- 不做 UTP 协议解析、连接管理、拥塞控制（属上层 `context`/`connection`）。
- 不做事件循环 / epoll 注册（由 `ContextImpl` 用 `m_udpSocket.fd()` 注册，`cpp/src/context/context_impl.cpp:521,525`）。
- 不管理 `MsgMetaInfo.data` 指向的接收缓冲生命周期以外的内存（发送缓冲由调用方持有）。
- `packet.cpp` 是空的 `main()` 占位文件（`cpp/src/socket/packet.cpp:11`），无实际逻辑；`packet.h` 仅定义 `PacketMetaInfo` 结构（`cpp/src/socket/packet.h:16`）。

### 1.1 一个 Context 一个 socket / 一个端口（关键结论）

- `ContextImpl` 以**值成员**持有单个 `UdpSocket m_udpSocket`（`cpp/src/context/context_impl.h:226`）。
- 该 Context 下所有连接共享此 socket：`ConnectionImpl` 构造时接收 `&m_udpSocket` 指针（`cpp/src/context/context_impl.cpp:502`、`:1041`）。
- 因此 **一个 Context = 一个 UDP socket = 一个绑定端口**。连接解复用（区分不同 peer / scid）必须在应用层基于 `MsgMetaInfo.peerAddress` 完成，内核层面所有连接的数据都从同一 fd 收上来。

---

## 2. 数据结构

### 2.1 `UdpSocket`（`cpp/src/socket/udp.h:23`）
| 成员 | 类型 | 说明 |
|---|---|---|
| `m_sock` | `socket_t` | 初值 `INVALID_SOCKET`（`udp.h:81`） |
| `m_bindAddr` | `Address` | 用户请求绑定的地址（`udp.h:82`） |
| `m_localAddr` | `Address` | `getsockname` 实际地址，绑定 0 端口时用于回填真实端口（`udp.h:83`，`udp.cpp:266`） |
| `m_config` | `const Config&` | 引用配置（`udp.h:84`） |
| `m_mmsg` | `MultipleMsg` | 仅 `USE_SENDMMSG` 时存在（`udp.h:86-88`） |
| `m_recvBuffer` | `ByteBuffer` | 非 mmsg 路径 / 错误队列的接收缓冲，容量 `reserve(UTP_ETHERNET_MTU)`=1500（`udp.cpp:38`） |
| `m_tag` | `std::string` | 日志标签（`udp.h:90`） |

常量：`kMaxMsgSlices = 4`（`udp.h:26`）——单包最多 4 个 iovec 分片。

### 2.2 `UdpSocket::MsgMetaInfo`（`udp.h:33`）——收发的统一载体
| 字段 | 说明 |
|---|---|
| `data` / `len` | 单块缓冲指针与长度 |
| `slice_count` | 分片数（0 表示用 `data/len`，>0 表示用 `slices[]`） |
| `slices[kMaxMsgSlices]` | `MsgSlice{const void* data; size_t len}`（`udp.h:28`），发送时拼成 iovec |
| `metaInfo` | `PacketMetaInfo`（fd / localAddress / peerAddress） |

### 2.3 `PacketMetaInfo`（`cpp/src/socket/packet.h:16`）
`int32_t fd; Address localAddress; Address peerAddress;`

### 2.4 `UdpSocket::ErrorMsg`（`udp.h:41`）
ICMP 错误：`ee_type/ee_code/ee_info`（各为 vector）、`data/len`、`peer_addr`。

### 2.5 `Address`（`cpp/src/socket/address.h:37`）
- `m_family`（`NONE=AF_UNSPEC / IPv4=AF_INET / IPv6=AF_INET6`，`address.h:40`）、`m_port`（主机字节序）、`union{v4[4]; v6[16]}`（网络字节序原始字节，`address.h:109`）。
- 提供 `AnyIPv4/AnyIPv6/LoopbackIPv4/LoopbackIPv6`（`address.cpp:41-67`）、`Parse("ip:port"/"[ipv6]:port")`（`address.cpp:69`）。
- 分类谓词：`isLoopback/isAny/isMulticast/isPrivate/isLinkLocal`（`address.cpp:271-338`）。
- 支持 `==/!=/</hash`，并特化 `std::hash<Address>`（`address.h:119`），可作 map/set 键。

### 2.6 `MultipleMsg`（`cpp/src/socket/mmsg.h:24`，仅 `USE_SENDMMSG`）
- 单块 `malloc` 承载 `m_nMsg` 组：`mmsghdr + sockaddr_storage + iovec + data(m_mss) + control(CMSG_SPACE(in6_pktinfo))`（布局见 `mmsg.cpp:124-160`）。
- 构造：`MultipleMsg(MAX_MMSG_SIZE, UTP_ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE)`（`udp.cpp:29`），即 size=32、mss=1500-20-8=**1472**。
- 每 slot 预置 `msg_namelen=sizeof(sockaddr_storage)`、`msg_iovlen=1`、`msg_control/controllen` 指向 cmsg 区（`mmsg.cpp:56-70`）。

---

## 3. 流程

### 3.1 绑定 `bind(ip, port, ifname)`（`udp.cpp:191`）
1. 用 `ip/port` 构造 `Address`，非法则返回 `UTP_ERR_INVALID_PARAM`（`udp.cpp:193-196`）。
2. 最多 **3 次**尝试（`kMaxOpenAttempts=3`），失败后 `sleep 20ms` 重试（`udp.cpp:199-201,272-274`）。
3. 每次尝试内按序执行（任一失败即 `break` → close → 重试）：
   `Socket::Open(family)` → `SetNonBlock` → `SetReuseAddr` → `SetReusePort` → `SetDontFragment` → `SetRecvError` → （IPv6 非 Any 时）`SetIPv6Only` → （ifname 非空）`SetBindInterface` → `SetRecvBufferSize` → `SetSendBufferSize` → `SetIPTos` → `SetNoSigPipe` → `Socket::Bind`（`udp.cpp:216-263`）。
4. 成功后记录 `m_bindAddr`，并 `m_localAddr.fromSocket(m_sock)` 回填真实地址（`udp.cpp:265-266`）。

### 3.2 接收 `recv(msgVec, status)`（`udp.cpp:343`）
- **`USE_SENDMMSG` 路径**：`recvmmsg(m_sock, m_mmsg[0], size=32, 0, nullptr)` 一次收多包（`udp.cpp:346`）。按需 `resize(msgVec)` 到 n，逐包填 `len/data/peerAddress`，`localAddress` 由 cmsg 提取（`GetIPPktInfo`），无效则回退到 `m_localAddr`（或 `m_bindAddr`）（`udp.cpp:364-378`）。
- **非 mmsg 路径**：Linux/Apple 用 `recvmsg`（含 cmsg 缓冲），Windows 用运行期取指针的 `WSARecvMsg`（`udp.cpp:383-451`）；每次只收 1 包写入 `msgVec[0]`（`udp.cpp:460-472`）。
- 返回值：`>0`=包数；`0`=`EAGAIN/EWOULDBLOCK`（无数据）；`<0`=错误并置 `status`（`udp.cpp:347-355`）。

### 3.3 发送
- 单包 `send(msg)` → `SendSingleMsgImpl`（`udp.cpp:56,478`）：校验 socket 有效、peerAddress 有效、data/slices 有效；有分片走 `sendmsg`（Linux）/`WSASendTo`（Windows），否则 `sendto`；均带 `MSG_NOSIGNAL`（Linux，`udp.cpp:155,160`）。
- 批量 `send(msgVec, count)`（`udp.cpp:483`）：`USE_SENDMMSG && OS_LINUX && count>1` 时用 `sendmmsg`，按 `MAX_MMSG_SIZE=32` 分批（`udp.cpp:494-583`）；否则退化为逐包 `SendSingleMsgImpl` 循环（`udp.cpp:586-601`）。
- 返回值语义：`>0`=成功发送包数；`0`=无可发/全被 `EAGAIN` 挡回；`<0`=首包即失败。部分成功返回已发送数（`udp.cpp:571,575,594-598`）。

### 3.4 ICMP 错误读取 `recvErrorMsg`（`udp.cpp:280`，仅 Linux）
`recvmsg(MSG_ERRQUEUE|MSG_NOSIGNAL|MSG_DONTWAIT)`，遍历 cmsg 过滤 `IP_RECVERR` + `SO_EE_ORIGIN_ICMP/ICMP6`，收集 `ee_type/ee_code/ee_info`（`udp.cpp:301-333`）。非 Linux 直接返回 0。

---

## 4. 不变量与规则（MUST / MUST NOT）

- **MUST** 在收发前保证 socket 有效（`isValid()`，`udp.h:60`）；`SendSingleMsgImpl` 首先校验（`udp.cpp:58`）。
- **MUST** 发送时 `peerAddress.isValid()` 且 `toSockAddr` 返回非 0，否则视为"无可发"返回 0（`udp.cpp:63-74`）。
- **MUST** socket 设为非阻塞（`SetNonBlock`，`udp.cpp:217`）；所有收发把 `EAGAIN/EWOULDBLOCK`（Windows `WSAEWOULDBLOCK`）当作"无数据/暂不可写"返回 0，而非错误（`udp.cpp:118,173,304,349,401,444,561`）。
- **MUST** 设置不分片，避免 PMTU 发现失效（`SetDontFragment`，`udp.cpp:230`，注释 `udp.cpp:229`）——Linux 用 `IP_MTU_DISCOVER=IP_PMTUDISC_DO`（`util.cpp:52`）。
- **MUST NOT** 在 macOS 上依赖 DF：Apple 无可移植 UDP 等价项，`SetDontFragment` 直接返回 0（不设置，`util.cpp:54-60`，注释在 `util.cpp:55-57`）。
- **MUST** 绑定 IPv6 且非 `AnyIPv6` 时设 `IPV6_V6ONLY`；绑定 `::`（Any）时保持双栈（`udp.cpp:238-242`，注释 `udp.cpp:238`）。
- **MUST** 分片发送时跳过空分片（data==null 或 len==0），全空则返回 0（`udp.cpp:84-93,136-147`）。
- 单包分片数 **MUST NOT** 超过 `kMaxMsgSlices=4`，循环以 `i < slice_count && i < kMaxMsgSlices` 硬截断（`udp.cpp:83,136,521`）。
- 发送 **MUST NOT** 挂 `msg_control`（批量 sendmmsg 显式置 `msg_control=nullptr, controllen=0`，`udp.cpp:544-545`）。
- `Address` 内部 v4/v6 字节 **MUST** 为网络字节序，`m_port` 为主机字节序（`address.cpp:132-143`）。

---

## 5. 参数与默认值

| 参数 | 值 | 来源 |
|---|---|---|
| `kMaxMsgSlices`（单包最大分片） | 4 | `udp.h:26` |
| `MAX_MMSG_SIZE`（批量收发上限） | 32 | `mmsg.h:18-20` |
| mmsg 单包缓冲 mss | 1472（`1500-20-8`） | `udp.cpp:29` |
| `m_recvBuffer` 预留容量 | 1500（`UTP_ETHERNET_MTU`） | `udp.cpp:38`；`mtu/mtu.h:17` |
| `UTP_ETHERNET_MTU` | 1500 | `cpp/src/mtu/mtu.h:17` |
| `IPV4_HEADER_SIZE` / `UDP_HEADER_SIZE` | 20 / 8 | `mtu/mtu.h:18,20` |
| `recv_buf_size`（`SO_RCVBUF`） | 1024*1024 = 1 MiB | `include/utp/config.h:84` |
| `send_buf_size`（`SO_SNDBUF`） | 1024*1024 = 1 MiB | `include/utp/config.h:85` |
| 绑定重试次数 / 间隔 | 3 次 / 20 ms | `udp.cpp:199-200` |
| ICMP cmsg 缓冲 | 1024 字节 | `udp.cpp:287` |
| IP TOS（Linux） | `0xA0`（DSCP CS5） | `util.cpp:207` |
| IP 服务类型（Apple） | `NET_SERVICE_TYPE_VO` | `util.cpp:215` |
| `INVALID_SOCKET` | -1（POSIX） | `cpp/src/commom.h:47,82` |

---

## 6. 接口

### 6.1 `UdpSocket`（`udp.h`）
- `UdpSocket(Config&)` / `~UdpSocket()`
- `Status bind(const std::string& ip, uint16_t port, const std::string& ifname)`
- `int32_t recv(std::vector<MsgMetaInfo>&, Status&)`
- `int32_t send(const MsgMetaInfo&, Status&)`
- `int32_t send(const MsgMetaInfo*, size_t count, Status&)`
- `int32_t send(const std::vector<MsgMetaInfo>&, Status&)`
- `int32_t recvErrorMsg(ErrorMsg&, Status&)`
- `bool isValid()`、`socket_t fd()`、`void updateTag(...)`、`const std::string& tag()`

### 6.2 `Socket`（静态工具，`util.h:19`）
- `Socket::Open(family, status)` / `Bind(fd, addr)` / `Close(fd)`
- `Socket::Ioctl::{SetNonBlock, SetReuseAddr, SetReusePort, SetDontFragment, SetBindInterface, SetRecvError, SetSendBufferSize, SetRecvBufferSize, SetPktInfoV4, SetPktInfoV6, SetIPv6Only, SetIPTos, SetNoSigPipe, GetMtuByIfname}`
- `Socket::Util::GetIPPktInfo(msghdr, port) -> Address`（从 cmsg 提取本地目的地址）

### 6.3 `Address`（`address.h`）
构造 / `Parse` / `parse` / `fromSockAddr*` / `fromSocket` / `parseHostPort` / `toSockAddr*` / `toIpString` / `toString` / `family` / `port` / `isValid` / `isIPv4` / `isIPv6` / 分类谓词 / `==` `!=` `<` `hash`。

### 6.4 返回码约定
- socket 操作返回 `Status`（错误码如 `UTP_ERR_SOCKET_CREATE/BIND/READ/WRITE/IOCTL`，见 `util.cpp`）。
- `Ioctl::Set{NonBlock,ReuseAddr,ReusePort,DontFragment}` 返回 `int32_t`（0 成功 / <0 或非 0 失败，来自 `setsockopt`/`fcntl` 原值）；其余 `Set*` 返回 `Status`。

---

## 7. 当前实现边界（限制 / 待确认）

1. **`IP_PKTINFO`/`IPV6_RECVPKTINFO` 从未启用**：`SetPktInfoV4`/`SetPktInfoV6` 有定义但 `bind()` 从未调用（全仓仅 `util.{h,cpp}` 出现，`grep` 无其他 caller）。因此内核不会填充 pktinfo cmsg，`GetIPPktInfo` 实际总返回无效 `Address`，`MsgMetaInfo.localAddress` **始终回退**到 `m_localAddr`/`m_bindAddr`（`udp.cpp:374-376,469-471`）。收 cmsg 的解析路径当前为事实上的死代码。**待确认**是否有意为之。
2. **Windows `SetReusePort` 实为 `SO_REUSEADDR`**：`util.cpp:37-39` 在 Windows 分支设的是 `SO_REUSEADDR`（Windows 无 `SO_REUSEPORT`），与 Linux 的 `SO_REUSEPORT`（`util.cpp:42`）语义不同。
3. **`SetReusePort` 失败即导致 bind 失败**：`bind` 中 `if (Socket::Ioctl::SetReusePort(m_sock)) break;`（`udp.cpp:225`）——setsockopt 非 0 返回会中断绑定并重试，`SO_REUSEPORT` 不可用的平台可能受影响。**待确认**是否应容忍失败。
4. **`GetIPPktInfo` 非 Windows 分支存在无 return 的路径**：Linux/Apple 分支循环后有 `return addr`（`util.cpp:378,402`），但 Windows `#else` 分支若无匹配 cmsg 会走到函数末尾**无显式 return**（`util.cpp:429`），UB 风险。**待确认**。
5. **`MSG_ZEROCOPY` 未实现**：`udp.h:76-77` 明确标注为 `TODO(next)`。
6. **`recvErrorMsg` 仅 Linux**：Apple/Windows 返回 0（`udp.cpp:336-340`）。
7. **`parseHostPort` 端口解析用 `std::stoi` 无 try/catch**：非法端口字符串会抛异常（`address.cpp:180,194`）。**待确认**是否上层兜底。
8. **非 mmsg 路径每次只收 1 包**（`udp.cpp:460-472`），吞吐低于 mmsg 路径。
9. **`~UdpSocket` 判断 `if (m_sock)`**（`udp.cpp:43`）而非 `isValid()`；`INVALID_SOCKET=-1` 为真，故析构时仍会 `Close(-1)`——无害但非预期。**待确认**。

---

## 8. 与 doc/ 的差异

doc/ 无 socket 层专门章节；相关零散提及与代码的差异：

1. **`doc/设计实现文档.md:407-408`** 把 `sendmmsg`/`recvmmsg`/GSO/GRO 描述为"待评估"的未来优化；但代码**已实现** `recvmmsg` 收 + `sendmmsg` 批量发（`USE_SENDMMSG` 编译期开关，`udp.cpp:346,558`）。GSO/GRO 确实尚未实现。**以代码为准：mmsg 已落地，GSO/GRO 未落地。**
2. **`doc/零拷贝与红黑树接收模型.md:27`** 称"`recvmmsg` 将数据从内核拷贝到 `MemoryManager` 预分配的 `PacketIn`"；但 socket 层 `recvmmsg` 实际收进 `UdpSocket` 内部自有缓冲 `m_mmsg`（`MultipleMsg` 的 `malloc` 区，`udp.cpp:346`、`mmsg.cpp:49`），`MsgMetaInfo.data` 指向该内部缓冲。socket 层与 `MemoryManager`/`PacketIn` **无直接耦合**；若存在到 `PacketIn` 的拷贝，发生在上层而非本模块。**以代码为准：本模块收进内部缓冲，非 `PacketIn`。**
3. 近期 commit 讨论"连接态不用 `SO_REUSEPORT"、"同 Context 用不同端口"等；但当前 `bind()` **无条件**对每个 socket 设 `SO_REUSEPORT`（`udp.cpp:225`），且一个 Context 只有一个 socket/端口（§1.1）。**以代码为准：SO_REUSEPORT 始终设置；一 Context 一 socket 一端口。** 若设计意图为"连接态禁用"，代码尚未体现。**待确认**设计与实现的收口。

---

## 9. 依赖

**被依赖（上游调用方）：**
- `cpp/src/context/context_impl.{h,cpp}`：值成员 `m_udpSocket`，负责 bind / 事件注册 / 收发驱动（`context_impl.h:226`、`context_impl.cpp:272,502,516,873,1466`）。
- `cpp/src/context/connection_impl.h`：持 `&m_udpSocket` 指针发包。

**依赖（本模块引用）：**
- `cpp/src/commom.h`：`socket_t` / `msghdr_t` / `INVALID_SOCKET` 平台类型（`commom.h:32-82`）。
- `cpp/src/mtu/mtu.h`：`UTP_ETHERNET_MTU` / `IPV4_HEADER_SIZE` / `UDP_HEADER_SIZE`。
- `include/utp/config.h`：`Config`（`recv_buf_size` / `send_buf_size`）。
- `include/utp/platform.h`：`OS_LINUX`/`OS_APPLE`/`OS_WINDOWS` 宏。
- `util/status.h`（`Status` 与错误码）、`util/error.h`（`GetSystemLastError`/`GetSystemErrnoMsg`）、`fmt`、`logger`。
- `utils/buffer.h`（`ByteBuffer`）、`utils/optional.hpp`、`utils/string8.h`（第三方 `libeular` 基础库）。
- 平台 socket API：`socket/bind/sendto/sendmsg/sendmmsg/recvmsg/recvmmsg/setsockopt/getsockname`；Windows `WSARecvMsg`/`WSASendTo`。
- 编译期开关：`USE_SENDMMSG`（启用 mmsg 路径）、`UTP_ENABLE_FAULT_INJECTION`（fiu 故障注入，`udp.cpp:165`、`mmsg.cpp:42`）。
