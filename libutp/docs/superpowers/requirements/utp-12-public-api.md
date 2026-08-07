# UTP-12 公共 API / 配置 / 错误码 / 回调 需求文档

> 本文档由 libutp 现有 C++ 实现反推。**代码为唯一 ground truth**，引用格式 `文件:行号`。
> 覆盖模块：`cpp/include/utp/` 全部对外头文件（context.h / connection.h / stream.h / config.h / errno.h / logger.h / platform.h / types.h / utp.h）及其在 `cpp/src/context/` 中的实现绑定。
> 默认值均取自代码；无法从代码确定的一律标注“待确认”。

---

## 1. 职责与边界

### 1.1 模块职责
- 提供 libutp 面向应用的 C++ 接口层，三层核心对象：
  - **Context**：库入口。管理底层 UDP 套接字、libevent 事件循环调度、所有连接生命周期、主动/被动建连、连接级回调注册与统计。`cpp/include/utp/context.h:33`
  - **Connection**：一条 uTP 连接，是多个 Stream 的容器；负责握手后数据传输、拥塞控制、MTU 探测、票据/恢复状态导出、连接级统计与回调。`cpp/include/utp/connection.h:27`
  - **Stream**：连接内的逻辑流，提供类 socket 的同步读写、零拷贝读写视图、优先级、状态机与流级回调。`cpp/include/utp/stream.h:25`
- 提供进程级/线程级辅助接口：版本号、错误码查询（线程本地）、日志回调与级别设置。

### 1.2 边界
- Context 采用 pImpl（`ContextImpl`，`context.h:267` / `context.cpp:52`），公开类只是薄转发层。
- Connection / Stream 是纯抽象基类（全 `virtual ... = 0`），实体由 `ConnectionImpl` / `StreamImpl` 实现（`connection.h:107`、`stream.h:71`）。应用不得自行构造，只能经由 Context/Connection 工厂方法获得。
- **对象拷贝/移动被禁用**：Context 显式 delete 拷贝与移动（`context.h:34-37`）；Connection 同样 delete（`connection.h:29-32`）。
- Context 不拥有 `event_base`，由调用方传入并负责生命周期（`context.h:168`）。
- Context 不拥有传入的 `Config *`（构造期读取，见 §5）。

---

## 2. 对外类型 / 结构

### 2.1 Context 内嵌枚举
| 枚举 | 取值 | 位置 |
|---|---|---|
| `EncryptionMode` | `kEncryptionNone=0` / `kEncryptionAesGcm128` / `kEncryptionAesGcm256` | `context.h:44-48` |
| `ConnectAttemptType` | `kConnectAttemptNormal=0` / `kConnectAttemptZeroRttToken` / `kConnectAttemptZeroRttState` / `kConnectAttemptPassive` | `context.h:54-59` |

### 2.2 Context 内嵌结构
- `Statistic`：上下文级统计，含 0-RTT（offered/accepted/rejected/replay_rejected/invalid_ticket_rejected）与路径验证（started/succeeded/failed）计数，全部默认 0。`context.h:65-74`
- `ConnectInfo`：普通建连参数（见 §5.2）。`context.h:80-86`
- `Connect0RttInfo`：票据式 0-RTT 参数，含 `session_ticket`、`early_data`、`early_fin`。`context.h:92-100`
- `Connect0RttWithStateInfo`：状态式 0-RTT 参数（加密恢复），含 `early_data`、`early_fin`。`context.h:106-113`
- `NewConnectionInfo`：服务端拦截回调入参（remote_ip/port、local_cid、peer_cid、encrypted）。`context.h:119-125`
- `ZeroRttDecisionInfo`：0-RTT 决策结果（accepted、reason）。`context.h:131-138`
- `ConnectAttemptInfo`：建连尝试上下文，透传给 `OnConnectError`。`context.h:144-155`

