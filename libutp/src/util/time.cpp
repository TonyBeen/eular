/*************************************************************************
    > File Name: time.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 14 Jan 2026 03:31:32 PM CST
 ************************************************************************/

#include "util/time.h"

namespace eular {
namespace utp {
namespace time {
#if defined(OS_WINDOWS)
#if _WIN32_WINNT < _WIN32_WINNT_WIN8
#define WindwsGetSystemTime GetSystemTimeAsFileTime
#else
#define WindwsGetSystemTime GetSystemTimePreciseAsFileTime
#endif

int64_t _QueryPerfCounter() // stl
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li); // always succeeds
    return li.QuadPart;
}

int64_t _QueryPerfFrequency()
{
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li); // always succeeds
    return li.QuadPart;
}

static std::atomic<int64_t> g_frequency{0};

#endif

uint64_t MonotonicMs()
{
#if defined(OS_WINDOWS)
    int64_t frequency = g_frequency.load(std::memory_order_acquire);
    if (frequency == 0) {
        frequency = _QueryPerfFrequency();
        g_frequency.store(frequency, std::memory_order_release);
    }
    int64_t counter = _QueryPerfCounter();
    return static_cast<uint64_t>(counter * 1000 / frequency);
#elif defined(OS_LINUX) || defined(OS_APPLE)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#endif
}

uint64_t MonotonicUs()
{
#if defined(OS_WINDOWS)
    int64_t frequency = g_frequency.load(std::memory_order_acquire);
    if (frequency == 0) {
        frequency = _QueryPerfFrequency();
        g_frequency.store(frequency, std::memory_order_release);
    }
    int64_t counter = _QueryPerfCounter();
    return static_cast<uint64_t>(counter * 1000000 / frequency);
#elif defined(OS_LINUX) || defined(OS_APPLE)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
#endif
}

uint64_t RealtimeMs()
{
#if defined(OS_WINDOWS)
    // FILETIME 是从 1601-01-01 开始的 100 纳秒间隔
    // Unix 时间戳是从 1970-01-01 开始的秒数
    // 两者之间的偏移量是 116444736000000000 个 100 纳秒间隔
    static const uint64_t FILETIME_TO_UNIX_OFFSET = 116444736000000000ULL;

    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t filetime = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // 转换为 Unix 时间戳 (100ns -> ms)
    return (filetime - FILETIME_TO_UNIX_OFFSET) / 10000;
#elif defined(OS_LINUX) || defined(OS_APPLE)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#endif
}

int32_t _Timezone()
{
    time_t now;
    ::time(&now);

    struct tm local_tm = *::localtime(&now);
    struct tm utc_tm = *::gmtime(&now);

    double diff = ::difftime(mktime(&local_tm), mktime(&utc_tm)) / 3600;
    return (int32_t)diff;
}

int32_t Timezone()
{
    static int32_t g_timezone = INT32_MIN;
    if (g_timezone == INT32_MIN) {
        g_timezone = _Timezone();
    }

    return g_timezone;
}
} // namespace time
} // namespace utp
} // namespace eular
