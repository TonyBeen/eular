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

#include <pthread.h>

#include "utils/errors.h"
#include "utils/utils.h"
#include "utils/exception.h"
#include "src/mutex.hpp"

namespace eular {
// @see https://codebrowser.dev/glibc/glibc/nptl/pthread_spin_lock.c.html

struct SpinLock::SpinLockImpl {
    pthread_spinlock_t _spinlock{};
};

SpinLock::SpinLock()
{
    m_impl = std::unique_ptr<SpinLockImpl>(new SpinLockImpl);
}

bool SpinLock::trylock() noexcept
{
    return pthread_spin_trylock(&m_impl->_spinlock) == 0;
}

void SpinLock::unlock() noexcept
{
    pthread_spin_unlock(&m_impl->_spinlock);
}

void SpinLock::lock() noexcept
{
    pthread_spin_lock(&m_impl->_spinlock); // No need to check the return value
}

Mutex::Mutex()
{
    m_impl = std::unique_ptr<MutexImpl>(new MutexImpl);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);       // for pthread_mutex_lock will return EOWNERDEAD
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK_NP);  // for EDEADLK
    pthread_mutex_init(&m_impl->_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

Mutex::~Mutex()
{
    int32_t ret = 0;
    do {
        if (ret == EBUSY) {
            unlock();
        }
        ret = pthread_mutex_destroy(&m_impl->_mutex);
    } while (ret == EBUSY);
    assert(ret == 0);
}

void Mutex::setMutexName(const String8 &name)
{
    m_name = name;
}

int32_t Mutex::lock()
{
    int32_t ret = 0;
    do {
        ret = pthread_mutex_lock(&m_impl->_mutex);
        if (ret == EDEADLK) { // already locked
            throw Exception(String8::Format("deadlock detected: tid = %d mutex: %s", (int32_t)gettid(), m_name.c_str()));
        } else if (ret == EOWNERDEAD) { // other threads exited abnormally without unlocking the mutex
            pthread_mutex_consistent(&m_impl->_mutex); // will lock the mutex
            ret = 0;
        }
    } while (0);

    return ret;
}

void Mutex::unlock()
{
    int32_t ret = pthread_mutex_unlock(&m_impl->_mutex);
    if (ret != 0 && ret != EPERM) { // EPERM: the calling thread does not own the mutex
        throw Exception(String8::Format("pthread_mutex_unlock error. return %d", ret));
    }
}

int32_t Mutex::trylock()
{
    return pthread_mutex_trylock(&m_impl->_mutex);
}

RecursiveMutex::RecursiveMutex()
{
    mImpl = std::unique_ptr<MutexImpl>(new MutexImpl);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);       // for pthread_mutex_lock will return EOWNERDEAD
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);   // for recursive
    pthread_mutex_init(&mImpl->_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

RecursiveMutex::~RecursiveMutex()
{
    int32_t ret = 0;
    do {
        if (ret == EBUSY) {
            unlock();
        }
        ret = pthread_mutex_destroy(&mImpl->_mutex);
    } while (ret == EBUSY);
    assert(ret == 0);
}

int32_t RecursiveMutex::lock()
{
    int32_t ret = 0;
    do {
        ret = pthread_mutex_lock(&mImpl->_mutex);
        if (ret == EOWNERDEAD) { // other threads exited abnormally without unlocking the mutex
            pthread_mutex_consistent(&mImpl->_mutex); // will lock the mutex
            ret = 0;
        }
    } while (0);

    return ret;
}

void RecursiveMutex::unlock()
{
    int32_t ret = pthread_mutex_unlock(&mImpl->_mutex);
    if (ret != 0 && ret != EPERM) { // EPERM: the calling thread does not own the mutex
        throw Exception(String8::Format("pthread_mutex_unlock error. return %d", ret));
    }
}

int32_t RecursiveMutex::trylock()
{
    return pthread_mutex_trylock(&mImpl->_mutex);
}

void RecursiveMutex::setMutexName(const String8 &name)
{
    mName = name;
}

struct RWMutex::RWMutexImpl {
    pthread_rwlock_t _rwlock{};
};

RWMutex::RWMutex()
{
    mImpl = std::unique_ptr<RWMutexImpl>(new RWMutexImpl);
    pthread_rwlock_init(&mImpl->_rwlock, nullptr);
}

RWMutex::~RWMutex()
{
    pthread_rwlock_destroy(&mImpl->_rwlock);
}

void RWMutex::rlock()
{
    int32_t ret = pthread_rwlock_rdlock(&mImpl->_rwlock);
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
    if (pthread_rwlock_wrlock(&mImpl->_rwlock) != 0) {
        throw Exception("pthread_rwlock_wrlock error");
    } else {
#ifdef DEBUG
        mWritLocked.store(true);
#endif
    }
}

void RWMutex::unlock()
{
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
}

struct Sem::SemImpl {
#ifdef OS_WINDOWS
    HANDLE _sem;    // 信号量
#else
    sem_t* _sem;    // 信号量
#endif // OS_WINDOWS
};

Sem::Sem(const char *semPath, uint8_t val) :
    mFilePath(semPath),
    isNamedSemaphore(true)
{
    mImpl = std::unique_ptr<SemImpl>(new SemImpl);
#if defined(OS_LINUX) || defined(OS_MACOS)
    if (semPath == nullptr) {
        throw Exception("the first param can not be null");
    }

    // 如果信号量已存在，则后两个参数会忽略，详见man sem_open
    mImpl->_sem = sem_open(semPath, O_CREAT | O_RDWR, 0664, val);
    if (mImpl->_sem == SEM_FAILED) {
        String8 erorMsg = String8::Format("sem_open failed. [%d, %s]", errno, strerror(errno));
        throw Exception(erorMsg);
    }
#elif defined(OS_WINDOWS)
    mImpl->_sem = CreateSemaphoreA(
        NULL,
        val,
        val,
        semPath
    );
    if (mImpl->_sem == nullptr) {
        int32_t status = GetLastError();
        String8 erorMsg = String8::Format("CreateSemaphoreA failed. [%d, %s]", status, FormatErrno(status));
        throw Exception(erorMsg);
    }
#endif
}

Sem::Sem(uint8_t valBase) :
    isNamedSemaphore(false)
{
    mImpl = std::unique_ptr<SemImpl>(new SemImpl);
#if defined(OS_LINUX) || defined(OS_MACOS)
    mImpl->_sem = new (std::nothrow)sem_t;
    if (mImpl->_sem == nullptr) {
        throw Exception("new sem_t error. no more memory");
    }

    if (sem_init(mImpl->_sem, false, valBase)) {
        throw Exception(String8::Format("%s() sem_init error %d, %s", __func__, errno, strerror(errno)));
    }
#else
    mImpl->_sem = CreateSemaphoreA(
        NULL,
        valBase,
        valBase,
        nullptr
    );
    if (mImpl->_sem == nullptr) {
        int32_t status = GetLastError();
        String8 erorMsg = String8::Format("CreateSemaphoreA failed. [%d, %s]", status, FormatErrno(status));
        throw Exception(erorMsg);
    }
#endif
}

Sem::~Sem()
{
    if (mImpl->_sem != nullptr) {
#ifdef OS_WINDOWS
         CloseHandle(mImpl->_sem);
#else
        if (isNamedSemaphore) {
            sem_close(mImpl->_sem);
            sem_unlink(mFilePath.c_str());
        } else {
            sem_destroy(mImpl->_sem);
            delete mImpl->_sem;
        }
#endif // OS_WINDOWS
        mImpl->_sem = nullptr;
    }
}

// see https://stackoverflow.com/questions/2013181/gdb-causes-sem-wait-to-fail-with-eintr-error
bool Sem::post()
{
#if defined(OS_WINDOWS)
    BOOL state = ReleaseSemaphore(mImpl->_sem, 1, nullptr);
    if (!state) {
        errno = GetLastError();
    }
    return state != 0;
#else
    int32_t rt = 0;
    do {
        rt = sem_post(mImpl->_sem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#endif
}

bool Sem::wait()
{
#if defined(OS_WINDOWS)
    DWORD status = WaitForSingleObject(mImpl->_sem, INFINITE);
    if (status != WAIT_OBJECT_0) {
        errno = GetLastError();
        return false;
    }
    return true;
#else
    int32_t rt = 0;
    do {
        rt = sem_wait(mImpl->_sem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#endif
}

bool Sem::trywait()
{
#if defined(OS_WINDOWS)
    DWORD status = WaitForSingleObject(mImpl->_sem, 0);
    if (status != WAIT_OBJECT_0) {
        errno = GetLastError();
        return false;
    }
    return true;
#else
    int32_t rt = 0;
    do {
        rt = sem_trywait(mImpl->_sem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#endif
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
        rt = sem_timedwait(mImpl->_sem, &expire);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
}
#else
bool Sem::timedwait(uint32_t ms)
{
    return false;
}
#endif

struct once_flag_impl
{
    pthread_once_t _once = PTHREAD_ONCE_INIT;
};

once_flag::once_flag() :
    mImpl(std::unique_ptr<once_flag_impl>(new once_flag_impl))
{
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
    return pthread_once(&once->_once, callback);
}

}