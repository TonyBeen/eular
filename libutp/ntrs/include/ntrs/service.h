#ifndef EULAR_NTRS_SERVICE_H
#define EULAR_NTRS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_NTRS_CONTROL_VERSION          1u
#define UTP_NTRS_CONTROL_HEADER_SIZE      8u
#define UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE 4096u
#define UTP_NTRS_NODE_ID_SIZE             16u
#define UTP_NTRS_BOOT_ID_SIZE             16u
#define UTP_NTRS_LINK_NONCE_SIZE          16u
#define UTP_NTRS_MAX_EXCLUDED_NODES       2u
#define UTP_NTRS_FORWARD_TOKEN_SIZE       12u
#define UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY  0x01u
#define UTP_NTRS_ASSIGNMENT_ROLE_BACKUP   0x02u
#define UTP_NTRS_UDP_DEFAULT_SOURCE_RATE  128u
#define UTP_NTRS_UDP_DEFAULT_SOURCE_BURST 256u

/** @brief NTRS 服务控制面的消息类型。 */
typedef enum utp_ntrs_control_type {
    UTP_NTRS_CONTROL_NODE_REGISTER           = 1,
    UTP_NTRS_CONTROL_NODE_REGISTER_OK        = 2,
    UTP_NTRS_CONTROL_NODE_REGISTER_REJECT    = 3,
    UTP_NTRS_CONTROL_NODE_ASSIGNMENT         = 4,
    UTP_NTRS_CONTROL_NODE_ASSIGNMENT_REQUEST = 5,
    UTP_NTRS_CONTROL_NODE_HEARTBEAT          = 6,
    UTP_NTRS_CONTROL_NODE_LINK_HELLO         = 7,
    UTP_NTRS_CONTROL_NODE_LINK_DUPLICATE     = 8,
    UTP_NTRS_CONTROL_NODE_LINK_PING          = 9,
    UTP_NTRS_CONTROL_NODE_LINK_PONG          = 10,
    UTP_NTRS_CONTROL_NAT_FORWARD_FILTER_RSP  = 11,
} utp_ntrs_control_type_t;

/** @brief 服务端使用的二进制 IP endpoint；port 为主机字节序。 */
typedef struct utp_ntrs_endpoint {
    uint8_t  family;       // AF_INET 或 AF_INET6
    uint16_t port;         // 主机字节序端口
    uint8_t  address[16];  // IPv4 仅使用前 4 字节
} utp_ntrs_endpoint_t;

/** @brief Node 实例标识；node_id 相同而 boot_id 不同代表重启后的新实例。 */
typedef struct utp_ntrs_node_instance {
    uint8_t node_id[UTP_NTRS_NODE_ID_SIZE];  // 稳定 Node 标识
    uint8_t boot_id[UTP_NTRS_BOOT_ID_SIZE];  // 本次启动随机标识
} utp_ntrs_node_instance_t;

/** @brief 一个地址族上 Node 的对外服务信息。 */
typedef struct utp_ntrs_node_family {
    utp_ntrs_endpoint_t public_endpoint;       // 公网 IP，port 必须为 0
    utp_ntrs_endpoint_t probe_endpoint;        // 客户端 PROBE1/PROBE2 的 UDP endpoint
    utp_ntrs_endpoint_t change_port_endpoint;  // 与 probe 同 IP、不同端口的 UDP endpoint
    utp_ntrs_endpoint_t control_endpoint;      // Node 间 TCP+TLS 控制 endpoint
    uint8_t             family;                // AF_INET 或 AF_INET6
    bool                valid;                 // 当前地址族是否由 Node 提供
} utp_ntrs_node_family_t;

/** @brief Node 向 Hub 注册或更新的完整状态。 */
typedef struct utp_ntrs_node_registration {
    utp_ntrs_node_instance_t instance;      // Node 实例
    utp_ntrs_node_family_t   ipv4;          // IPv4 服务信息
    utp_ntrs_node_family_t   ipv6;          // IPv6 服务信息
    uint32_t                 load;          // Node 当前负载，越小越优先
    uint32_t                 heartbeat_ms;  // Hub 心跳周期
} utp_ntrs_node_registration_t;

