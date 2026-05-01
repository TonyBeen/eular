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

#if !defined(OS_WINDOWS)
#include <pthread.h>
#endif

#if defined(OS_APPLE)
#include <os/lock.h>
#endif

#include "utils/errors.h"
#include "utils/utils.h"
#include "utils/exception.h"
#include "src/mutex.hpp"

namespace eular {

// @see https://codebrowser.dev/glibc/glibc/nptl/pthread_spin_lock.c.html

struct SpinLock::SpinLockImpl {
#if defined(OS_WINDOWS)
    std::atomic_flag _spinlock = ATOMIC_FLAG_INIT;
#elif defined(OS_LINUX)
    pthread_spinlock_t _spinlock{};
#else
    os_unfair_lock _spinlock = OS_UNFAIR_LOCK_INIT;
#endif

    SpinLockImpl() {
#if defined(OS_LINUX)
        pthread_spin_init(&_spinlock, PTHREAD_PROCESS_PRIVATE);
#endif
    }

    ~SpinLockImpl() {
#if defined(OS_LINUX)
        pthread_spin_destroy(&_spinlock);
#endif
    }
};

SpinLock::SpinLock()
{
    m_impl = std::unique_ptr<SpinLockImpl>(new SpinLockImpl);
}

bool SpinLock::trylock() noexcept
{
#if defined(OS_WINDOWS)
    return !m_impl->_spinlock.test_and_set(std::memory_order_acquire);
#elif defined(OS_LINUX)
    return pthread_spin_trylock(&m_impl->_spinlock) == 0;
#else
    return os_unfair_lock_trylock(&m_impl->_spinlock);
#endif
}

void SpinLock::unlock() noexcept
{
#if defined(OS_WINDOWS)
    m_impl->_spinlock.clear(std::memory_order_release);
#elif defined(OS_LINUX)
    pthread_spin_unlock(&m_impl->_spinlock);
#else
    os_unfair_lock_unlock(&m_impl->_spinlock);
#endif
}

void SpinLock::lock() noexcept
{
#if defined(OS_WINDOWS)
    while (m_impl->_spinlock.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
#elif defined(OS_LINUX)
    pthread_spin_lock(&m_impl->_spinlock); // No need to check the return value
#else
    os_unfair_lock_lock(&m_impl->_spinlock);
#endif
}

Mutex::Mutex()
{
    m_impl = std::unique_ptr<MutexImpl>(new MutexImpl);
#if defined(OS_WINDOWS)
    InitializeCriticalSection(&m_impl->_mutex);
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
#if defined(OS_LINUX) || defined(OS_WINDOWS)
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);       // for pthread_mutex_lock will return EOWNERDEAD
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);  // for EDEADLK
#elif defined(OS_APPLE)
    // macOS does not support robust mutexes
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);     // for EDEADLK
#endif
    pthread_mutex_init(&m_impl->_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

Mutex::~Mutex()
{
#if defined(OS_WINDOWS)
    DeleteCriticalSection(&m_impl->_mutex);
#else
    int32_t ret = 0;
    do {
        if (ret == EBUSY) {
            unlock();
        }
        ret = pthread_mutex_destroy(&m_impl->_mutex);
    } while (ret == EBUSY);
    assert(ret == 0);
#endif
}

void Mutex::setMutexName(const String8 &name)
{
    m_name = name;
}

int32_t Mutex::lock()
{
#if defined(OS_WINDOWS)
    EnterCriticalSection(&m_impl->_mutex);
    return 0;
#else
    int32_t ret = 0;
    do {
        ret = pthread_mutex_lock(&m_impl->_mutex);
        if (ret == EDEADLK) { // already locked
            throw Exception(String8::Format("deadlock detected: tid = %d mutex: %s", (int32_t)gettid(), m_name.c_str()));
        } else if (ret == EOWNERDEAD) { // other threads exited abnormally without unlocking the mutex
#if defined(OS_LINUX) || defined(OS_WINDOWS)
            pthread_mutex_consistent(&m_impl->_mutex); // will lock the mutex
            ret = 0;
#endif
        }
    } while (0);

    return ret;
#endif
}

void Mutex::unlock()
{
#if defined(OS_WINDOWS)
    LeaveCriticalSection(&m_impl->_mutex);
#else
    int32_t ret = pthread_mutex_unlock(&m_impl->_mutex);
    if (ret != 0 && ret != EPERM) { // EPERM: the calling thread does not own the mutex
        throw Exception(String8::Format("pthread_mutex_unlock error. return %d", ret));
    }
#endif
}

int32_t Mutex::trylock()
{
#if defined(OS_WINDOWS)
    return TryEnterCriticalSection(&m_impl->_mutex) ? 0 : EBUSY;
#else
    return pthread_mutex_trylock(&m_impl->_mutex);
#endif
}

RecursiveMutex::RecursiveMutex()
{
    mImpl = std::unique_ptr<MutexImpl>(new MutexImpl);
#if defined(OS_WINDOWS)
    InitializeCriticalSection(&mImpl->_mutex);
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
#if defined(OS_LINUX) || defined(OS_WINDOWS)
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);       // for pthread_mutex_lock will return EOWNERDEAD
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);   // for recursive
#elif defined(OS_APPLE)
    // macOS does not support robust mutexes
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);      // for recursive
#endif
    pthread_mutex_init(&mImpl->_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

RecursiveMutex::~RecursiveMutex()
{
#if defined(OS_WINDOWS)
    DeleteCriticalSection(&mImpl->_mutex);
#else
    int32_t ret = 0;
    do {
        if (ret == EBUSY) {
            unlock();
        }
        ret = pthread_mutex_destroy(&mImpl->_mutex);
    } while (ret == EBUSY);
    assert(ret == 0);
#endif
}

int32_t RecursiveMutex::lock()
{
#if defined(OS_WINDOWS)
    EnterCriticalSection(&mImpl->_mutex);
    return 0;
#else
    int32_t ret = 0;
    do {
        ret = pthread_mutex_lock(&mImpl->_mutex);
#if defined(OS_LINUX) || defined(OS_WINDOWS)
        if (ret == EOWNERDEAD) { // other threads exited abnormally without unlocking the mutex
            pthread_mutex_consistent(&mImpl->_mutex); // will lock the mutex
            ret = 0;
        }
#endif
    } while (0);

    return ret;
#endif
}

void RecursiveMutex::unlock()
{
#if defined(OS_WINDOWS)
    LeaveCriticalSection(&mImpl->_mutex);
#else
    int32_t ret = pthread_mutex_unlock(&mImpl->_mutex);
    if (ret != 0 && ret != EPERM) { // EPERM: the calling thread does not own the mutex
        throw Exception(String8::Format("pthread_mutex_unlock error. return %d", ret));
    }
#endif
}

int32_t RecursiveMutex::trylock()
{
#if defined(OS_WINDOWS)
    return TryEnterCriticalSection(&mImpl->_mutex) ? 0 : EBUSY;
#else
    return pthread_mutex_trylock(&mImpl->_mutex);
#endif
}

void RecursiveMutex::setMutexName(const String8 &name)
{
    mName = name;
}

struct RWMutex::RWMutexImpl {
#if defined(OS_WINDOWS)
    CRITICAL_SECTION _lock;
    CONDITION_VARIABLE _cv;
    uint32_t _readers = 0;
    bool _writer = false;
#else
    pthread_rwlock_t _rwlock{};
#endif
};

RWMutex::RWMutex()
{
    mImpl = std::unique_ptr<RWMutexImpl>(new RWMutexImpl);
#if defined(OS_WINDOWS)
    InitializeCriticalSection(&mImpl->_lock);
    InitializeConditionVariable(&mImpl->_cv);
#else
    pthread_rwlock_init(&mImpl->_rwlock, nullptr);
#endif
}

RWMutex::~RWMutex()
{
#if defined(OS_WINDOWS)
    DeleteCriticalSection(&mImpl->_lock);
#else
    pthread_rwlock_destroy(&mImpl->_rwlock);
#endif
}

void RWMutex::rlock()
{
#if defined(OS_WINDOWS)
    EnterCriticalSection(&mImpl->_lock);
    while (mImpl->_writer) {
        SleepConditionVariableCS(&mImpl->_cv, &mImpl->_lock, INFINITE);
    }
    ++mImpl->_readers;
    LeaveCriticalSection(&mImpl->_lock);
#ifdef DEBUG
    mReadLocked.store(true);
#endif
#else
    int32_t ret = pthread_rwlock_rdlock(&mImpl->_rwlock);
    if (ret != 0 && ret != EAGAIN) {
        throw Exception("pthread_rwlock_rdlock error");
    } else {
#ifdef DEBUG
        mReadLocked.store(true);
#endif
    }
#endif
}

void RWMutex::wlock()
{
#if defined(OS_WINDOWS)
    EnterCriticalSection(&mImpl->_lock);
    while (mImpl->_writer || mImpl->_readers > 0) {
        SleepConditionVariableCS(&mImpl->_cv, &mImpl->_lock, INFINITE);
    }
    mImpl->_writer = true;
    LeaveCriticalSection(&mImpl->_lock);
#ifdef DEBUG
    mWritLocked.store(true);
#endif
#else
    if (pthread_rwlock_wrlock(&mImpl->_rwlock) != 0) {
        throw Exception("pthread_rwlock_wrlock error");
    } else {
#ifdef DEBUG
        mWritLocked.store(true);
#endif
    }
#endif
}

void RWMutex::unlock()
{
#if defined(OS_WINDOWS)
    EnterCriticalSection(&mImpl->_lock);
#ifdef DEBUG
    if (mWritLocked) {
        mImpl->_writer = false;
        mWritLocked.store(false);
    } else if (mReadLocked) {
        if (mImpl->_readers > 0) {
            --mImpl->_readers;
        }
        mReadLocked.store(false);
    } else {
        if (mImpl->_writer) {
            mImpl->_writer = false;
        } else if (mImpl->_readers > 0) {
            --mImpl->_readers;
        }
    }
#else
    if (mImpl->_writer) {
        mImpl->_writer = false;
    } else if (mImpl->_readers > 0) {
        --mImpl->_readers;
    }
#endif
    WakeAllConditionVariable(&mImpl->_cv);
    LeaveCriticalSection(&mImpl->_lock);
#else
    if (pthread_rwlock_unlock(&mImpl->_rwlock) != 0) {
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
#endif
}

struct once_flag_impl
{
#if defined(OS_WINDOWS)
    INIT_ONCE _once = INIT_ONCE_STATIC_INIT;
    void (*_callback)(void) = nullptr;
#else
    pthread_once_t _once = PTHREAD_ONCE_INIT;
#endif
};

once_flag::once_flag() :
    mImpl(std::unique_ptr<once_flag_impl>(new once_flag_impl))
{
}

once_flag::~once_flag()
{
    mImpl.reset();
}

namespace detail {
THREAD_LOCAL void* __once_callable;
THREAD_LOCAL void (*__once_call)();

extern "C" void call_once_proxy(void)
{
    detail::__once_call();
}
} // namespace detail

extern "C" int32_t call_once_internal(once_flag_impl *once, void (*callback)(void))
{
#if defined(OS_WINDOWS)
    once->_callback = callback;
    auto once_proxy = [] (PINIT_ONCE, PVOID parameter, PVOID *) -> BOOL {
        once_flag_impl *impl = static_cast<once_flag_impl *>(parameter);
        if (impl->_callback) {
            impl->_callback();
        }
        return TRUE;
    };
    return InitOnceExecuteOnce(&once->_once, once_proxy, once, nullptr) ? 0 : static_cast<int32_t>(GetLastError());
#else
    return pthread_once(&once->_once, callback);
#endif
}

} // namespace eular