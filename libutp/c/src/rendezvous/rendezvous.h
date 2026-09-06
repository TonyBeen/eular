#ifndef EULAR_UTP_INTERNAL_RENDEZVOUS_H
#define EULAR_UTP_INTERNAL_RENDEZVOUS_H

#include <stddef.h>
#include <stdint.h>

#include <utp/option.h>

#include "proto/frame.h"
#include "socket/address.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_RENDEZVOUS_ID_SIZE                 16u
#define UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE 8u
#define UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE        8u
#define UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES    4u
#define UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES     4u

/* REGISTER：请求标识、当前 token、Context 身份与本地候选地址。 */
typedef struct utp_rendezvous_register {
    const uint8_t*       peer_id;                   // Context peer_id 的字节视图
    const utp_address_t* local_candidates;          // 最多四个本地候选地址
    const utp_address_t* reported_public_endpoint;  // NAT 探测得出的公网 endpoint，可为空
    uint8_t              registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    utp_address_t        decoded_reported_public_endpoint;
    utp_address_t        decoded_local_candidates[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];
    uint64_t             registration_request_id;  // 首次注册及其重传保持不变
    uint16_t             local_port;               // Context 已绑定 UDP 端口
    uint8_t              peer_id_length;           // 1..128
    uint8_t              nat_class;                // utp_nat_class_t 的线上值
    uint8_t              local_family;             // 4 或 6
    uint8_t              local_candidate_count;    // 0..4
} utp_rendezvous_register_t;

/* REGISTERED：注册确认和仅对对称 NAT 下发的初始 calibration endpoint。 */
typedef struct utp_rendezvous_registered {
    const utp_address_t* calibration_endpoints;  // 最多四个 calibration endpoint
    uint8_t              registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    utp_address_t        decoded_calibration_endpoints[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];
    uint64_t             registration_request_id;
    uint64_t             calibration_id;              // 非对称 NAT 时固定为 0
    uint8_t              calibration_endpoint_count;  // 0..4
} utp_rendezvous_registered_t;

/* PING：用于 Context/NTRS 半连接保活及 calibration 采样。 */
typedef struct utp_rendezvous_ping {
    uint8_t  registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    uint64_t calibration_id;  // 普通保活固定为 0
} utp_rendezvous_ping_t;

/* CALIBRATE：NTRS 要求本轮主动连接从指定副端口发出 PING，以采集对称 NAT 端口样本。 */
typedef struct utp_rendezvous_calibrate {
    const utp_address_t* endpoints;  // 1..4 个 NTRS 副端口
    uint8_t              rendezvous_id[UTP_RENDEZVOUS_ID_SIZE];
    uint8_t              calibration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    utp_address_t        decoded_endpoints[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];
    uint64_t             calibration_id;
    uint8_t              endpoint_count;
} utp_rendezvous_calibrate_t;

/* PONG：确认携带该 token 的关联和指定的半连接包号。 */
typedef struct utp_rendezvous_pong {
    uint8_t  registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    uint64_t acknowledged_packet_number;
} utp_rendezvous_pong_t;

/* ADDRESS_UPDATE：注册 Context 批量上报被连接对端观察到的公网地址。 */
typedef struct utp_rendezvous_address_update {
    const utp_address_t* samples;  // 1..4 个观测样本
    uint8_t              registration_token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE];
    utp_address_t        decoded_samples[UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES];
    uint64_t             observed_at_unix_ms[UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES];
    uint64_t             update_id;  // 同一批重传保持不变
    uint8_t              sample_count;
} utp_rendezvous_address_update_t;

/* ADDRESS_UPDATED：NTRS 已处理指定 update_id，不表达每条样本的采纳结果。 */
typedef struct utp_rendezvous_address_updated {
    uint64_t update_id;
} utp_rendezvous_address_updated_t;

