/*************************************************************************
    > File Name: mutex.cpp
    > Author: hsz
    > Desc: 
    > Created Time: 2021年04月25日 星期日 21时25分03秒
 ************************************************************************/

#include "utils/mutex.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <sys/stat.h>
#include <fcntl.h>

#include "utils/utils.h"
#include "utils/exception.h"

namespace eular {
// @see https://codebrowser.dev/glibc/glibc/nptl/pthread_spin_lock.c.html
void SpinLock::LockSlow() noexcept
{
    // do {
    //     while (m_locked.load(std::memory_order_relaxed)) {
    //         CPU_RELAX_NOP();
    //     }
    // } while (m_locked.exchange(true, std::memory_order_acquire));
}

Mutex::Mutex()
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);       // for pthread_mutex_lock will return EOWNERDEAD
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);  // for EDEADLK
    pthread_mutex_init(&mMutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

Mutex::~Mutex()
{
    int32_t ret = 0;
    do {
        if (ret == EBUSY) {
            unlock();
        }
        ret = pthread_mutex_destroy(&mMutex);
    } while (ret == EBUSY);
    assert(ret == 0);
}

void Mutex::setMutexName(const String8 &name)
{
    mName = name;
}

int32_t Mutex::lock()
{
    int32_t ret = 0;
    do {
        ret = pthread_mutex_lock(&mMutex);
        if (ret == EDEADLK) { // already locked
            throw Exception(String8::Format("deadlock detected: tid = %d mutex: %s", (int32_t)gettid(), mName.c_str()));
        } else if (ret == EOWNERDEAD) { // other threads exited abnormally without unlocking the mutex
            pthread_mutex_consistent(&mMutex); // will lock the mutex
            ret = 0;
        }
    } while (0);

    return ret;
}

void Mutex::unlock()
{
    int32_t ret = pthread_mutex_unlock(&mMutex);
    if (ret != 0 && ret != EPERM) { // EPERM: the calling thread does not own the mutex
        throw Exception(String8::Format("pthread_mutex_unlock error. return %d", ret));
    }
}

int32_t Mutex::trylock()
{
    return pthread_mutex_trylock(&mMutex);
}

RecursiveMutex::RecursiveMutex()
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);       // for pthread_mutex_lock will return EOWNERDEAD
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);   // for recursive
    pthread_mutex_init(&mMutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

RecursiveMutex::~RecursiveMutex()
{
    int32_t ret = 0;
    do {
        if (ret == EBUSY) {
            unlock();
        }
        ret = pthread_mutex_destroy(&mMutex);
    } while (ret == EBUSY);
    assert(ret == 0);
}

int32_t RecursiveMutex::lock()
{
    int32_t ret = 0;
    do {
        ret = pthread_mutex_lock(&mMutex);
        if (ret == EOWNERDEAD) { // other threads exited abnormally without unlocking the mutex
            pthread_mutex_consistent(&mMutex); // will lock the mutex
            ret = 0;
        }
    } while (0);

    return ret;
}

void RecursiveMutex::unlock()
{
    int32_t ret = pthread_mutex_unlock(&mMutex);
    if (ret != 0 && ret != EPERM) { // EPERM: the calling thread does not own the mutex
        throw Exception(String8::Format("pthread_mutex_unlock error. return %d", ret));
    }
}

int32_t RecursiveMutex::trylock()
{
    return pthread_mutex_trylock(&mMutex);
}

void RecursiveMutex::setMutexName(const String8 &name)
{
    mName = name;
}

RWMutex::RWMutex()
{
    pthread_rwlock_init(&mRWMutex, nullptr);
}

RWMutex::~RWMutex()
{
    pthread_rwlock_destroy(&mRWMutex);
}

void RWMutex::rlock()
{
    int32_t ret = pthread_rwlock_rdlock(&mRWMutex);
    if (ret != 0 && ret != EAGAIN) {
        throw Exception("pthread_rwlock_rdlock error");
    } else {
#ifdef DEBUG
        mReadLocked.store(true);
#endif
    }
}

void RWMutex::wlock()
{
    if (pthread_rwlock_wrlock(&mRWMutex) != 0) {
        throw Exception("pthread_rwlock_wrlock error");
    } else {
#ifdef DEBUG
        mWritLocked.store(true);
#endif
    }
}

void RWMutex::unlock()
{
    if (pthread_rwlock_unlock(&mRWMutex) != 0) {
        throw Exception("pthread_rwlock_unlock error");
    } else {
#ifdef DEBUG
        if (mReadLocked) {
            mReadLocked.store(false);
        }
        if (mWritLocked) {
            mWritLocked.store(false);
        }
#endif
    }
}

Sem::Sem(const char *semPath, uint8_t val)
{
#if defined(OS_LINUX) || defined(OS_MACOS)
    if (semPath == nullptr) {
        throw Exception("the first param can not be null");
    }

    // 如果信号量已存在，则后两个参数会忽略，详见man sem_open
    mSem = sem_open(semPath, O_CREAT | O_RDWR, 0664, val);
    if (mSem == SEM_FAILED) {
        String8 erorMsg = String8::Format("sem_open failed. %d %s", errno, strerror(errno));
        throw Exception(erorMsg);
    }

    isNamedSemaphore = true;
#elif defined(OS_WINDOWS)
    #error "Named semaphore is not supported on Windows"
#endif
}

Sem::Sem(uint8_t valBase)
{
    mSem = new (std::nothrow)sem_t;
    if (mSem == nullptr) {
        throw Exception("new sem_t error. no more memory");
    }

    if (sem_init(mSem, false, valBase)) {
        throw Exception(String8::Format("%s() sem_init error %d, %s", __func__, errno, strerror(errno)));
    }

    isNamedSemaphore = false;
}

Sem::~Sem()
{
    if (isNamedSemaphore) {
        sem_close(mSem);
        sem_unlink(mFilePath.c_str());
    } else {
        sem_destroy(mSem);
        delete mSem;
    }
}

// see https://stackoverflow.com/questions/2013181/gdb-causes-sem-wait-to-fail-with-eintr-error

bool Sem::post()
{
    int32_t rt = 0;
    do {
        rt = sem_post(mSem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
}

bool Sem::wait()
{
    int32_t rt = 0;
    do {
        rt = sem_wait(mSem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
}

bool Sem::trywait()
{
    int32_t rt = 0;
    do {
        rt = sem_trywait(mSem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
}

#if _POSIX_C_SOURCE >= 200112L
bool Sem::timedwait(uint32_t ms)
{
    struct timespec expire;
#if defined(OS_LINUX) || defined(OS_MACOS)
    clock_gettime(CLOCK_REALTIME, &expire);
    expire.tv_sec += ms / 1000;
    expire.tv_nsec += (ms % 1000 * 1000 * 1000);
    if (expire.tv_nsec > 1000 * 1000 * 1000) {
        ++expire.tv_sec;
        expire.tv_nsec -= 1000 * 1000 * 1000;
    }
#elif defined(OS_WINDOWS)
    struct timespec reltive;
    reltive.tv_sec = ms / 1000;
    reltive.tv_nsec = (ms % 1000) * 1000000; // convert to nanoseconds
    pthread_win32_getabstime_np(&expire, &reltive)
#endif
    int32_t rt = 0;
    do {
        rt = sem_timedwait(mSem, &expire);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
}
#else
bool Sem::timedwait(uint32_t ms)
{
    return false;
}
#endif

namespace detail {
__thread void* __once_callable;
__thread void (*__once_call)();

extern "C" void __once_proxy(void)
{
    detail::__once_call();
}
} // namespace detail

}