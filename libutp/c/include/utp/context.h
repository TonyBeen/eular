#ifndef EULAR_UTP_C_CONTEXT_H
#define EULAR_UTP_C_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/log.h>
#include <utp/option.h>
#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_VERSION_MAJOR  1u
#define UTP_VERSION_MINOR  0u
#define UTP_VERSION_PATCH  1u
#define UTP_VERSION_STRING "1.0.1"

// 对外只暴露不透明句柄，对象内存均由对应的上级对象管理。
typedef struct utp_context    utp_context_t;
typedef struct utp_connection utp_connection_t;
typedef struct utp_stream     utp_stream_t;
struct event_base;

typedef enum utp_encryption_mode {
    UTP_ENCRYPTION_NONE,
    UTP_ENCRYPTION_AES_GCM_128,
    UTP_ENCRYPTION_AES_GCM_256,
} utp_encryption_mode_t;

typedef enum utp_connect_attempt_type {
    UTP_CONNECT_ATTEMPT_NORMAL,
    UTP_CONNECT_ATTEMPT_ZERO_RTT_TOKEN,
    UTP_CONNECT_ATTEMPT_ZERO_RTT_STATE,
    UTP_CONNECT_ATTEMPT_PASSIVE,
} utp_connect_attempt_type_t;

typedef struct utp_endpoint {
    uint8_t  family;
    uint16_t port;
    uint32_t scope_id;
    uint8_t  address[16];
} utp_endpoint_t;

// Context 配置的完整默认值；创建 Context 前必须由调用方设置 event_base。
#define UTP_CONTEXT_OPTIONS_INIT  \
    {NULL,                        \
     NULL,                        \
     0u,                          \
     UTP_LOG_LEVEL_INFO,          \
     UTP_STREAM_SCHEDULER_STRICT, \
     UTP_CONGESTION_BBR,          \
     true,                        \
     1280u,                       \
     1500u,                       \
     1400u,                       \
     300u,                        \
     16u,                         \
     2000u,                       \
     1u,                          \
     3u,                          \
     3000u,                       \
     5000u,                       \
     600u,                        \
     4096u,                       \
     800u,                        \
     2u,                          \
     true,                        \
     0u,                          \
     1500u,                       \
     3u,                          \
     30000u,                      \
     4u,                          \
     3u,                          \
     25u,                         \
     32u,                         \
     16u,                         \
     UINT64_C(8) * 1024u * 1024u, \
     UINT64_C(256) * 1024u,       \
     UINT64_C(256) * 1024u}

typedef struct utp_connect_options {
    const char*           address;
    uint16_t              port;
    uint32_t              timeout_ms;
    int8_t                retries;
    utp_encryption_mode_t encryption;
} utp_connect_options_t;

// 主动连接配置的完整默认值。
#define UTP_CONNECT_OPTIONS_INIT {NULL, 0u, 3000u, 0, UTP_ENCRYPTION_NONE}

typedef struct utp_connect_0rtt_options {
    const char*    address;
    uint16_t       port;
    uint32_t       timeout_ms;
    int8_t         retries;
    const uint8_t* session_token;
    size_t         session_token_size;
    const uint8_t* early_data;
    size_t         early_data_size;
    bool           early_fin;
} utp_connect_0rtt_options_t;

// early_data 可被网络重放，调用方只能放入幂等或自行去重的应用数据。
#define UTP_CONNECT_0RTT_OPTIONS_INIT {NULL, 0u, 3000u, 0, NULL, 0u, NULL, 0u, false}

typedef struct utp_new_connection_info {
    utp_endpoint_t        remote;
    uint32_t              local_cid;
    uint32_t              peer_cid;
    utp_encryption_mode_t encryption;
} utp_new_connection_info_t;

