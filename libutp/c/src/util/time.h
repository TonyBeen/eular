#ifndef EULAR_UTP_INTERNAL_TIME_H
#define EULAR_UTP_INTERNAL_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*utp_clock_now_us_fn)(void *user_data);

typedef struct utp_clock {
    utp_clock_now_us_fn now_us;
    void               *user_data;
} utp_clock_t;

uint64_t utp_clock_now_us(const utp_clock_t *clock);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_TIME_H