### 2.3 Connection 内嵌类型
| 类型 | 说明 | 位置 |
|---|---|---|
| `Ptr` | `std::shared_ptr<Connection>` | `connection.h:34` |
| `StreamType` | `kStreamTypeBidirectional=0` / `kStreamTypeUnidirectional=1` / `kStreamTypeAll=0xFF`（0xFF 仅用于统计计数） | `connection.h:40-44` |
| `ConnectionErrorInfo` | `error_code` + `error_reason` | `connection.h:50-53` |
| `ConnectionCloseInfo` | `error_code` + `error_reason` + `by_peer` | `connection.h:59-63` |
| `Description` | `scid`(本端CID) / `dcid`(对端CID) / `remoteHost` / `remotePort` | `connection.h:74-79` |
| `Statistic` | pmtu(bytes)、rtt/rttvar(us)、bw_estimate(bytes/s)、rx/tx/rtx_bytes，及 10 项流调度器指标 | `connection.h:85-105` |

### 2.4 Stream 内嵌类型
| 类型 | 说明 | 位置 |
|---|---|---|
| 优先级常量 | `kPriorityHighest=0` / `kPriorityLowest=7` / `kPriorityDefault=4` | `stream.h:31-35` |
| `ConstBufferView` | 只读零拷贝视图（data/len） | `stream.h:41-44` |
| `MutableBufferView` | 可写零拷贝视图（data/len） | `stream.h:50-53` |
| `State` | `kStateOpen=0` / `kStateHalfClosedLocal` / `kStateHalfClosedRemote` / `kStateClosed` | `stream.h:59-64` |

### 2.5 基础类型（types.h）
- `utp_time_t = uint64_t`、`utp_packno_t = uint64_t`、`ull`、`ll`、`struct Range{low,high}`。`types.h:18-36`
- `utp_error_t = uint32_t`。`errno.h:82`

---

## 3. API 时序 / 流程

### 3.1 客户端主动建连
1. `new Context(base, &config)` → 3. `bind(ip, port[, ifname])` → `connect(ConnectInfo)` / `connect0Rtt(...)` / `connect0RttWithState(...)`。
2. `bind` 注册读/写事件到 `event_base`，启动读事件。`context_impl.cpp:514-531`
3. `connect` 走 `connectInternal`：做同步前置校验（IP/端口非法→`UTP_ERR_INVALID_PARAM`；未 bind→`UTP_ERR_SOCKET_NOT_BOUND`；重复连接→`UTP_ERR_IN_PROGRESS`/`UTP_ERR_SOCKET_CONNECTED`），随后进入 pending 异步握手。`context_impl.cpp:533-581`
4. 握手结果异步收敛到 `handleConnectionState`（见 §6.2），成功触发 `OnConnected(Connection::Ptr)`，失败触发 `OnConnectError`。`context_impl.cpp:398-493`

### 3.2 服务端被动建连
1. `Context` + `bind` 后，收到 Initial → 触发 `OnNewConnection(NewConnectionInfo)`（返回 bool）。`context_impl.cpp:1748-1749`、`1976-1977`
2. 返回 `false` → 立即发 CONNECTION_CLOSE 拒绝；返回 `true` → 连接进入 pending（若回调内未立即 `accept()`）。`context.h:193-197`、`context_impl.cpp:1751-1758`
3. `accept()` 从 `m_pendingIncomingQueue` 取队首，发送握手；无 pending → 返回 `UTP_ERR_WOULD_BLOCK`。`context_impl.cpp:727-768`

### 3.3 流数据收发
- 主动建流：`Connection::createStream(type)` → 返回流 ID（>0）或 -1（错误，见 §6.1）。`connection_impl.cpp:3617-3626`
- 对端建流：触发 `Connection::OnIncomingStream(Stream*)`。`connection.h:65,114`
- 读写：同步拷贝 `write`/`read`，或零拷贝 `acquireWriteBuffer`+`commitWrite` / `acquireReadViews`+`commitReadViews`。`stream.h:87-128`
- 关闭：`Stream::close()` 发 FIN；`Stream::reset(errorCode)` 发 RESET_STREAM。`stream.h:151,158`

