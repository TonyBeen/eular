#ifndef NTRS_NTRS_H
#define NTRS_NTRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#    if defined(NTRS_BUILD_SHARED)
#        if defined(NTRS_EXPORTS)
#            define NTRS_API __declspec(dllexport)
#        else
#            define NTRS_API __declspec(dllimport)
#        endif
#    else
#        define NTRS_API
#    endif
#elif defined(NTRS_BUILD_SHARED)
#    define NTRS_API __attribute__((visibility("default")))
#else
#    define NTRS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif


#define NTRS_MAX_IP_LEN     64      // IP 地址文本缓冲区长度
#define NTRS_MAX_TEXT_LEN   128     // 通用短文本字段缓冲区长度
#define NTRS_MAX_CANDIDATES 8       // 单次会话信令最多返回的候选地址数量

typedef uint16_t ntrs_nat_class_t;
typedef uint8_t  ntrs_punch_order_t;
typedef uint8_t  ntrs_connect_role_t;

/**
 * @brief NAT 探测分类结果。
 */
enum {
    // 无法判断 NAT 类型
    NTRS_NAT_CLASS_UNKNOWN = 0,
    // 本机地址和公网映射一致, 通常可被公网直接访问
    NTRS_NAT_CLASS_OPEN_PUBLIC = 1,
    // 本机公网可见, 但变源 UDP 回复被防火墙过滤
    NTRS_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL = 2,
    // Full cone NAT, 映射稳定且过滤较宽松
    NTRS_NAT_CLASS_FULL_CONE = 3,
    // IP restricted NAT, 回包通常要求本机先向对端 IP 发包
    NTRS_NAT_CLASS_IP_RESTRICTED = 4,
    // Port restricted NAT, 回包通常要求本机先向对端 IP:port 发包
    NTRS_NAT_CLASS_PORT_RESTRICTED = 5,
    // Symmetric NAT, 不同目标可能产生不同公网映射
    NTRS_NAT_CLASS_SYMMETRIC = 6,
    // 多公网 IP 或多线路映射, 打洞难度高
    NTRS_NAT_CLASS_SYMMETRIC_MULTI_LINE = 7,
    // UDP 探测不可达, 可能被本机/网络或运营商阻断
    NTRS_NAT_CLASS_UDP_BLOCKED = 8,
};

/**
 * @brief 打洞启动顺序。
 */
enum {
    /** 未指定打洞顺序。 */
    NTRS_PUNCH_ORDER_UNKNOWN = 0,
    /** 本端先发送打洞包。 */
    NTRS_PUNCH_ORDER_SEND_FIRST = 1,
    /** 本端先等待对端打洞包。 */
    NTRS_PUNCH_ORDER_WAIT_FIRST = 2,
    /** 双方同时发送打洞包。 */
    NTRS_PUNCH_ORDER_SIMULTANEOUS = 3,
};

/**
 * @brief 连接建立角色。
 */
enum {
    /** 未指定角色。 */
    NTRS_CONNECT_ROLE_UNKNOWN = 0,
    /** 本端主动发起连接确认。 */
    NTRS_CONNECT_ROLE_INITIATOR = 1,
    /** 本端等待对端连接确认。 */
    NTRS_CONNECT_ROLE_LISTENER = 2,
};

/**
 * @brief NAT 探测结果。
 *
 * 该结构由 `ntrs_detect_nat_options_init()` 初始化探测选项后，通过
 * `ntrs_async_detect_nat()` 的回调结果返回。字段只表达对外稳定 API:
 * endpoint、探测质量统计和最终 NAT 分类。
 */
