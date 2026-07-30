#ifndef EULAR_UTP_CONGESTION_MINMAX_H
#define EULAR_UTP_CONGESTION_MINMAX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_MINMAX_SAMPLE_COUNT 3u

typedef struct utp_minmax_sample {
    uint64_t time;
    uint64_t value;
} utp_minmax_sample_t;

typedef struct utp_minmax {
    uint64_t            window;
    utp_minmax_sample_t samples[UTP_MINMAX_SAMPLE_COUNT];
} utp_minmax_t;

void     utp_minmax_init(utp_minmax_t *minmax, uint64_t window);
uint64_t utp_minmax_get(const utp_minmax_t *minmax);
uint64_t utp_minmax_get_at(const utp_minmax_t *minmax, uint32_t index);
void     utp_minmax_update_min(utp_minmax_t *minmax, uint64_t now, uint64_t measurement);
void     utp_minmax_update_max(utp_minmax_t *minmax, uint64_t now, uint64_t measurement);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_CONGESTION_MINMAX_H