### 3.4 0-RTT / 会话恢复
- 导出：连接建立后 `Connection::exportSessionToken`（非加密）或 `exportSessionResumptionState`（加密），并可在 `OnSessionTokenReady` 后调用。`connection.h:117-120,166,174`
- 复用：下次 `connect0Rtt(Connect0RttInfo{session_ticket,...})`（仅非加密）或 `connect0RttWithState(info, state)`（支持加密）。`context.h:229,254,258`

---

## 4. 不变量与规则（MUST / MUST NOT）

- **MUST** 在调用 `connect*` 前先 `bind` 成功，否则返回 `UTP_ERR_SOCKET_NOT_BOUND`。`context_impl.cpp:557-559`
- **MUST** 由调用方保证 `event_base` 在 Context 生命周期内有效（Context 不拥有它）。`context.h:168`
- **MUST NOT** 拷贝或移动 Context / Connection（已 delete）。`context.h:34-37`、`connection.h:29-32`
- **MUST NOT** 自行 new/delete Connection、Stream 实体（抽象基类，工厂托管；Connection 经 `Connection::Ptr` 共享，Stream 由 Connection 拥有裸指针）。
- **MUST**（约定，代码强约束见 §6）在事件循环线程内调用 API 与访问回调入参对象——库内部无锁，回调均在 loop 线程同步触发。
- `connect0Rtt` payload（含 session_ticket、可选 early_data）**MUST** 使 `UTP_HEADER_SIZE + payload ≤ 1280`，否则 `UTP_ERR_OVERFLOW`。`context_impl.cpp:590-596`
- `connect0RttWithState` 的 state **MUST NOT** 为空且能解析；解析失败返回解析错误码。`context_impl.cpp:621-631`
- 流优先级取值 **MUST** 在 `[0,7]`；实现对超界值 `std::min` 收敛到 `kPriorityLowest=7`。`connection_impl.cpp:3634`
- `OnNewConnection` 回调返回 `false` **MUST** 导致连接被拒绝（发 CLOSE）。`context_impl.cpp:1751-1758`
- 建连失败与已连接后断开的回调语义**分离**：pending 阶段失败→`OnConnectError`；connected 后断开→`OnConnectionClosed`（不再走 error）。`context_impl.cpp:413-492`

---

## 5. 配置项与默认值（`config.h`，权威表）

> 全部字段为 `Config` 公有成员，默认值即成员初始化值。位置：`cpp/include/utp/config.h`。

### 5.1 Config 完整默认值表