/* REQUEST：rendezvous_id(16), source_id, target_id, NAT 信息和本地候选地址。 */
typedef struct utp_rendezvous_request {
    const uint8_t*       source_peer_id;                         // 请求方 Context peer_id 的字节视图
    const uint8_t*       target_peer_id;                         // 目标节点 peer_id 的字节视图
    const utp_address_t* local_candidates;                       // 最多四个本地候选地址
    const utp_address_t* reported_public_endpoint;               // NAT 探测得出的公网 endpoint，可为空
    uint8_t              rendezvous_id[UTP_RENDEZVOUS_ID_SIZE];  // 本轮幂等键
    utp_address_t        decoded_reported_public_endpoint;       // 解码存储
    utp_address_t        decoded_local_candidates[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];  // 解码存储
    uint16_t             local_port;                                                     // Context 已绑定 UDP 端口
    uint8_t              source_peer_id_length;                                          // 1..128
    uint8_t              target_peer_id_length;                                          // 1..128
    uint8_t              source_nat_class;                                               // utp_nat_class_t 的线上值
    uint8_t              local_family;                                                   // 4 或 6
    uint8_t              local_candidate_count;                                          // 0..4
} utp_rendezvous_request_t;

/* CandidatePlan：本地候选地址及不可拆分的公网 IP:port 候选。 */
typedef struct utp_rendezvous_candidate_plan {
    const utp_address_t* local_candidates;                                                // 最多四个本地候选地址
    const utp_address_t* public_candidates;                                               // 最多四个公网 IP:port 候选
    utp_address_t        decoded_local_candidates[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];   // 解码存储
    utp_address_t        decoded_public_candidates[UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES];  // 解码存储
    uint16_t             local_port;                                                      // Context 已绑定 UDP 端口
    uint8_t              family;                                                          // 4 或 6
    uint8_t              local_candidate_count;                                           // 0..4
    uint8_t              public_candidate_count;                                          // 1..4
} utp_rendezvous_candidate_plan_t;

/* REDIRECT：rendezvous_id(16) 加目标 CandidatePlan。 */
typedef struct utp_rendezvous_redirect {
    uint8_t                         rendezvous_id[UTP_RENDEZVOUS_ID_SIZE];  // 对应 REQUEST 幂等键
    uint8_t                         punch_token[UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE];
    utp_rendezvous_candidate_plan_t target_plan;  // 目标 B 的候选计划
} utp_rendezvous_redirect_t;

/* FORWARD：rendezvous_id(16)、请求方 peer_id 与请求方 CandidatePlan。 */
typedef struct utp_rendezvous_forward {
    const uint8_t*                  source_peer_id;                         // 请求方 peer_id 的输入视图
    uint8_t                         rendezvous_id[UTP_RENDEZVOUS_ID_SIZE];  // 对应 REQUEST 幂等键
    uint8_t                         punch_token[UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE];
    uint8_t                         source_peer_id_length;  // 1..128
    utp_rendezvous_candidate_plan_t source_plan;            // 请求方 A 的候选计划
} utp_rendezvous_forward_t;

/** @brief 编码 REGISTER 消息体，输出不含 FrameRendezvous 包络。 */
utp_internal_error_t utp_rendezvous_register_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_rendezvous_register_t* registration, size_t* out_length);
/** @brief 解码严格的 REGISTER 消息体，peer_id 和候选地址均借用/存入输出结构。 */
utp_internal_error_t utp_rendezvous_register_decode(utp_rendezvous_register_t* registration, const uint8_t* buffer,
                                                    size_t length);
/** @brief 编码 REGISTERED 消息体，输出不含 FrameRendezvous 包络。 */
utp_internal_error_t utp_rendezvous_registered_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_rendezvous_registered_t* registered,
                                                      size_t*                            out_length);
/** @brief 解码严格的 REGISTERED 消息体，endpoint 存入输出结构。 */
utp_internal_error_t utp_rendezvous_registered_decode(utp_rendezvous_registered_t* registered, const uint8_t* buffer,
                                                      size_t length);
/** @brief 编码固定长度 PING 消息体。 */
utp_internal_error_t utp_rendezvous_ping_encode(uint8_t* buffer, size_t capacity, const utp_rendezvous_ping_t* ping);
/** @brief 解码固定长度 PING 消息体。 */
utp_internal_error_t utp_rendezvous_ping_decode(utp_rendezvous_ping_t* ping, const uint8_t* buffer, size_t length);
/** @brief 编码临时对称 NAT 校准指令。 */
utp_internal_error_t utp_rendezvous_calibrate_encode(uint8_t* buffer, size_t capacity,
                                                     const utp_rendezvous_calibrate_t* calibrate, size_t* out_length);
