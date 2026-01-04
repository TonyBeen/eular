/*************************************************************************
    > File Name: condition.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 11时44分31秒
 ************************************************************************/

#include "utils/condition.h"
#include "utils/sysdef.h"

#include <time.h>
#include <pthread.h>

#if defined(OS_LINUX) || defined(OS_APPLE)
#include <sys/time.h>
#endif

#include "src/mutex.hpp"

namespace eular {
struct Condition::ConditionImpl
{
    pthread_cond_t cond;
};

Condition::Condition()
{
    m_impl = std::unique_ptr<ConditionImpl>(new ConditionImpl);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if defined(OS_LINUX)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&m_impl->cond, &attr);
    pthread_condattr_destroy(&attr);
}

Condition::~Condition()
{
    pthread_cond_destroy(&m_impl->cond);
}

int Condition::wait(Mutex& mutex)
{
    // 先解锁，等待条件，加锁
    return pthread_cond_wait(&m_impl->cond, &mutex.m_impl->_mutex);
}

int Condition::timedWait(Mutex& mutex, uint64_t ms)
{
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
#else
    struct timespec relative = {static_cast<long>(ms / 1000), static_cast<long>(ms * 1000000)};
    pthread_win32_getabstime_np(&ts, &relative);
#endif
    return pthread_cond_timedwait(&m_impl->cond, &mutex.m_impl->_mutex, &ts);
}

void Condition::signal()
{
    pthread_cond_signal(&m_impl->cond);
}

void Condition::broadcast()
{
    pthread_cond_broadcast(&m_impl->cond);
}

} // namespace eular