**DPLPMTUD** (`config.h:58-67`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `enable_dplpmtud` | `true` | 是否开启 DPLPMTUD |
| `mtu_min` | `1280` | 下探最小 MTU（IPv6 最小要求） |
| `mtu_max` | `1500` | 上探最大 MTU |
| `mtu_base` | `1400` | 初始 MTU 估值 |
| `mtu_probe_interval` | `300` | 探测间隔（秒，注释称“默认 5 分钟”，实为 300 秒＝5 分钟；单位注释写“秒”） |
| `mtu_probe_step` | `16` | 探测步长 |
| `mtu_probe_timeout` | `2000` | 单次探测超时（ms） |
| `mtu_blackhole_loss_threshold` | `3` | 黑洞判定连续大包丢失阈值 |
| `mtu_blackhole_loss_window_ms` | `3000` | 黑洞判定时间窗口（ms） |
| `mtu_blackhole_cooldown_ms` | `5000` | 黑洞判定后冷却期（ms） |

**Keepalive** (`config.h:70-74`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `enable_keepalive` | `true` | 是否开启保活 |
| `keepalive_interval` | `0` | 保活间隔（ms），0＝由 `max_idle_timeout` 推算 |
| `keepalive_timeout` | `1500` | 保活包超时（ms） |
| `keepalive_probes` | `3` | 保活最大重试次数 |
| `max_idle_timeout` | `30000` | 最大空闲超时（ms） |

**Token / 0-RTT** (`config.h:77-78`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `zero_rtt_token_max_lifetime` | `600` | 0-RTT 票据最长时效（秒） |
| `zero_rtt_replay_window` | `10` | C++ 现状的抗重放窗口（秒）；C 版加密 0-RTT 不采用该窗口作为记录保留期，replay record 必须保留至 token 绝对过期时间，详见 utp-10 §10.4 |

**Path Migration** (`config.h:81`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `path_migration_mode` | `kPathMigrationConservative(0)` | 路径迁移策略（Conservative/Aggressive） |

**Socket** (`config.h:84-85`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `recv_buf_size` | `1024*1024` (1 MiB) | UDP 接收缓冲区 |
| `send_buf_size` | `1024*1024` (1 MiB) | UDP 发送缓冲区 |

**Congestion Control** (`config.h:88-104`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `cc_algorithm` | `0` | 算法：0=默认(BBR)/1=BBR/2=Cubic |
| `clock_granularity_us` | `1` | Pacer 时钟粒度（us） |
| `bbr_init_cwnd_mss` | `16` | BBR 初始 cwnd（MSS） |
| `bbr_min_cwnd_mss` | `4` | BBR 最小 cwnd（MSS） |
| `bbr_startup_high_gain` | `2.885f` | STARTUP 增益 |
| `bbr_cwnd_gain` | `2.0f` | PROBE_BW cwnd 增益 |
| `bbr_startup_growth_target` | `1.25f` | 带宽增长判定倍率 |
| `bbr_startup_full_bw_rounds` | `3` | STARTUP 退出判定轮数 |
| `bbr_probe_rtt_ms` | `200` | PROBE_RTT 时长（ms） |
| `bbr_min_rtt_expiry_ms` | `10000` | min_rtt 过期（ms） |
| `bbr_probe_rtt_multiplier` | `0.75f` | ProbeRTT 增益系数 |
| `bbr_similar_min_rtt_threshold` | `1.125f` | 相似 RTT 判定阈值 |
| `bbr_pacing_gains` | `{1.25,0.75,1,1,1,1,1,1}` | Pacing 周期增益 |
| `cubic_beta` | `0.7` | CUBIC 丢包回退系数（有效区间 (0,1)，超界回退默认，见 doc） |
| `cubic_c` | `0.4` | CUBIC 曲线常数（有效区间 (0,2]，超界回退默认，见 doc） |
| `cubic_init_cwnd_mss` | `32` | CUBIC 初始 cwnd（MSS） |
| `cubic_min_cwnd_mss` | `4` | CUBIC 最小 cwnd（MSS） |

**ACK 行为** (`config.h:107-110`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `ack_every_n_packets` | `4` | 每 N 包发一次 ACK |
| `max_ack_range_size` | `149` | ACK 帧最大 Range 数 |
| `ack_delay_exponent` | `3` | ACK 延迟指数 |
| `ack_delay` | `25` | 最大 ACK 延迟（ms） |

**Transport Parameters** (`config.h:113-124`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `handshake_timeout` | `800` | 握手首轮超时基准（ms） |
| `handshake_max_retries` | `2` | 握手最大重试次数 |
| `pending_incoming_limit` | `1024` | 全局未完成被动握手上限 |
| `pending_incoming_per_ip_limit` | `32` | 单 IP 未完成被动握手上限 |
| `pending_unaccepted_timeout_ms` | `3000` | 等待应用 accept 的最长时间 |
| `pending_pre_handshake_buffer_packets` | `16` | 握手前缓冲区包数上限 |
| `pending_pre_handshake_buffer_bytes` | `32*1024` (32 KiB) | 握手前缓冲区字节上限 |
| `init_max_streams_bidi` | `32` | 初始最大双向流数 |
| `init_max_streams_uni` | `16` | 初始最大单向流数 |
| `initial_max_data` | `8 MiB` | 初始连接级流控窗口 |
| `initial_max_stream_data_bidi_local` | `256*1024` (256 KiB) | 对端可向本端双向流发送的初始窗口 |
| `initial_max_stream_data_bidi_remote` | `256*1024` (256 KiB) | 本端可向对端双向流发送的初始窗口 |

**Stream Scheduler** (`config.h:127-137`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `stream_default_priority` | `4` | 默认流优先级（0-7） |
| `stream_scheduler_mode` | `kStreamSchedulerStrict(1)` | 流调度模式（Disabled/Strict/DRR） |
| `stream_aging_threshold` | `8` | 优先级老化阈值 |
| `stream_aging_step` | `1` | 优先级老化步进 |
| `stream_drr_quantum` | `1200` | DRR 基准量子 |
| `stream_drr_deficit_cap` | `128*1024` (128 KiB) | DRR 赤字上限 |
| `stream_send_buffer_limit` | `256*1024` (256 KiB) | 单流写缓冲区上限 |
| `stream_enable_coalescing` | `true` | 小包聚合发送开关 |
| `stream_min_payload_before_immediate_send` | `1200` | 聚合触发阈值 |
| `stream_coalesce_delay_us` | `1000` | 聚合等待时延（us） |
| `stream_unacked_data_limit` | `256*1024` (256 KiB) | 在途未确认数据上限（bytes） |

**Receive Reassembly Limits** (`config.h:140-144`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `recv_reassembly_memory_limit` | `16 MiB` | 单连接乱序重组内存上限 |
| `recv_reassembly_fragment_limit` | `4096` | 单连接乱序片段上限 |
| `recv_stream_reassembly_memory_limit` | `4 MiB` | 单流乱序重组内存上限 |
| `recv_stream_reassembly_fragment_limit` | `1024` | 单流乱序片段上限 |
| `recv_stream_max_gap` | `2 MiB` | 单帧相对当前读取点最大前向间隙 |

**Connection Scheduler (WDRR)** (`config.h:147-149`)
| 字段 | 默认值 | 含义 |
|---|---|---|
| `connection_scheduler_mode` | `kConnectionSchedulerWdrr(2)` | 连接级调度模式（Disabled/Strict/WDRR） |
| `connection_wdrr_quantum` | `1200` | 连接级 WDRR 量子 |
| `connection_wdrr_deficit_cap` | `512*1024` (512 KiB) | 连接级 WDRR 赤字上限 |

### 5.2 建连参数默认值（内嵌结构）
| 结构.字段 | 默认值 | 位置 |
|---|---|---|
| `ConnectInfo.timeout` | `3000` (ms) | `context.h:83` |
| `ConnectInfo.retries` | `0` | `context.h:84` |
| `ConnectInfo.encrypted` | `kEncryptionNone` | `context.h:85` |
| `Connect0RttInfo.early_fin` | `false` | `context.h:99` |
| `Connect0RttWithStateInfo.early_fin` | `false` | `context.h:112` |
| （0-RTT 票据时效） | 取 `zero_rtt_token_max_lifetime`，最小 1 秒 | `context_impl.cpp:611` |

### 5.3 日志默认级别
- `utp_set_log_level` 默认级别为 `UTP_LOG_SILENT`（不输出）。`logger.h:50`

### 5.4 枚举默认（config.h 独立枚举）
- `PathMigrationMode`：`kPathMigrationConservative=0` / `kPathMigrationAggressive=1`。`config.h:25-28`
- `StreamSchedulerMode`：`kStreamSchedulerDisabled=0` / `Strict=1` / `Drr=2`。`config.h:34-38`
- `ConnectionSchedulerMode`：`kConnectionSchedulerDisabled=0` / `Strict=1` / `Wdrr=2`。`config.h:44-48`

---

## 6. 错误码（errno.h）与返回值语义

### 6.1 公共 API 返回值约定（重要，与头注释不一致）
- Context 的 `bind/connect/connect0Rtt/connect0RttWithState/accept` 头注释写“返回错误码，0 表示成功”，**但实现经 `NormalizePublicStatus` 归一化为：成功返回 `0`，失败返回 `-1`**，真实错误码写入线程本地 last error。`context.cpp:17-45,109-127`
  - 因此应用要拿具体错误码 **MUST** 调 `utp_get_last_error()` / `utp_get_error_string()`。`errno.h:88-94`
- `Connection::createStream` 返回**流 ID（>0）**或 `-1`（失败，last error 已设置）。`connection_impl.cpp:3617-3626`
- `Stream::write` 返回实际写入长度或负数错误；`read` 同理（`stream.h:87,95`，具体见 Stream 模块文档）。
- `getStream` 未找到返回 `nullptr`。`connection.h:188`

### 6.2 错误码取值全表（`errno.h:19-76`，`utp_error_t=uint32_t`）
**通用（0x0000 起，连续）**
`UTP_ERR_OK=0x0000`、`INVALID_PARAM`、`INTERNAL_ERROR`、`CANCELLED`、`TIMEOUT`、`VERSION_MISMATCH`、`NOT_IMPLEMENTED`、`BUSY`、`NO_MEMORY`、`IN_PROGRESS`、`WOULD_BLOCK`、`INVALID_STATE`、`CONNECTION_CLOSING`、`CID_CONFLICT`、`OVERFLOW`、`STREAM_LIMITED`（0x0001-0x000F 连续）；
`PATH_VALIDATION_BLOCKED=0x0010`、`SESSION_TOKEN_UNAVAILABLE`、`RESUMPTION_STATE_UNAVAILABLE`、`CONTEXT_UNAVAILABLE`。

**Socket（0x0020 起）**：`SOCKET_CREATE=0x0020`、`SOCKET_OPTION`、`SOCKET_NOT_BOUND`、`SOCKET_BIND`、`SOCKET_IOCTL`、`SOCKET_READ`、`SOCKET_WRITE`、`SOCKET_CONNECTED`、`SOCKET_EVENT`。

**Stream（0x0040 起）**：`STREAM_CLOSED=0x0040`、`STREAM_NOT_FOUND`、`STREAM_STATE_ERROR`、`STREAM_LIMIT_ERROR`、`STREAM_FLOW_CONTROL`、`STREAM_DATA_BLOCKED`、`STREAM_DATA_LIMITED`、`STREAM_ID_EXHAUSTED`。

**帧（0x0060 起）**：`FRAME_FORMAT_ERROR=0x0060`、`FRAME_UNEXPECTED`。

**加密（0x0080 起）**：`CRYPTO_UNINITIALIZED=0x0080`、`CRYPTO_INIT_FAILED`、`CRYPTO_ENCRYPTION`、`CRYPTO_DECRYPTION`、`RANDOM_GENERATION_FAILED`。

**应用层**：`UTP_ERR_APP_ERROR_BASE=0x0100`（0x0100-0xFFFF 保留给应用）。

---

## 7. 回调语义（事件循环亲和性 / 同步异步）

### 7.1 通用亲和性
- 全部回调均在传入 Context 的 `event_base` 事件循环线程内**同步**触发（由 `onReadEvent` / 定时器处理链调用），库内部无加锁；应用回调内的重入调用（如 `accept()`）在同一线程内是安全的。回调触发点见 `context_impl.cpp:416,442,484,1095-1103,1634,1749,1799,1977`。

### 7.2 Context 级回调
| 回调 | 签名 | 触发时机 | 位置 |
|---|---|---|---|
| `OnConnected` | `void(Connection::Ptr)` | 连接从 pending 收敛到 connected（主动/被动均触发） | `context.h:157`；触发 `context_impl.cpp:416-417,1634,1799` |
| `OnConnectError` | `void(int32_t, const std::string&, ConnectAttemptInfo)` | 建连阶段失败：同步前置失败、握手超时、握手期被关闭、CID 冲突、被动拒绝 | `context.h:158`；触发 `context_impl.cpp:302-309,442,481,754,1057,1391` |
| `OnConnectionClosed` | `void(Connection::Ptr)` | **已连接**后（非 pending）连接断开 | `context.h:159`；触发 `context_impl.cpp:483-484` |
| `OnNewConnection` | `bool(const NewConnectionInfo&)` | 收到新的被动建连请求；返回 `true` 允许（可在回调内直接 `accept()`），`false` 拒绝（发 CLOSE） | `context.h:160`；`context_impl.cpp:1748,1976` |
| `OnZeroRttDecision` | `void(const ZeroRttDecisionInfo&)` | 0-RTT 接受/拒绝决策（含无效票据、重放拒绝等），`accepted` + `reason` | `context.h:161`；`context_impl.cpp:1095-1103` |

- **收敛出口统一**：`handleConnectionState` 是所有连接状态回调的单一出口（`context_impl.cpp:398`）。connected→只走 `OnConnected`；handshake 期 close/timedwait 且无重试剩余→`OnConnectError`；disconnected 且非 pending→`OnConnectionClosed`；disconnected 且仍 pending 且有重试→重发不回调。`context_impl.cpp:413-492`

> C 版目标语义：普通连接和 0-RTT 的 `utp_on_new_connection_fn` 都必须在回调内部调用 `utp_context_accept()`；调用成功后返回 `true`，拒绝或调用失败返回 `false`。0-RTT 的两消息响应只由成功的 `utp_context_accept()` 触发。

### 7.3 Connection 级回调
| 回调 | 签名 | 触发时机 | 位置 |
|---|---|---|---|
| `OnIncomingStream` | `void(Stream*)` | 对端发起新流 | `connection.h:65,114` |
| `OnSessionTokenReady` | `void()` | 握手完成且服务器下发了票据 | `connection.h:66,120` |
| `OnError` | `void(const ConnectionErrorInfo&)` | 连接级错误 | `connection.h:67,126` |
| `OnClosed` | `void(const ConnectionCloseInfo&)` | 连接关闭（`by_peer` 标识是否对端发起） | `connection.h:68,132` |

- 注意：Context 级 `OnConnectionClosed(Connection::Ptr)` 与 Connection 级 `OnClosed(ConnectionCloseInfo)` 是**两个不同回调**，前者用于 Context 侧生命周期回收，后者带关闭原因/发起方，供应用观测。

### 7.4 Stream 级回调
| 回调 | 签名 | 语义 | 位置 |
|---|---|---|---|
| `OnReadable` | `void()` | 接收缓冲区可读 | `stream.h:66,183` |
| `OnWritable` | `void()` | 发送缓冲区可写 | `stream.h:67,189` |
| `OnClosed` | `void()` | 流双向完全关闭 | `stream.h:68,195` |
| `OnReset` | `void(uint16_t)` | 收到对端 RESET_STREAM（携带错误码） | `stream.h:69,201` |

### 7.5 全局日志回调
- `utp_log_callback_t = void(*)(int32_t level, const char* msg, int32_t size)`，C 风格函数指针。`logger.h:35`
- `utp_set_log_cb` / `utp_set_log_level` **非线程安全**，MUST 在 Context 创建前设置。`logger.h:43-51`

---

## 8. 当前实现边界

- 公共 API 建连/绑定类接口**不返回具体错误码**，仅 0/-1，具体码经线程本地 last error 获取（§6.1）。这是与头注释语义的实现偏差。
- `Config *` 在 Context 构造时读取（`context.cpp:52`）；运行期修改 Config 对象是否生效**待确认**（未在本次审阅的公共头/`context.cpp` 中体现热更新接口）。
- Context 未提供显式 `unbind` / 重绑定 / 关闭套接字的公共接口；生命周期随 Context 析构（`context.h:169`）。
- `connect0Rtt`（票据式）仅支持非加密场景（`context.h:254`）；加密恢复须走 `connect0RttWithState`（`context.h:224,229`）。
- `connect0Rtt` 单包 payload 上限硬编码 `1280`（`context_impl.cpp:594`），非取自 `mtu_*` 配置。
- `exportSessionToken` / `exportSessionResumptionState` 无票据/状态时分别返回 `UTP_ERR_SESSION_TOKEN_UNAVAILABLE` / `UTP_ERR_RESUMPTION_STATE_UNAVAILABLE`（错误码已定义，`errno.h:38-39`；确切触发路径在 Connection 模块，本文不展开）。
- 平台：`platform.h` 声明支持 Windows / Linux / macOS / iOS / *BSD / Solaris，未识别平台编译期 `#error`（`platform.h:11-45`）。

---

## 9. 与 doc/ 的差异

参考 `doc/连接与异常处理流程.md`、`doc/默认值与调参指南.md`、`cpp/README.md`。

1. **返回值语义**：doc（及头注释）表述“返回错误码 / 0 成功”，与实现的“0/-1 + last error”不一致（§6.1）。以代码为准：`bind/connect/accept/connect0Rtt/connect0RttWithState` 实际返回 0 或 -1。
2. **回调收敛规则一致**：`doc/连接与异常处理流程.md:52-67,170-184` 描述的 `handleConnectionState` 三分支（`OnConnected` / `OnConnectError` / `OnConnectionClosed`）与实现 `context_impl.cpp:413-492` 完全吻合，且明确“建连失败”与“连接后断开”回调分离。
3. **cubic 参数区间**：`doc/默认值与调参指南.md:137-138` 称 `cubic_beta ∈ (0,1)`、`cubic_c ∈ (0,2]` 超界回退默认；config.h 仅给默认值 0.7 / 0.4，未在头文件表达区间约束（约束在拥塞模块实现，属其他模块）。
4. **`mtu_probe_interval` 单位注释歧义**：`config.h:62` 同时写“探测间隔时间 (秒)”与“默认 5 分钟”，值 300 秒＝5 分钟自洽，但“单位=秒”与其他 `*_ms` 字段风格不同，需留意。
5. doc 中出现的“流调度器/连接调度器”指标与配置项，与 `connection.h:94-104`、`config.h:126-149` 的字段一一对应，无冲突。

---

## 10. 依赖

- **对外头依赖**：所有对外头 `#include <utp/platform.h>`（导出宏 `UTP_API`、`EXTERN_C_*`、`UTP_THREAD_LOCAL`、平台检测）。`utp.h` 仅是 `context.h` 的兼容包装（`utp.h:10`）。
- **第三方 / 系统**：Context 依赖 **libevent**（`struct event` / `struct event_base` 前置声明，`context.h:21-22`）。
- **标准库**：`std::string` / `std::vector` / `std::array` / `std::function` / `std::shared_ptr`（context.h/connection.h/stream.h）。
- **实现层依赖**：`Context` → `ContextImpl`（`context/context_impl.h`）→ `ConnectionImpl` / `StreamImpl`；错误归一化依赖 `util/error.h`（`SetLastErrorV`，`context.cpp:12,23`）；版本号来自 `src/version.h`（`context.cpp:10,61`）。
- 加密（AES-GCM / x25519 / token / resumption codec）位于 `cpp/src/crypto/`，被 0-RTT / 会话恢复路径使用（本文只涉及其对外表现，不展开实现）。

---

_引用文件清单：`cpp/include/utp/{context,connection,stream,config,errno,logger,platform,types,utp}.h`、`cpp/src/context.cpp`、`cpp/src/context/context_impl.cpp`、`cpp/src/context/connection_impl.cpp`、`cpp/README.md`、`doc/连接与异常处理流程.md`、`doc/默认值与调参指南.md`。_