/** @brief 解码严格的临时对称 NAT 校准指令。 */
utp_internal_error_t utp_rendezvous_calibrate_decode(utp_rendezvous_calibrate_t* calibrate, const uint8_t* buffer,
                                                     size_t length);
/** @brief 编码固定长度 PONG 消息体。 */
utp_internal_error_t utp_rendezvous_pong_encode(uint8_t* buffer, size_t capacity, const utp_rendezvous_pong_t* pong);
/** @brief 解码固定长度 PONG 消息体。 */
utp_internal_error_t utp_rendezvous_pong_decode(utp_rendezvous_pong_t* pong, const uint8_t* buffer, size_t length);
/** @brief 编码 ADDRESS_UPDATE 消息体。 */
utp_internal_error_t utp_rendezvous_address_update_encode(uint8_t* buffer, size_t capacity,
                                                          const utp_rendezvous_address_update_t* update,
                                                          size_t*                                out_length);
/** @brief 解码严格的 ADDRESS_UPDATE 消息体，samples 指向输出结构内的解码存储。 */
utp_internal_error_t utp_rendezvous_address_update_decode(utp_rendezvous_address_update_t* update,
                                                          const uint8_t* buffer, size_t length);
/** @brief 编码固定长度 ADDRESS_UPDATED 消息体。 */
utp_internal_error_t utp_rendezvous_address_updated_encode(uint8_t* buffer, size_t capacity,
                                                           const utp_rendezvous_address_updated_t* updated);
/** @brief 解码固定长度 ADDRESS_UPDATED 消息体。 */
utp_internal_error_t utp_rendezvous_address_updated_decode(utp_rendezvous_address_updated_t* updated,
                                                           const uint8_t* buffer, size_t length);
/** @brief 编码 REQUEST 消息体，输出不含 FrameRendezvous 包络。 */
utp_internal_error_t utp_rendezvous_request_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_rendezvous_request_t* request, size_t* out_length);
/** @brief 解码 REQUEST 消息体，peer_id 和候选地址均借用输入缓冲。 */
utp_internal_error_t utp_rendezvous_request_decode(utp_rendezvous_request_t* request, const uint8_t* buffer,
                                                   size_t length);
/** @brief 编码 CandidatePlan，输出不含任意 FrameRendezvous 包络。 */
utp_internal_error_t utp_rendezvous_candidate_plan_encode(uint8_t* buffer, size_t capacity,
                                                          const utp_rendezvous_candidate_plan_t* plan,
                                                          size_t*                                out_length);
/** @brief 解码 CandidatePlan；local_candidates 指向输出结构内的解码存储。 */
utp_internal_error_t utp_rendezvous_candidate_plan_decode(utp_rendezvous_candidate_plan_t* plan, const uint8_t* buffer,
                                                          size_t length, size_t* consumed);
/** @brief 编码 REDIRECT 消息体。 */
utp_internal_error_t utp_rendezvous_redirect_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_rendezvous_redirect_t* redirect, size_t* out_length);
/** @brief 解码 REDIRECT 消息体。 */
utp_internal_error_t utp_rendezvous_redirect_decode(utp_rendezvous_redirect_t* redirect, const uint8_t* buffer,
                                                    size_t length);
/** @brief 编码 FORWARD 消息体。 */
utp_internal_error_t utp_rendezvous_forward_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_rendezvous_forward_t* forward, size_t* out_length);
/** @brief 解码 FORWARD 消息体；source_peer_id 借用输入缓冲。 */
utp_internal_error_t utp_rendezvous_forward_decode(utp_rendezvous_forward_t* forward, const uint8_t* buffer,
                                                   size_t length);
/** @brief 校验 INTRODUCTION 消息体必须恰为一个 rendezvous_id。 */
utp_internal_error_t utp_rendezvous_introduction_decode(uint8_t        rendezvous_id[UTP_RENDEZVOUS_ID_SIZE],
                                                        const uint8_t* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_RENDEZVOUS_H
