#ifndef EULAR_UTP_CONGESTION_CUBIC_H
#define EULAR_UTP_CONGESTION_CUBIC_H

#include <stdint.h>

#include "congestion/congestion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_CUBIC_DEFAULT_MSS UINT64_C(1460)

typedef struct utp_cubic_config {
    double   beta;
    double   cubic_c;
    uint32_t initial_cwnd_mss;
    uint32_t minimum_cwnd_mss;
} utp_cubic_config_t;

typedef struct utp_cubic {
    utp_congestion_t       congestion;
    const utp_rtt_stats_t* rtt_stats;
    uint64_t               cwnd;
    uint64_t               ssthresh;
    uint64_t               acked_bytes;
    uint64_t               epoch_start_us;
    uint64_t               last_max_cwnd;
    uint64_t               origin_point_cwnd;
    uint64_t               initial_cwnd;
    uint64_t               minimum_cwnd;
    double                 k;
    double                 beta;
    double                 cubic_c;
} utp_cubic_t;

void                    utp_cubic_init(utp_cubic_t* cubic, const utp_cubic_config_t* config);
utp_congestion_t*       utp_cubic_as_congestion(utp_cubic_t* cubic);
const utp_congestion_t* utp_cubic_as_const_congestion(const utp_cubic_t* cubic);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_CUBIC_H