typedef struct utp_connect_attempt_info {
    utp_endpoint_t             remote;
    uint32_t                   timeout_ms;
    int8_t                     retries;
    utp_encryption_mode_t      encryption;
    utp_connect_attempt_type_t type;
    uint32_t                   session_token_size;
    uint32_t                   resumption_state_size;
    uint32_t                   early_data_size;
    bool                       early_fin;
} utp_connect_attempt_info_t;

typedef void (*utp_on_connected_fn)(utp_connection_t* connection, void* user_data);
typedef void (*utp_on_connect_error_fn)(utp_status_t status, const char* message,
                                        const utp_connect_attempt_info_t* attempt, void* user_data);
typedef bool (*utp_on_new_connection_fn)(const utp_new_connection_info_t* info, void* user_data);
typedef struct utp_connection_error_info {
    // UTP_STATUS_OK 表示对端正常关闭，本地致命错误使用对应的终止状态码。
    utp_status_t   status;
    // 对端 CONNECTION_CLOSE 中的原始错误码；本地错误固定为 0。
    uint16_t       peer_error_code;
    // 零拷贝的关闭原因视图，仅在回调执行期间有效。
    const uint8_t* reason;
    size_t         reason_length;
    bool           peer_initiated;
} utp_connection_error_info_t;

typedef void (*utp_on_connection_error_fn)(utp_connection_t* connection, const utp_connection_error_info_t* info,
                                           void* user_data);

/** @brief 返回当前链接的 C 库语义版本字符串。 */
const char*  utp_version(void);
/** @brief 按选项创建 Context；成功时由 @p out_context 返回所有权。 */
utp_status_t utp_context_create(const utp_context_options_t* options, utp_context_t** out_context);
/** @brief 同步销毁 Context 管理的连接和流；已建立连接仅尽力发送一次 CONNECTION_CLOSE。 */
void         utp_context_destroy(utp_context_t* context);
/** @brief 绑定 UDP 地址及可选网卡，空 @p ifname 表示不绑定特定网卡。 */
utp_status_t utp_context_bind(utp_context_t* context, const char* address, uint16_t port, const char* ifname,
                              uint16_t* out_port);
/** @brief 动态设置日志回调和最低输出级别；非线程安全，NULL 回调会关闭日志输出。 */
void         utp_context_set_logger(utp_context_t* context, utp_log_sink_fn callback, utp_log_level_t level);
/** @brief 设置连接建立成功回调，回调内获得的连接由 Context 持有。 */
void         utp_context_set_on_connected(utp_context_t* context, utp_on_connected_fn callback, void* user_data);
/** @brief 设置主动建连失败回调。 */
void utp_context_set_on_connect_error(utp_context_t* context, utp_on_connect_error_fn callback, void* user_data);
/** @brief 设置被动建连决策回调，返回 false 会拒绝该连接。 */
void utp_context_set_on_new_connection(utp_context_t* context, utp_on_new_connection_fn callback, void* user_data);
/** @brief 设置被动关闭或本地致命错误回调，关闭原因仅在回调期间有效。 */
void utp_context_set_on_connection_error(utp_context_t* context, utp_on_connection_error_fn callback, void* user_data);
/** @brief 设置恢复根密钥；替换后立即废止该 Context 已签发的恢复凭证。 */
void utp_context_set_resumption_key(utp_context_t* context, const uint8_t root_key[32]);
/** @brief 发起异步主动连接，建连结果通过回调报告。 */
utp_status_t utp_context_connect(utp_context_t* context, const utp_connect_options_t* options);
/** @brief 基于会话票据发起非加密 0-RTT 建连；早数据固定写入客户端首个双向流，可能被重放。 */
utp_status_t utp_context_connect_0rtt(utp_context_t* context, const utp_connect_0rtt_options_t* options);
/** @brief 接受一个已通过 on_new_connection 回调的 pending 被动连接。 */
utp_status_t utp_context_accept(utp_context_t* context);

#ifdef __cplusplus
}
#endif

#include <utp/connection.h>
#include <utp/stream.h>

#endif  // EULAR_UTP_C_CONTEXT_H