/** @brief Hub 在注册成功时回传的 Node 对外地址，仅携带地址，port 必须为 0。 */
typedef struct utp_ntrs_registration_ok {
    utp_ntrs_endpoint_t public_endpoint;  // Hub 从 Node 控制连接观察到的公网地址
} utp_ntrs_registration_ok_t;

/** @brief Hub 对一个地址族下发的主备协同 Node。 */
typedef struct utp_ntrs_assignment {
    utp_ntrs_node_instance_t primary;          // 主协同 Node 实例
    utp_ntrs_node_instance_t backup;           // 备协同 Node 实例
    utp_ntrs_endpoint_t      primary_probe;    // 主 Node UDP probe endpoint
    utp_ntrs_endpoint_t      primary_control;  // 主 Node TCP+TLS control endpoint
    utp_ntrs_endpoint_t      backup_probe;     // 备 Node UDP probe endpoint
    utp_ntrs_endpoint_t      backup_control;   // 备 Node TCP+TLS control endpoint
    uint64_t                 version;          // 每个 Node、每地址族单调递增的版本
    uint8_t                  family;           // 本 assignment 所属地址族
    bool                     has_primary;      // 是否存在主协同 Node
    bool                     has_backup;       // 是否存在备协同 Node
} utp_ntrs_assignment_t;

/** @brief Hub 的内部节点表。调用方只可将其作为不透明存储使用。 */
typedef struct utp_ntrs_hub {
    void*  nodes;     // 内部动态 Node 表
    size_t count;     // 当前在线 Node 数量
    size_t capacity;  // 动态表容量
} utp_ntrs_hub_t;

/** @brief Node 间一条已建立控制连接的去重状态。 */
typedef struct utp_ntrs_node_link {
    utp_ntrs_node_instance_t remote;                                     // 对端实例
    uint8_t                  initiator_node_id[UTP_NTRS_NODE_ID_SIZE];   // 保留连接的发起 Node
    uint8_t                  initiator_nonce[UTP_NTRS_LINK_NONCE_SIZE];  // 保留连接的发起随机数
    bool                     active;                                     // 是否已有待复用连接
} utp_ntrs_node_link_t;

/** @brief 控制连接解码出的固定头部。 */
typedef struct utp_ntrs_control_header {
    uint32_t payload_length;  // payload 长度，不含 8 字节头
    uint8_t  type;            // utp_ntrs_control_type_t
} utp_ntrs_control_header_t;

/** @brief Node 心跳；线格式为 instance:32 | load:u32。 */
typedef struct utp_ntrs_node_heartbeat {
    utp_ntrs_node_instance_t instance;  // 发送心跳的当前 Node 实例
    uint32_t                 load;      // 当前负载
} utp_ntrs_node_heartbeat_t;

/** @brief Node 请求替换失效协同节点；失效角色必须与当前 assignment 相符。 */
typedef struct utp_ntrs_assignment_request {
    utp_ntrs_node_instance_t instance;            // 发起请求的 Node 实例
    utp_ntrs_node_instance_t failed_primary;      // failed_roles 包含 PRIMARY 时的失效实例
    utp_ntrs_node_instance_t failed_backup;       // failed_roles 包含 BACKUP 时的失效实例
    uint64_t                 assignment_version;  // Node 当前已接受 assignment 版本
    uint8_t                  family;              // AF_INET 或 AF_INET6
    uint8_t                  failed_roles;        // UTP_NTRS_ASSIGNMENT_ROLE_*
} utp_ntrs_assignment_request_t;

