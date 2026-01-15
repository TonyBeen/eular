/*************************************************************************
    > File Name: time.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 14 Jan 2026 03:31:32 PM CST
 ************************************************************************/

#include "util/time.h"

#include <utils/mutex.h>

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
#elif defined(OS_APPLE)
static mach_timebase_info_data_t g_timebase;
static eular::once_flag g_timebaseFlag;
void LoadTimebase()
{
    mach_timebase_info(&g_timebase);
}

#endif

uint64_t MonotonicMs() noexcept
{
#if defined(OS_WINDOWS)
    int64_t frequency = g_frequency.load(std::memory_order_acquire);
    if (frequency == 0) {
        frequency = _QueryPerfFrequency();
        g_frequency.store(frequency, std::memory_order_release);
    }
    int64_t counter = _QueryPerfCounter();
    return static_cast<uint64_t>(counter * 1000 / frequency);
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#elif defined(OS_APPLE)
    eular::call_once(g_timebaseFlag, LoadTimebase);
    uint64_t t = mach_absolute_time();
    t *= g_timebase.numer;
    t /= g_timebase.denom;
    t /= 1000000;
    return t;
#endif
}

uint64_t MonotonicUs() noexcept
{
#if defined(OS_WINDOWS)
    int64_t frequency = g_frequency.load(std::memory_order_acquire);
    if (frequency == 0) {
        frequency = _QueryPerfFrequency();
        g_frequency.store(frequency, std::memory_order_release);
    }
    int64_t counter = _QueryPerfCounter();
    return static_cast<uint64_t>(counter * 1000000 / frequency);
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
#elif defined(OS_APPLE)
    eular::call_once(g_timebaseFlag, LoadTimebase);
    uint64_t t = mach_absolute_time();
    t *= g_timebase.numer;
    t /= g_timebase.denom;
    t /= 1000;
    return t;
#endif
}

uint64_t RealtimeMs() noexcept
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
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#else
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return static_cast<uint64_t>(tv.tv_sec) * 1000 + static_cast<uint64_t>(tv.tv_usec) / 1000;
#endif
}

int32_t _Timezone() noexcept
{
    time_t now;
    ::time(&now); // UTC 秒数

    struct tm local_tm;
#if defined(OS_WINDOWS)
    ::localtime_s(&local_tm, &now);
#elif defined(OS_LINUX) || defined(OS_APPLE)
    ::localtime_r(&now, &local_tm);
#endif

    local_tm.tm_isdst = 0; // 不考虑夏令时
    // return static_cast<int32_t>(local_tm.tm_gmtoff / 3600); // 受夏令时影响
    double diff = ::difftime(mktime(&local_tm), now) / 3600;
    return (int32_t)diff;
}

int32_t Timezone() noexcept
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
