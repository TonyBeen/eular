/*************************************************************************
    > File Name: condition.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 11时44分31秒
 ************************************************************************/

#include "utils/condition.h"
#include "utils/sysdef.h"

#include <time.h>
#if !defined(OS_WINDOWS)
#include <pthread.h>
#endif

#if defined(OS_LINUX) || defined(OS_APPLE)
#include <sys/time.h>
#endif

#include "src/mutex.hpp"

namespace eular {
struct Condition::ConditionImpl
{
#if defined(OS_WINDOWS)
    CONDITION_VARIABLE cond;
#else
    pthread_cond_t cond;
#endif
};

Condition::Condition()
{
    m_impl = std::unique_ptr<ConditionImpl>(new ConditionImpl);
#if defined(OS_WINDOWS)
    InitializeConditionVariable(&m_impl->cond);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if defined(OS_LINUX)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&m_impl->cond, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

Condition::~Condition()
{
#if !defined(OS_WINDOWS)
    pthread_cond_destroy(&m_impl->cond);
#endif
}

int Condition::wait(Mutex& mutex)
{
    // 先解锁，等待条件，加锁
#if defined(OS_WINDOWS)
    return SleepConditionVariableCS(&m_impl->cond, &mutex.m_impl->_mutex, INFINITE) ? 0 : GetLastError();
#else
    return pthread_cond_wait(&m_impl->cond, &mutex.m_impl->_mutex);
#endif
}

int Condition::timedWait(Mutex& mutex, uint64_t ms)
{
    if (ms > static_cast<uint64_t>(UINT32_MAX)) {
        ms = UINT32_MAX;
    }
#if defined(OS_WINDOWS)
    return SleepConditionVariableCS(&m_impl->cond,
                                    &mutex.m_impl->_mutex,
                                    static_cast<DWORD>(ms)) ? 0 : GetLastError();
#else
    struct timespec ts;
#if defined(OS_LINUX)
    clock_gettime(CLOCK_MONOTONIC, &ts);

    int64_t reltime_sec = ms / 1000;
    ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000;
    if (reltime_sec < INT64_MAX && ts.tv_nsec >= 1000000000) {
        ts.tv_nsec -= 1000000000;
        ++reltime_sec;
    }

    int64_t time_sec = ts.tv_sec;
    if (time_sec > INT64_MAX - reltime_sec) {
        time_sec = INT64_MAX;
    } else {
        time_sec += reltime_sec;
    }

    ts.tv_sec = static_cast<long>(time_sec);
#elif defined(OS_APPLE)
    struct timeval tv;
    gettimeofday(&tv, NULL);

    ts.tv_sec = tv.tv_sec + (ms / 1000);
    ts.tv_nsec = tv.tv_usec * 1000 + (ms % 1000) * 1000000;

    // 处理纳秒溢出
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
#endif
    return pthread_cond_timedwait(&m_impl->cond, &mutex.m_impl->_mutex, &ts);
#endif
}

void Condition::signal()
{
#if defined(OS_WINDOWS)
    WakeConditionVariable(&m_impl->cond);
#else
    pthread_cond_signal(&m_impl->cond);
#endif
}

void Condition::broadcast()
{
#if defined(OS_WINDOWS)
    WakeAllConditionVariable(&m_impl->cond);
#else
    pthread_cond_broadcast(&m_impl->cond);
#endif
}

} // namespace eular
