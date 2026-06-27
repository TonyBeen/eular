#ifndef __NTRS_PROBE_TYPES_H__
#define __NTRS_PROBE_TYPES_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NTRS 探测 token 的默认最大字节数。
 *
 * token 在线上传输时必须使用二进制字段承载，不允许通过文本前缀参与消息 framing。
 */
#define NTRS_PROBE_TOKEN_MAX_SIZE 64u

/**
 * @brief NTRS 当前线上探测 token 的固定字节数。
 *
 * 迁移阶段为了兼容既有节点协同与控制面转发逻辑，线上 token 仍固定为 12 字节。
 * 该常量属于 NTRS 私有协议定义，不应再继续引用旧公开协议里的 transaction-id
 * 长度常量。
 */
#define NTRS_PROBE_TOKEN_WIRE_SIZE 12u

/**
 * @brief NTRS 节点标识、peer 标识等文本字段的默认最大长度。
 *
 * 文本字段只允许作为值内容存在，外层仍需通过二进制 TLV 或定长二进制字段承载。
 */
#define NTRS_PROBE_TEXT_MAX_LEN 128u

/**
 * @brief NTRS 探测/打洞状态。
 *
 * 该状态用于区分“偶发丢包、高概率不可达、本地发送失败、明确 ICMP 不可达”等
 * 不同失败类型，避免继续使用单一布尔值表达所有诊断结果。
 */
typedef enum ntrs_probe_status {
    NTRS_PROBE_STATUS_UNKNOWN = 0,
    NTRS_PROBE_STATUS_SUCCESS = 1,
    NTRS_PROBE_STATUS_PROBABLE_LOSS = 2,
    NTRS_PROBE_STATUS_NO_RESPONSE = 3,
    NTRS_PROBE_STATUS_BLOCKED = 4,
    NTRS_PROBE_STATUS_LOCAL_SEND_FAILED = 5,
    NTRS_PROBE_STATUS_ICMP_UNREACHABLE = 6,
    NTRS_PROBE_STATUS_TOKEN_REJECTED = 7,
    NTRS_PROBE_STATUS_PROTOCOL_REJECTED = 8,
    NTRS_PROBE_STATUS_TIMED_OUT = 9,
} ntrs_probe_status_t;

/**
 * @brief NTRS 探测失败原因码。
 */
typedef enum ntrs_probe_reason {
    NTRS_PROBE_REASON_NONE = 0,
    NTRS_PROBE_REASON_RECV_TIMEOUT = 1,
    NTRS_PROBE_REASON_ACK_MISMATCH = 2,
    NTRS_PROBE_REASON_LOCAL_SOCKET_ERROR = 3,
    NTRS_PROBE_REASON_REMOTE_SILENT_DROP = 4,
    NTRS_PROBE_REASON_REMOTE_REJECT = 5,
    NTRS_PROBE_REASON_TOKEN_INVALID = 6,
    NTRS_PROBE_REASON_PROTOCOL_INVALID = 7,
    NTRS_PROBE_REASON_PATH_MTU_EXCEEDED = 8,
    NTRS_PROBE_REASON_ICMP_ERROR = 9,
} ntrs_probe_reason_t;

/**
 * @brief 探测 token 容器。
 */
typedef struct ntrs_probe_token {
    uint8_t bytes[NTRS_PROBE_TOKEN_MAX_SIZE];
    uint8_t length;
} ntrs_probe_token_t;

/**
 * @brief 结构化探测结果。
 */
typedef struct ntrs_probe_result {
    ntrs_probe_status_t status;
    ntrs_probe_reason_t reason;
    uint32_t            request_id;
    uint32_t            sequence;
    uint32_t            tx_count;
    uint32_t            rx_count;
    uint32_t            ack_count;
    uint32_t            timeout_count;
    int32_t             local_error;
    int32_t             icmp_error;
    bool                icmp_seen;
    uint32_t            rtt_min_ms;
    uint32_t            rtt_avg_ms;
    uint32_t            rtt_max_ms;
} ntrs_probe_result_t;

/**
 * @brief 将探测 token 清零初始化。
 *
 * @param token 待初始化 token。
 */
void ntrs_probe_token_init(ntrs_probe_token_t* token);

/**
 * @brief 将结构化探测结果清零初始化。
 *
 * @param result 待初始化结果对象。
 */
void ntrs_probe_result_init(ntrs_probe_result_t* result);

#ifdef __cplusplus
}
#endif

#endif