/** @brief Node 间控制链路的首条消息；发起标识用于双向拨号去重。 */
typedef struct utp_ntrs_node_link_hello {
    utp_ntrs_node_instance_t instance;                                   // 发送 HELLO 的 Node 实例
    uint8_t                  initiator_node_id[UTP_NTRS_NODE_ID_SIZE];   // 建立该 TCP 连接的发起 Node
    uint8_t                  initiator_nonce[UTP_NTRS_LINK_NONCE_SIZE];  // 发起方为本连接生成的随机值
} utp_ntrs_node_link_hello_t;

/** @brief 主 Node 请求协同 Node 直接回送 CHANGE_IP 的 FILTER_RSP。 */
typedef struct utp_ntrs_forward_filter_response {
    utp_ntrs_node_instance_t target;                              // 必须等于接收 Node 的当前实例
    utp_ntrs_endpoint_t      client;                              // 客户端观察到的源 endpoint
    uint64_t                 forward_id;                          // 短期去重标识
    uint64_t                 packet_number;                       // 原 NAT 探测 UTP 包号
    uint8_t                  token[UTP_NTRS_FORWARD_TOKEN_SIZE];  // 原请求的 PROBE_TOKEN
    uint8_t                  phase;                               // 当前固定为 CHANGE_IP
} utp_ntrs_forward_filter_response_t;

typedef void (*utp_ntrs_control_message_fn)(void* user_data, uint8_t type, const uint8_t* payload,
                                            uint32_t payload_length);
typedef void (*utp_ntrs_udp_forward_fn)(void* user_data, const utp_ntrs_forward_filter_response_t* forward);

/** @brief TLS 控制流的有界消息重组器。 */
typedef struct utp_ntrs_control_stream {
    uint8_t  buffer[UTP_NTRS_CONTROL_MAX_MESSAGE_SIZE];  // 一条完整控制消息的暂存区
    uint32_t expected_length;                            // 当前消息完整长度；0 表示尚未获得头部
    uint32_t used;                                       // 已累计字节数
} utp_ntrs_control_stream_t;

/** @brief Linux Node UDP worker 的监听配置。 */
typedef struct utp_ntrs_udp_server_options {
    utp_ntrs_endpoint_t      probe_endpoint;            // PROBE1、CHANGE_IP、PROBE2 的本地监听 endpoint
    utp_ntrs_endpoint_t      change_port_endpoint;      // CHANGE_PORT 的本地监听 endpoint
    utp_ntrs_endpoint_t      public_probe_endpoint;     // 回包 ORIGIN_ADDR 使用的公网 probe endpoint
    utp_ntrs_endpoint_t      public_change_port_endpoint;  // 回包 ORIGIN_ADDR 使用的公网 change-port endpoint
    utp_ntrs_endpoint_t      alternate_probe_endpoint;  // 当前 primary Node 的 probe endpoint；无
                                                        // primary 时 family 为 0
    utp_ntrs_node_instance_t primary_instance;          // alternate endpoint 对应的 Node
    struct event_base*       main_base;                 // Node 主 libevent loop；NULL 时禁用 CHANGE_IP 转发
    utp_ntrs_udp_forward_fn  on_forward;                // worker 向主 loop 投递 CHANGE_IP 请求
    void*                    user_data;                 // on_forward 回调上下文
    const char*              interface_name;            // 绑定的 Linux 网卡；空指针表示不限制
    uint16_t                 worker_count;              // SO_REUSEPORT worker 数；0 时使用 1
    uint32_t                 source_rate_per_second;    // 单源 IP 每秒最大请求数；0 时使用默认值
    uint32_t                 source_burst;              // 单源 IP 突发请求数；0 时使用默认值
} utp_ntrs_udp_server_options_t;

typedef struct utp_ntrs_udp_server utp_ntrs_udp_server_t;

/** @brief 解码控制消息头；未知类型和保留位非零均视为协议错误。 */
bool   utp_ntrs_control_decode_header(const uint8_t* data, size_t length, utp_ntrs_control_header_t* header);
/** @brief 编码控制消息头，返回完整消息所需总长度；失败返回 0。 */
size_t utp_ntrs_control_encode_header(uint8_t* data, size_t capacity, uint8_t type, uint32_t payload_length);
/** @brief 编码 NODE_REGISTER 消息，返回完整长度；失败返回 0。 */
size_t utp_ntrs_control_encode_registration(uint8_t* data, size_t capacity,
                                            const utp_ntrs_node_registration_t* registration);