typedef struct ntrs_nat_info {
    /** 本地 UDP socket 地址。 */
    char             local_ip[NTRS_MAX_IP_LEN];
    /** 本地 UDP socket 端口。 */
    uint16_t         local_port;
    /** 主探测端点观察到的公网映射地址。 */
    char             srflx_ip[NTRS_MAX_IP_LEN];
    /** 主探测端点观察到的公网映射端口。 */
    uint16_t         srflx_port;
    /** 辅助探测端点观察到的公网映射地址。 */
    char             srflx_ip_2[NTRS_MAX_IP_LEN];
    /** 辅助探测端点观察到的公网映射端口。 */
    uint16_t         srflx_port_2;
    /** 主探测端点是否至少收到一次有效响应。 */
    bool             probe1_ok;
    /** 辅助探测端点是否至少收到一次有效响应。 */
    bool             probe2_ok;
    /** 主探测端点平均 RTT, 单位毫秒; 未成功时为 -1。 */
    int32_t          probe1_rtt_ms;
    /** 辅助探测端点平均 RTT, 单位毫秒; 未成功时为 -1。 */
    int32_t          probe2_rtt_ms;
    /** 每个探测端点的目标成功轮数。 */
    int32_t          probe_rounds;
    /** 主探测端点成功响应次数。 */
    int32_t          probe1_success_count;
    /** 辅助探测端点成功响应次数。 */
    int32_t          probe2_success_count;
    /** 主探测端点观察到的不同公网映射数量。 */
    int32_t          probe1_distinct_mappings;
    /** 辅助探测端点观察到的不同公网映射数量。 */
    int32_t          probe2_distinct_mappings;
    /** 最终 NAT 分类, 取值见 `NTRS_NAT_CLASS_*`。 */
    ntrs_nat_class_t nat_class;
} ntrs_nat_info_t;

/**
 * @brief 对端候选地址。
 *
 * 候选地址来自控制面信令，可传给 `ntrs_try_udp_hole_punch()` 或
 * `ntrs_async_try_udp_hole_punch()` 进行 UDP 打洞。
 */
typedef struct ntrs_peer_candidate {
    /** 候选 IP 地址。 */
    char     ip[NTRS_MAX_IP_LEN];
    /** 候选 UDP 端口。 */
    uint16_t port;
    /** 候选类型文本, 例如 local、srflx。 */
    char     type[NTRS_MAX_TEXT_LEN];
} ntrs_peer_candidate_t;

/**
 * @brief 会话信令结果。
 *
 * 由 `ntrs_create_session()` 或 `ntrs_wait_for_signal()` 返回，描述对端身份、
 * 打洞策略、会话 token 和候选地址列表。
 */
typedef struct ntrs_session_signal {
    /** 对端 peer id。 */
    char             peer_id[NTRS_MAX_TEXT_LEN];
    /** 对端 device id。 */
    char             peer_device_id[NTRS_MAX_TEXT_LEN];
    /** 对端 NAT 分类。 */
    ntrs_nat_class_t peer_nat_class;
    /** 本次 P2P 会话 id。 */
    char             session_id[NTRS_MAX_TEXT_LEN];
    /** 对端会话 token, 用于后续打洞/确认报文。 */
    char             peer_session_token[NTRS_MAX_TEXT_LEN];
    /** 打洞启动顺序。 */
    ntrs_punch_order_t  punch_order;
    /** 连接确认角色。 */
    ntrs_connect_role_t connect_role;
    /** 预热打洞轮数。 */
    uint32_t         warmup_rounds;
    /** 预热打洞间隔, 单位毫秒。 */
    uint32_t         warmup_interval_ms;
    /** 会话过期时间戳, 单位秒。 */
    uint32_t         expire_at;
    /** `candidates` 中有效候选数量。 */
    uint32_t         candidate_count;
    /** 对端候选地址数组。 */
    ntrs_peer_candidate_t candidates[NTRS_MAX_CANDIDATES];
} ntrs_session_signal_t;

/**
 * @brief NAT 探测选项。
 */
