/*************************************************************************
    > File Name: condition.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 11时44分31秒
 ************************************************************************/

#include "utils/condition.h"
#include "utils/sysdef.h"

#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

namespace eular {
Condition::Condition()
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if defined(OS_LINUX) || defined(OS_MACOS)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&mCond, &attr);
    pthread_condattr_destroy(&attr);
}

Condition::~Condition()
{
    pthread_cond_destroy(&mCond);
}

int Condition::wait(Mutex& mutex)
{
    // 先解锁，等待条件，加锁
    return pthread_cond_wait(&mCond, &mutex.mMutex);
}

int Condition::timedWait(Mutex& mutex, uint64_t ms)
{
    struct timespec ts;
#if defined(OS_LINUX) || defined(OS_MACOS)
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
#else
    struct timespec relative = {static_cast<long>(ms / 1000), static_cast<long>(ms * 1000000)};
    pthread_win32_getabstime_np(&ts, &reltime);
#endif
    return pthread_cond_timedwait(&mCond, &mutex.mMutex, &ts);
}

void Condition::signal()
{
    pthread_cond_signal(&mCond);
}

void Condition::broadcast()
{
    pthread_cond_broadcast(&mCond);
}

} // namespace eular
