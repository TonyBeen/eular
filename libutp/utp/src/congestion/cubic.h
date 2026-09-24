#ifndef EULAR_UTP_CONGESTION_CUBIC_H
#define EULAR_UTP_CONGESTION_CUBIC_H

#include <stdint.h>

#include "congestion/congestion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_CUBIC_DEFAULT_MSS UINT64_C(1460)

typedef struct utp_cubic_config {
    double   beta;              // 丢失后的窗口回退系数
    double   cubic_c;           // CUBIC 曲线常数
    uint32_t initial_cwnd_mss;  // 初始拥塞窗口，单位 MSS
    uint32_t minimum_cwnd_mss;  // 最小拥塞窗口，单位 MSS
} utp_cubic_config_t;

typedef struct utp_cubic {
    utp_congestion_t       congestion;         // 通用拥塞控制接口
    const utp_rtt_stats_t* rtt_stats;          // 连接 RTT 统计，不拥有
    uint64_t               cwnd;               // 当前拥塞窗口，单位 bytes
    uint64_t               ssthresh;           // 慢启动阈值，单位 bytes
    uint64_t               acked_bytes;        // 本 ACK 批次新确认字节
    uint64_t               epoch_start_us;     // 当前 CUBIC epoch 开始时刻
    uint64_t               last_max_cwnd;      // 最近丢失前的最大窗口
    uint64_t               origin_point_cwnd;  // 当前 CUBIC 曲线原点
    uint64_t               initial_cwnd;       // 初始窗口，单位 bytes
    uint64_t               minimum_cwnd;       // 最小窗口，单位 bytes
    double                 k;                  // CUBIC 曲线拐点时间
    double                 beta;               // 配置的回退系数
    double                 cubic_c;            // 配置的曲线常数
} utp_cubic_t;

void                    utp_cubic_init(utp_cubic_t* cubic, const utp_cubic_config_t* config);
utp_congestion_t*       utp_cubic_as_congestion(utp_cubic_t* cubic);
const utp_congestion_t* utp_cubic_as_const_congestion(const utp_cubic_t* cubic);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_CUBIC_H