typedef struct ntrs_detect_nat_options {
    /** 每个探测端点需要完成的成功轮数。 */
    int32_t probe_rounds;
    /** 每轮探测的最大重试次数。 */
    int32_t retries_per_round;
    /** 是否启用过滤行为辅助探测。 */
    bool    enable_filter_probe;
    /** 是否输出详细探测日志。 */
    bool    verbose;
} ntrs_detect_nat_options_t;

/**
 * @brief 异步客户端句柄。
 *
 * 由 `ntrs_async_client_create()` 创建，由 `ntrs_async_client_destroy()` 释放。
 */
typedef struct ntrs_async_client ntrs_async_client_t;

/**
 * @brief libevent event_base 前置声明。
 */
struct event_base;

/**
 * @brief 异步请求类型。
 *
 * `ntrs_async_result_t::type` 使用该枚举标识回调对应的请求类型。
 */
typedef enum ntrs_async_request_type {
    /** 异步认证请求。 */
    NTRS_ASYNC_AUTH = 1,
    /** 异步请求 NAT 探测端点。 */
    NTRS_ASYNC_REQUEST_PROBE_ENDPOINTS = 2,
    /** 异步 NAT 探测。 */
    NTRS_ASYNC_DETECT_NAT = 3,
    /** 异步注册 peer。 */
    NTRS_ASYNC_REGISTER_PEER = 4,
    /** 异步注销 peer。 */
    NTRS_ASYNC_UNREGISTER_PEER = 5,
    /** 异步创建 P2P 会话。 */
    NTRS_ASYNC_CREATE_SESSION = 6,
    /** 异步等待对端会话信令。 */
    NTRS_ASYNC_WAIT_FOR_SIGNAL = 7,
    /** 异步 UDP 打洞。 */
    NTRS_ASYNC_UDP_HOLE_PUNCH = 8,
    /** 异步连接控制面。 */
    NTRS_ASYNC_CONNECT_CONTROL = 9,
    /** 异步心跳。 */
    NTRS_ASYNC_HEARTBEAT = 10,
} ntrs_async_request_type_t;

/**
 * @brief 异步请求完成结果。
 *
 * 回调中根据 `type` 读取对应字段。失败时 `success` 为 false,
 * `error_message` 给出简要原因。
 */
typedef struct ntrs_async_result {
    /** 请求 id。 */
    uint64_t                  request_id;
    /** 请求类型。 */
    ntrs_async_request_type_t type;
    /** 请求是否成功。 */
    bool                      success;
    /** 请求是否被取消。 */
    bool                      cancelled;
    /** 失败原因文本。 */
    char                      error_message[NTRS_MAX_TEXT_LEN];
    /** 认证成功后返回的控制面 session token。 */
    char                      session_token[NTRS_MAX_TEXT_LEN];
    /** 控制面建议的默认租约秒数。 */
    uint32_t                  lease_default_sec;
    /** 主 NAT 探测端点文本, 格式为 host:port 或 [ipv6]:port。 */
    char                      probe1[NTRS_MAX_TEXT_LEN];
    /** 辅助 NAT 探测端点文本, 格式为 host:port 或 [ipv6]:port。 */
    char                      probe2[NTRS_MAX_TEXT_LEN];
    /** NAT 探测完成结果。 */
    ntrs_nat_info_t           nat_info;
    /** 会话创建或等待信令完成结果。 */
    ntrs_session_signal_t     session_signal;
    /** UDP 打洞选中的候选地址。 */
    ntrs_peer_candidate_t     selected_candidate;
    /** 异步连接控制面成功后返回的 socket fd。 */
    int32_t                   control_fd;
} ntrs_async_result_t;

/**
 * @brief 异步请求完成回调。
 *
 * @param result 请求完成结果。指针只在回调期间有效。
 * @param user_data 调用异步 API 时传入的用户上下文。
 */
typedef void (*ntrs_async_callback_t)(const ntrs_async_result_t* result, void* user_data);

