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
#endif

uint64_t MonotonicMs()
{
#if defined(OS_WINDOWS)
#elif defined(OS_LINUX) || defined(OS_APPLE)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#endif
}

uint64_t MonotonicUs()
{
#if defined(OS_WINDOWS)
#elif defined(OS_LINUX) || defined(OS_APPLE)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
#endif
}

uint64_t RealtimeMs()
{
#if defined(OS_WINDOWS)
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
