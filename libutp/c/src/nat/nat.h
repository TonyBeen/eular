#ifndef EULAR_UTP_INTERNAL_NAT_H
#define EULAR_UTP_INTERNAL_NAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/nat.h>

#include "socket/address.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_NAT_PROBE_VERSION          1u
#define UTP_NAT_PROBE_TOKEN_SIZE       12u
#define UTP_NAT_PROBE_PACKET_SIZE      128u
#define UTP_NAT_PROBE_MAX_IN_FLIGHT    6u
#define UTP_NAT_PROBE_BATCH_SIZE       2u
#define UTP_NAT_PROBE_MAX_ROUNDS       3u
#define UTP_NAT_PROBE_DEFAULT_TIMEOUT  3000u
#define UTP_NAT_PROBE_MIN_TIMEOUT      6u

typedef enum utp_nat_probe_message_type {
    UTP_NAT_PROBE_MESSAGE_PROBE_REQ  = 1,
    UTP_NAT_PROBE_MESSAGE_PROBE_RSP  = 2,
    UTP_NAT_PROBE_MESSAGE_FILTER_REQ = 3,
    UTP_NAT_PROBE_MESSAGE_FILTER_RSP = 4,
} utp_nat_probe_message_type_t;

typedef enum utp_nat_probe_phase {
    UTP_NAT_PROBE_PHASE_NONE        = 0,
    UTP_NAT_PROBE_PHASE_PROBE1      = 1,
    UTP_NAT_PROBE_PHASE_CHANGE_PORT = 2,
    UTP_NAT_PROBE_PHASE_CHANGE_IP   = 3,
    UTP_NAT_PROBE_PHASE_PROBE2      = 4,
} utp_nat_probe_phase_t;

typedef enum utp_nat_probe_tlv_type {
    UTP_NAT_PROBE_TLV_PROBE_TOKEN                = 1,
    UTP_NAT_PROBE_TLV_MAPPED_ADDR                = 6,
    UTP_NAT_PROBE_TLV_ORIGIN_ADDR                = 7,
    UTP_NAT_PROBE_TLV_ALTERNATE_PROBE_ENDPOINT   = 8,
    UTP_NAT_PROBE_TLV_PADDING                    = 12,
} utp_nat_probe_tlv_type_t;

typedef struct utp_nat_probe_record {
    uint64_t packet_number;                         // 请求使用的 UTP 包号
    uint64_t sent_at_us;                            // 内核接受发送的单调时刻
    uint8_t  token[UTP_NAT_PROBE_TOKEN_SIZE];       // 本包专属关联 token
    bool     used : 1;                              // 当前阶段是否仍保留该记录
    bool     consumed : 1;                          // 是否已接受过对应响应
} utp_nat_probe_record_t;

typedef struct utp_nat_probe_response {
    utp_address_t mapped;                     // 服务端观察到的公网映射
    utp_address_t origin;                     // 服务端实际响应 endpoint
    utp_address_t alternate;                  // 不同公网 IP 的辅助探测 endpoint
    const uint8_t* token;                     // 请求 token 的零拷贝视图
    size_t         token_length;              // token 长度
    uint8_t        message_type;              // utp_nat_probe_message_type_t
    uint8_t        phase;                     // utp_nat_probe_phase_t
    bool           has_alternate : 1;         // 是否携带辅助 endpoint
} utp_nat_probe_response_t;

typedef struct utp_nat_probe_task {
    utp_address_t                  primary_endpoint;                         // 本次调用解析得到的主 NAT endpoint
    utp_address_t                  alternate_endpoint;                       // PROBE1 响应下发的辅助 endpoint
    utp_nat_probe_result_t         result;                                   // 回调前构造的结果快照
    utp_on_nat_probe_fn            callback;                                 // 用户完成回调
    void*                          user_data;                                // 用户回调数据
    uint64_t                       phase_deadline_us;                        // 当前阶段总截止时刻
    uint64_t                       phase_started_us;                         // 当前阶段开始时刻
    uint64_t                       round_deadline_us;                        // 当前轮次截止时刻
    uint64_t                       primary_rtt_sum_us;                       // 主端点 RTT 累计
    uint64_t                       secondary_rtt_sum_us;                     // 辅助端点 RTT 累计
    utp_nat_probe_record_t         records[UTP_NAT_PROBE_MAX_IN_FLIGHT];     // 当前阶段全部在途请求
    uint32_t                       phase_timeout_ms;                         // 单阶段总时限
    uint8_t                        phase;                                    // utp_nat_probe_phase_t
    uint8_t                        round;                                    // 已开始的发送轮数
    uint8_t                        record_count;                             // records 中已分配的记录数
    uint8_t                        batch_sent_count;                         // 当前轮次已写入内核的请求数
    uint8_t                        primary_response_count;                   // 主映射有效响应次数
    uint8_t                        secondary_response_count;                 // 辅映射有效响应次数
    bool                           active : 1;                                // 是否有正在执行的探测
    bool                           alternate_valid : 1;                       // 是否有可用辅助 endpoint
    bool                           change_port_succeeded : 1;                 // 同 IP 异端口过滤成功
    bool                           change_ip_succeeded : 1;                   // 异 IP 过滤成功
    bool                           primary_mapping_changed : 1;               // 主探测阶段出现不同映射
    bool                           secondary_mapping_changed : 1;             // 辅探测阶段出现不同映射
    bool                           write_pending : 1;                          // 当前轮次等待 UDP 可写后续发
} utp_nat_probe_task_t;

/** @brief 将 NAT 任务恢复为空闲状态，不触发回调。 */
void utp_nat_probe_task_reset(utp_nat_probe_task_t* task);
/** @brief 构造固定 128 字节、带 UTP 包头和 PADDING TLV 的 NAT 探测请求。 */
utp_internal_error_t utp_nat_probe_encode_request(uint8_t packet[UTP_NAT_PROBE_PACKET_SIZE], uint64_t packet_number,
                                                   uint8_t message_type, uint8_t phase,
                                                   const uint8_t token[UTP_NAT_PROBE_TOKEN_SIZE]);
/** @brief 解码 NAT 探测响应 payload；输出中的 TLV 数据均借用输入缓冲。 */
utp_internal_error_t utp_nat_probe_decode_response(const uint8_t* payload, size_t payload_length,
                                                    utp_nat_probe_response_t* response);
/** @brief 返回阶段请求应使用的消息类型。 */
uint8_t utp_nat_probe_request_message_type(uint8_t phase);
/** @brief 返回阶段响应应使用的消息类型。 */
uint8_t utp_nat_probe_response_message_type(uint8_t phase);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_NAT_H