/**
 * @brief 初始化 NAT 探测结果结构。
 *
 * @param info 待初始化的 NAT 探测结果。允许为 NULL, 为 NULL 时无操作。
 */
NTRS_API void ntrs_nat_info_init(ntrs_nat_info_t* info);

/**
 * @brief 初始化 NAT 探测选项。
 *
 * 默认值适合常规探测: 多轮探测、启用过滤辅助探测、关闭详细日志。
 *
 * @param options 待初始化的探测选项。允许为 NULL, 为 NULL 时无操作。
 */
NTRS_API void ntrs_detect_nat_options_init(ntrs_detect_nat_options_t* options);

/**
 * @brief 建立到 NTRS 控制面的同步 TCP 连接。
 *
 * @param ntrs_ip 控制面主机名或 IP 地址。
 * @param ntrs_port 控制面 TCP 端口。
 * @return 成功返回 socket fd, 失败返回 -1。
 */
NTRS_API int32_t ntrs_connect_control(const char* ntrs_ip, uint16_t ntrs_port);

/**
 * @brief 同步请求 NAT 探测端点。
 *
 * @param control_fd 已连接控制面的 socket fd。
 * @param session_token 认证后获得的 session token, 可为 NULL。
 * @param probe1 输出主探测端点文本缓冲区。
 * @param probe1_len `probe1` 缓冲区长度。
 * @param probe2 输出辅助探测端点文本缓冲区。
 * @param probe2_len `probe2` 缓冲区长度。
 * @return 成功返回 true, 失败返回 false。
 */
NTRS_API bool    ntrs_request_probe_endpoints(int32_t control_fd, const char* session_token, char* probe1,
                                              size_t probe1_len, char* probe2, size_t probe2_len);

/**
 * @brief 同步认证 peer 并获取控制面 session token。
 *
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param bootstrap_token 启动认证 token。
 * @param session_token 输出 session token 缓冲区。
 * @param session_token_len `session_token` 缓冲区长度。
 * @param lease_default_sec 输出默认租约秒数, 可为 NULL。
 * @return 成功返回 true, 失败返回 false。
 */
NTRS_API bool    ntrs_auth(int32_t control_fd, const char* peer_id, const char* bootstrap_token, char* session_token,
                           size_t session_token_len, uint32_t* lease_default_sec);

/**
 * @brief 同步注册本端 peer 在线状态。
 *
 * 注册信息包含本端身份、设备 id、NAT 分类和候选 endpoint。注册后其他 peer
 * 才能通过控制面创建到本端的会话。
 *
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param device_id 本端 device id。
 * @param session_token 认证后获得的 session token。
 * @param nat 本端 NAT 探测结果。
 * @return 成功返回 true, 失败返回 false。
 */
NTRS_API bool    ntrs_register_peer(int32_t control_fd, const char* peer_id, const char* device_id,
                                    const char* session_token, const ntrs_nat_info_t* nat);

/**
 * @brief 同步注销本端 peer 在线状态。
 *
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param session_token 认证后获得的 session token。
 * @param reason 注销原因文本, 可为 NULL。
 * @return 成功返回 true, 失败返回 false。
 */
NTRS_API bool    ntrs_unregister_peer(int32_t control_fd, const char* peer_id, const char* session_token,
                                      const char* reason);

/**
 * @brief 同步创建到目标 peer 的 P2P 会话。
 *
 * @param control_fd 已连接控制面的 socket fd。
 * @param src_peer_id 本端 peer id。
 * @param src_device_id 本端 device id。
 * @param dst_peer_id 目标 peer id。
 * @param dst_device_id 目标 device id, 可为空字符串表示自动选择。
 * @param session_token 认证后获得的 session token。
 * @param out 输出会话信令。
 * @return 成功返回 true, 失败返回 false。
 */
NTRS_API bool    ntrs_create_session(int32_t control_fd, const char* src_peer_id, const char* src_device_id,
                                     const char* dst_peer_id, const char* dst_device_id,
                                     const char* session_token, ntrs_session_signal_t* out);

