#ifndef EULAR_UTP_C_NAT_H
#define EULAR_UTP_C_NAT_H

#include <stdint.h>

#include <utp/context.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_NAT_PORT_SAMPLE_CAPACITY 6u

/** @brief NAT 探测得出的映射与过滤分类。 */
typedef enum utp_nat_class {
    UTP_NAT_CLASS_UNKNOWN                   = 0,
    UTP_NAT_CLASS_OPEN_PUBLIC               = 1,
    UTP_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL = 2,
    UTP_NAT_CLASS_FULL_CONE                 = 3,
    UTP_NAT_CLASS_IP_RESTRICTED             = 4,
    UTP_NAT_CLASS_PORT_RESTRICTED           = 5,
    UTP_NAT_CLASS_SYMMETRIC                 = 6,
    UTP_NAT_CLASS_SYMMETRIC_MULTI_LINE      = 7,
    UTP_NAT_CLASS_UDP_BLOCKED               = 8,
} utp_nat_class_t;

/** @brief 单个 NAT 探测阶段的超时配置。 */
typedef struct utp_nat_probe_options {
    const char* nat_service_address;  // NAT 服务 IPv4/IPv6 字面 IP，仅在调用期间借用
    uint16_t    nat_service_port;     // NAT 服务 UDP 端口
    uint32_t    phase_timeout_ms;     // 每阶段总时限；0 时采用 3000 ms
} utp_nat_probe_options_t;

#define UTP_NAT_PROBE_OPTIONS_INIT {NULL, 0u, 0u}

/** @brief NAT 探测的只读结果视图，仅在回调期间有效。 */
typedef struct utp_nat_probe_result {
    utp_endpoint_t  primary_mapped_endpoint;                      // 主探测端点观察到的公网映射
    utp_endpoint_t  secondary_mapped_endpoint;                    // 辅助探测端点观察到的公网映射；无结果时 family 为 0
    uint64_t        probe_time_us;                                // 本次探测完成的单调时钟时刻
    uint64_t        expires_at_us;                                // NAT 结果的单调时钟失效时刻
    int32_t         primary_rtt_ms;                               // 主探测端点平均 RTT；无成功响应时为 -1
    int32_t         secondary_rtt_ms;                             // 辅助探测端点平均 RTT；无成功响应时为 -1
    uint16_t        port_samples[UTP_NAT_PORT_SAMPLE_CAPACITY];   // 去重后的公网映射端口样本
    uint8_t         address_family;                               // 本次 Context bind 的地址族
    uint8_t         port_sample_count;                            // port_samples 中有效项数量
    utp_nat_class_t nat_class;                                    // 最终 NAT 分类
} utp_nat_probe_result_t;

/** @brief NAT 探测完成回调；失败时 @p result 为 NULL。 */
typedef void (*utp_on_nat_probe_fn)(utp_context_t* context, utp_status_t status,
                                    const utp_nat_probe_result_t* result, void* user_data);

/** @brief 启动异步 NAT 探测；同一 Context 同时仅允许一个任务。 */
utp_status_t utp_context_probe_nat(utp_context_t* context, const utp_nat_probe_options_t* options,
                                   utp_on_nat_probe_fn callback, void* user_data);
/** @brief 同步取消正在执行的 NAT 探测，主动取消不触发原完成回调。 */
utp_status_t utp_context_cancel_nat_probe(utp_context_t* context);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_NAT_H
