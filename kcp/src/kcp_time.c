#include "kcp_time.h"
#include <time.h>
#ifdef USE_C11_ATOMICS
#include <stdatomic.h> // c11
#endif
#include "kcp_inc.h"

#if defined(OS_WINDOWS)

#if _WIN32_WINNT < _WIN32_WINNT_WIN8
#define WindwsGetSystemTime GetSystemTimeAsFileTime
#else
#define WindwsGetSystemTime GetSystemTimePreciseAsFileTime
#endif

int64_t _Query_perf_counter() // stl
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li); // always succeeds
    return li.QuadPart;
}

int64_t _Query_perf_frequency()
{
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li); // always succeeds
    return li.QuadPart;
}

#endif

uint64_t kcp_time_monotonic_ms()
{
#if defined(OS_WINDOWS)

#ifdef USE_C11_ATOMICS
    static atomic_int_fast64_t g_frequency = ATOMIC_VAR_INIT(0);
    if (0 == atomic_load(&g_frequency)) {
        atomic_store(&g_frequency, _Query_perf_frequency()); // doesn't change after system boot
    }
    int64_t frequency = atomic_load(&g_frequency);
#else
    static int64_t frequency = 0;
    if (0 == frequency) {
        frequency = _Query_perf_frequency(); // doesn't change after system boot
    }
#endif

    int64_t now = _Query_perf_counter();
    return (uint64_t)(now * 1000 / frequency);
#else // Linux Mac
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

uint64_t kcp_time_monotonic_us()
{
#if defined(OS_WINDOWS)

#ifdef USE_C11_ATOMICS
    static atomic_int_fast64_t g_frequency = ATOMIC_VAR_INIT(0);
    if (0 == atomic_load(&g_frequency)) {
        atomic_store(&g_frequency, _Query_perf_frequency()); // doesn't change after system boot
    }
    int64_t frequency = atomic_load(&g_frequency);
#else
    static int64_t frequency = 0;
    if (0 == frequency) {
        frequency = _Query_perf_frequency(); // doesn't change after system boot
    }
#endif

    int64_t now = _Query_perf_counter();
    return (uint64_t)(now * 1000 * 1000 / frequency);
#else // Linux Mac
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
#endif
}

uint64_t kcp_time_realtime_ms()
{
#if defined(OS_WINDOWS)
    // FILETIME 是从 1601-01-01 开始的 100 纳秒间隔
    // Unix 时间戳是从 1970-01-01 开始的秒数
    // 两者之间的偏移量是 116444736000000000 个 100 纳秒间隔
    static const uint64_t FILETIME_TO_UNIX_OFFSET = 116444736000000000ULL;

    FILETIME ft;
    WindwsGetSystemTime(&ft);
    uint64_t filetime = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // 转换为 Unix 时间戳 (100ns -> ms)
    return (filetime - FILETIME_TO_UNIX_OFFSET) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

int32_t _kcp_time_zone()
{
    time_t now;
    time(&now);

    struct tm local_tm = *localtime(&now);
    struct tm utc_tm = *gmtime(&now);

    double diff = difftime(mktime(&local_tm), mktime(&utc_tm)) / 3600;
    return (int32_t)diff;
}

int32_t kcp_time_zone()
{
    static int32_t g_timezone = INT32_MIN;
    if (g_timezone == INT32_MIN) {
        g_timezone = _kcp_time_zone();
    }

    return g_timezone;
}