/**
 * @brief 同步等待控制面推送的会话信令。
 *
 * @param control_fd 已连接控制面的 socket fd。
 * @param timeout_ms 等待超时, 单位毫秒。
 * @param out 输出会话信令。
 * @return 收到有效信令返回 true, 超时或失败返回 false。
 */
NTRS_API bool    ntrs_wait_for_signal(int32_t control_fd, int32_t timeout_ms, ntrs_session_signal_t* out);

/**
 * @brief 同步尝试 UDP 打洞并选择可用候选地址。
 *
 * @param udp_sock 已绑定的 UDP socket fd。
 * @param candidates 候选地址数组。
 * @param candidate_count 候选地址数量。
 * @param send_rounds 打洞发送轮数。
 * @param select_wait_ms 选择候选前等待响应的时间, 单位毫秒。
 * @param selected 输出被选中的候选地址, 可为 NULL。
 * @return 打洞成功并选中候选返回 true, 否则返回 false。
 */
NTRS_API bool    ntrs_try_udp_hole_punch(int32_t udp_sock, const ntrs_peer_candidate_t* candidates,
                                         uint32_t candidate_count, int32_t send_rounds, int32_t select_wait_ms,
                                         ntrs_peer_candidate_t* selected);

/**
 * @brief 创建异步客户端。
 *
 * 异步客户端复用调用方提供的 libevent `event_base`。
 *
 * @param base 调用方管理的 event_base, 不可为 NULL。
 * @return 成功返回异步客户端句柄, 失败返回 NULL。
 */
NTRS_API ntrs_async_client_t* ntrs_async_client_create(struct event_base* base);

/**
 * @brief 销毁异步客户端并释放其内部资源。
 *
 * 未完成请求会被取消。传入 NULL 时无操作。
 *
 * @param client 异步客户端句柄。
 */
NTRS_API void                 ntrs_async_client_destroy(ntrs_async_client_t* client);

/**
 * @brief 取消指定异步请求。
 *
 * @param client 异步客户端句柄。
 * @param request_id 需要取消的请求 id。
 * @return 成功提交取消返回 true, 未找到或参数非法返回 false。
 */
NTRS_API bool                 ntrs_async_client_cancel(ntrs_async_client_t* client, uint64_t request_id);