/** @brief 解码严格的 NODE_REGISTER 消息。 */
bool   utp_ntrs_control_decode_registration(const uint8_t* data, size_t length,
                                            utp_ntrs_node_registration_t* registration);
/** @brief 编码 NODE_REGISTER_OK 消息，携带 Hub 观察到的 Node 公网地址。 */
size_t utp_ntrs_control_encode_registration_ok(uint8_t* data, size_t capacity,
                                               const utp_ntrs_registration_ok_t* registration_ok);
/** @brief 解码严格的 NODE_REGISTER_OK 消息。 */
bool   utp_ntrs_control_decode_registration_ok(const uint8_t* data, size_t length,
                                               utp_ntrs_registration_ok_t* registration_ok);
/** @brief 编码 NODE_ASSIGNMENT 消息，返回完整长度；失败返回 0。 */
size_t utp_ntrs_control_encode_assignment(uint8_t* data, size_t capacity, const utp_ntrs_assignment_t* assignment);
/** @brief 解码严格的 NODE_ASSIGNMENT 消息。 */
bool   utp_ntrs_control_decode_assignment(const uint8_t* data, size_t length, utp_ntrs_assignment_t* assignment);
/** @brief 编码 NODE_HEARTBEAT 消息，返回完整长度；失败返回 0。 */
size_t utp_ntrs_control_encode_heartbeat(uint8_t* data, size_t capacity, const utp_ntrs_node_heartbeat_t* heartbeat);
/** @brief 解码严格的 NODE_HEARTBEAT 消息。 */
bool   utp_ntrs_control_decode_heartbeat(const uint8_t* data, size_t length, utp_ntrs_node_heartbeat_t* heartbeat);
/** @brief 编码 NODE_ASSIGNMENT_REQUEST 消息，返回完整长度；失败返回 0。 */
size_t utp_ntrs_control_encode_assignment_request(uint8_t* data, size_t capacity,
                                                  const utp_ntrs_assignment_request_t* request);
/** @brief 解码严格的 NODE_ASSIGNMENT_REQUEST 消息。 */
bool   utp_ntrs_control_decode_assignment_request(const uint8_t* data, size_t length,
                                                  utp_ntrs_assignment_request_t* request);
/** @brief 编码 NODE_LINK_HELLO 消息，返回完整长度；失败返回 0。 */
size_t utp_ntrs_control_encode_link_hello(uint8_t* data, size_t capacity, const utp_ntrs_node_link_hello_t* hello);
/** @brief 解码严格的 NODE_LINK_HELLO 消息。 */
bool   utp_ntrs_control_decode_link_hello(const uint8_t* data, size_t length, utp_ntrs_node_link_hello_t* hello);
/** @brief 编码 NAT_FORWARD_FILTER_RESPONSE 消息，返回完整长度；失败返回 0。 */
size_t utp_ntrs_control_encode_forward_filter_response(uint8_t* data, size_t capacity,
                                                       const utp_ntrs_forward_filter_response_t* forward);
/** @brief 解码严格的 NAT_FORWARD_FILTER_RESPONSE 消息。 */
bool   utp_ntrs_control_decode_forward_filter_response(const uint8_t* data, size_t length,
                                                       utp_ntrs_forward_filter_response_t* forward);
/** @brief 初始化 TLS 控制流重组状态。 */
void   utp_ntrs_control_stream_init(utp_ntrs_control_stream_t* stream);
/** @brief 喂入任意长度 TLS 明文字节；非法前缀返回 false。 */
bool   utp_ntrs_control_stream_feed(utp_ntrs_control_stream_t* stream, const uint8_t* data, size_t length,
                                    utp_ntrs_control_message_fn callback, void* user_data);

/** @brief 初始化 Hub 节点表。 */
void   utp_ntrs_hub_init(utp_ntrs_hub_t* hub);
/** @brief 释放 Hub 动态节点表。 */
void   utp_ntrs_hub_destroy(utp_ntrs_hub_t* hub);
/** @brief 注册或更新 Node；不同 boot_id 会替换同 node_id 的旧实例。 */
bool   utp_ntrs_hub_register(utp_ntrs_hub_t* hub, const utp_ntrs_node_registration_t* registration, uint64_t now_ms,
                             bool* replaced);
/** @brief 接收 Node 心跳；实例不匹配时失败，防止旧进程续租新实例。 */
bool   utp_ntrs_hub_heartbeat(utp_ntrs_hub_t* hub, const utp_ntrs_node_instance_t* instance, uint32_t load,
                              uint64_t now_ms);
/** @brief 移除精确匹配的 Node 实例；控制连接断开时立即调用。 */
bool   utp_ntrs_hub_remove(utp_ntrs_hub_t* hub, const utp_ntrs_node_instance_t* instance, uint64_t now_ms);
/** @brief 淘汰连续三次心跳周期未更新的 Node，返回淘汰数量。 */
size_t utp_ntrs_hub_sweep_expired(utp_ntrs_hub_t* hub, uint64_t now_ms);
/** @brief 获取指定 Node、地址族的当前主备 assignment。 */
bool   utp_ntrs_hub_get_assignment(const utp_ntrs_hub_t* hub, const utp_ntrs_node_instance_t* instance, uint8_t family,
                                   utp_ntrs_assignment_t* assignment);
/** @brief 仅替换请求中标记的失效角色；健康角色保持不变。 */
bool utp_ntrs_hub_request_assignment(utp_ntrs_hub_t* hub, const utp_ntrs_assignment_request_t* request, uint64_t now_ms,
                                     utp_ntrs_assignment_t* assignment);

/** @brief 比较两个 Node 实例是否完全相同。 */
bool utp_ntrs_node_instance_equal(const utp_ntrs_node_instance_t* left, const utp_ntrs_node_instance_t* right);
/** @brief 考虑一条 Node 间连接；返回 true 表示保留新连接，replaced
 * 表示应关闭旧连接。 */
bool utp_ntrs_node_link_consider(utp_ntrs_node_link_t* link, const utp_ntrs_node_instance_t* remote,
                                 const uint8_t initiator_node_id[UTP_NTRS_NODE_ID_SIZE],
                                 const uint8_t initiator_nonce[UTP_NTRS_LINK_NONCE_SIZE], bool* replaced);

/** @brief 启动 Linux SO_REUSEPORT UDP worker；每个 worker 有独立 libevent
 * loop。 */
utp_ntrs_udp_server_t* utp_ntrs_udp_server_start(const utp_ntrs_udp_server_options_t* options);
/** @brief 停止全部 UDP worker 并同步释放资源。 */
void                   utp_ntrs_udp_server_stop(utp_ntrs_udp_server_t* server);
/** @brief 原子更新已激活的 primary Node；任一参数为 NULL 时停止发布新的辅助
 * endpoint。 */
void utp_ntrs_udp_server_set_primary(utp_ntrs_udp_server_t* server, const utp_ntrs_node_instance_t* primary_instance,
                                     const utp_ntrs_endpoint_t* primary_endpoint);
/** @brief 原子更新回包公告 endpoint；监听 socket 不受影响。 */
void utp_ntrs_udp_server_set_public_endpoints(utp_ntrs_udp_server_t* server,
                                              const utp_ntrs_endpoint_t* probe_endpoint,
                                              const utp_ntrs_endpoint_t* change_port_endpoint);
/** @brief 由协同 Node 从自身 probe socket 直接发送一次 FILTER_RSP。 */
bool utp_ntrs_udp_server_send_filter_response(utp_ntrs_udp_server_t*                    server,
                                              const utp_ntrs_forward_filter_response_t* forward);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_NTRS_SERVICE_H
