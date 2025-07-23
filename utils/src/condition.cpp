/*************************************************************************
    > File Name: condition.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年05月30日 星期日 11时44分31秒
 ************************************************************************/

#include "utils/condition.h"

#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

namespace eular {
Condition::Condition() : Condition(PRIVATE)
{

}
Condition::Condition(int type)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);  // 绝对时间
    if (SHARED == type) {
        pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    }

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

int Condition::timedWait(Mutex& mutex, uint64_t ns)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    int64_t reltime_sec = ns / 1000000000;
    ts.tv_nsec += static_cast<long>(ns % 1000000000);
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