/**
 * @brief 异步认证 peer。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param bootstrap_token 启动认证 token。
 * @param callback 完成回调。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_auth(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                              const char* peer_id, const char* bootstrap_token, ntrs_async_callback_t callback,
                              void* user_data);

/**
 * @brief 异步建立到控制面的 TCP 连接。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param host 控制面主机名或 IP 地址。
 * @param port 控制面 TCP 端口。
 * @param timeout_ms 连接超时, 单位毫秒。
 * @param callback 完成回调。成功时 `result->control_fd` 为连接 fd。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_connect_control(ntrs_async_client_t* client, uint64_t* request_id, const char* host,
                                         uint16_t port, int32_t timeout_ms, ntrs_async_callback_t callback,
                                         void* user_data);

/**
 * @brief 异步请求 NAT 探测端点。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param session_token 认证后获得的 session token。
 * @param callback 完成回调。成功时读取 `result->probe1` 和 `result->probe2`。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_request_probe_endpoints(ntrs_async_client_t* client, uint64_t* request_id,
                                                 int32_t control_fd, const char* session_token,
                                                 ntrs_async_callback_t callback, void* user_data);

/**
 * @brief 异步执行 NAT 探测。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param udp_sock 已绑定的 UDP socket fd。
 * @param probe1_host 主探测端点主机名或 IP 地址。
 * @param probe1_port 主探测端点 UDP 端口。
 * @param probe2_host 辅助探测端点主机名或 IP 地址, 可为空。
 * @param probe2_port 辅助探测端点 UDP 端口。
 * @param control_fd 已连接控制面的 socket fd。
 * @param session_token 认证后获得的 session token。
 * @param options 探测选项, 可为 NULL 表示使用默认值。
 * @param callback 完成回调。成功时读取 `result->nat_info`。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_detect_nat(ntrs_async_client_t* client, uint64_t* request_id, int32_t udp_sock,
                                    const char* probe1_host, uint16_t probe1_port, const char* probe2_host,
                                    uint16_t probe2_port, int32_t control_fd, const char* session_token,
                                    const ntrs_detect_nat_options_t* options, ntrs_async_callback_t callback,
                                    void* user_data);

/**
 * @brief 异步注册本端 peer 在线状态。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param device_id 本端 device id。
 * @param session_token 认证后获得的 session token。
 * @param nat 本端 NAT 探测结果, 可为 NULL 表示未知 NAT。
 * @param callback 完成回调。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_register_peer(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                       const char* peer_id, const char* device_id, const char* session_token,
                                       const ntrs_nat_info_t* nat, ntrs_async_callback_t callback, void* user_data);

/**
 * @brief 异步注销本端 peer 在线状态。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param session_token 认证后获得的 session token。
 * @param reason 注销原因文本, 可为 NULL。
 * @param callback 完成回调。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_unregister_peer(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                         const char* peer_id, const char* session_token, const char* reason,
                                         ntrs_async_callback_t callback, void* user_data);

/**
 * @brief 异步发送 peer 心跳。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param peer_id 本端 peer id。
 * @param session_token 认证后获得的 session token。
 * @param lease_seq 当前租约序号。
 * @param callback 完成回调。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_heartbeat(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                   const char* peer_id, const char* session_token, uint32_t lease_seq,
                                   ntrs_async_callback_t callback, void* user_data);

/**
 * @brief 异步创建到目标 peer 的 P2P 会话。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param src_peer_id 本端 peer id。
 * @param src_device_id 本端 device id。
 * @param dst_peer_id 目标 peer id。
 * @param dst_device_id 目标 device id, 可为空字符串表示自动选择。
 * @param session_token 认证后获得的 session token。
 * @param callback 完成回调。成功时读取 `result->session_signal`。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_create_session(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                        const char* src_peer_id, const char* src_device_id, const char* dst_peer_id,
                                        const char* dst_device_id, const char* session_token,
                                        ntrs_async_callback_t callback, void* user_data);

/**
 * @brief 异步等待控制面推送的会话信令。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param control_fd 已连接控制面的 socket fd。
 * @param timeout_ms 等待超时, 单位毫秒。
 * @param callback 完成回调。成功时读取 `result->session_signal`。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_wait_for_signal(ntrs_async_client_t* client, uint64_t* request_id, int32_t control_fd,
                                         int32_t timeout_ms, ntrs_async_callback_t callback, void* user_data);

/**
 * @brief 异步尝试 UDP 打洞并选择可用候选地址。
 *
 * @param client 异步客户端句柄。
 * @param request_id 输出请求 id, 可为 NULL。
 * @param udp_sock 已绑定的 UDP socket fd。
 * @param candidates 候选地址数组。
 * @param candidate_count 候选地址数量。
 * @param send_rounds 打洞发送轮数。
 * @param interval_ms 每轮发送间隔, 单位毫秒。
 * @param callback 完成回调。成功时读取 `result->selected_candidate`。
 * @param user_data 用户上下文。
 * @return 请求提交成功返回 true, 否则返回 false。
 */
NTRS_API bool ntrs_async_try_udp_hole_punch(ntrs_async_client_t* client, uint64_t* request_id, int32_t udp_sock,
                                            const ntrs_peer_candidate_t* candidates, uint32_t candidate_count,
                                            int32_t send_rounds, int32_t interval_ms,
                                            ntrs_async_callback_t callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif
