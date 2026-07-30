#include "util/time.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

static uint64_t utp_system_monotonic_now_us(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t      ticks;
    uint64_t      ticks_per_second;

    if (QueryPerformanceCounter(&counter) == 0 || QueryPerformanceFrequency(&frequency) == 0 || counter.QuadPart < 0 ||
        frequency.QuadPart <= 0) {
        return 0u;
    }
    ticks            = (uint64_t)counter.QuadPart;
    ticks_per_second = (uint64_t)frequency.QuadPart;
    return (ticks / ticks_per_second) * UINT64_C(1000000) +
           (ticks % ticks_per_second) * UINT64_C(1000000) / ticks_per_second;
#elif defined(__APPLE__) || defined(__unix__)
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / UINT64_C(1000);
#else
#error "libutp C requires a monotonic clock implementation for this platform"
#endif
}

uint64_t utp_clock_now_us(const utp_clock_t *clock) {
    if (clock != NULL && clock->now_us != NULL) {
        return clock->now_us(clock->user_data);
    }
    return utp_system_monotonic_now_us();
}
