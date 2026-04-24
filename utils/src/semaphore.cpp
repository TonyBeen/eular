/*************************************************************************
    > File Name: semaphore.cpp
    > Author: hsz
    > Desc:
    > Created Time: 2026年04月24日 星期五
 ************************************************************************/

#include "utils/semaphore.h"

#include <string.h>
#include <errno.h>
#include <new>
#include <time.h>

#include <sys/stat.h>
#include <fcntl.h>

#if defined(OS_LINUX)
#include <semaphore.h>
#elif defined(OS_APPLE)
#include <dispatch/dispatch.h>
#elif defined(OS_WINDOWS)
#include <windows.h>
#endif

#include "utils/errors.h"
#include "utils/utils.h"
#include "utils/exception.h"

namespace eular {

struct Semaphore::SemaphoreImpl {
#ifdef OS_WINDOWS
    HANDLE _sem{};
#elif defined(OS_LINUX)
    sem_t *_sem{};
#elif defined(OS_APPLE)
    dispatch_semaphore_t _semApple{};
#endif
};

Semaphore::Semaphore(const char *semPath, uint8_t val) :
    mFilePath(semPath),
    isNamedSemaphore(true)
{
    mImpl = std::unique_ptr<SemaphoreImpl>(new SemaphoreImpl);
#if defined(OS_LINUX)
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
#elif defined(OS_APPLE)
    UNUSED(semPath);
    UNUSED(val);
    throw Exception("named semaphore is not supported on macOS; use anonymous Semaphore(uint8_t) instead");
#endif
}

Semaphore::Semaphore(uint8_t valBase) :
    isNamedSemaphore(false)
{
    mImpl = std::unique_ptr<SemaphoreImpl>(new SemaphoreImpl);
#if defined(OS_LINUX)
    mImpl->_sem = new (std::nothrow) sem_t;
    if (mImpl->_sem == nullptr) {
        throw Exception("new sem_t error. no more memory");
    }

    if (sem_init(mImpl->_sem, false, valBase)) {
        throw Exception(String8::Format("%s() sem_init error %d, %s", __func__, errno, strerror(errno)));
    }
#elif defined(OS_APPLE)
    mImpl->_semApple = dispatch_semaphore_create(static_cast<intptr_t>(valBase));
    if (!mImpl->_semApple) {
        throw Exception("dispatch_semaphore_create error. no more memory");
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

Semaphore::~Semaphore()
{
#ifdef OS_WINDOWS
    if (mImpl->_sem != nullptr) {
        CloseHandle(mImpl->_sem);
        mImpl->_sem = nullptr;
    }
#elif defined(OS_LINUX)
    if (mImpl->_sem != nullptr) {
        if (isNamedSemaphore) {
            sem_close(mImpl->_sem);
            sem_unlink(mFilePath.c_str());
        } else {
            sem_destroy(mImpl->_sem);
            delete mImpl->_sem;
        }
        mImpl->_sem = nullptr;
    }
#elif defined(OS_APPLE)
    if (mImpl->_semApple != nullptr) {
        dispatch_release(mImpl->_semApple);
        mImpl->_semApple = nullptr;
    }
#endif
}

// see https://stackoverflow.com/questions/2013181/gdb-causes-sem-wait-to-fail-with-eintr-error
bool Semaphore::post()
{
#if defined(OS_WINDOWS)
    BOOL state = ReleaseSemaphore(mImpl->_sem, 1, nullptr);
    if (!state) {
        errno = GetLastError();
    }
    return state != 0;
#elif defined(OS_LINUX)
    int32_t rt = 0;
    do {
        rt = sem_post(mImpl->_sem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#elif defined(OS_APPLE)
    dispatch_semaphore_signal(mImpl->_semApple);
    return true;
#endif
}

bool Semaphore::wait()
{
#if defined(OS_WINDOWS)
    DWORD status = WaitForSingleObject(mImpl->_sem, INFINITE);
    if (status != WAIT_OBJECT_0) {
        errno = GetLastError();
        return false;
    }
    return true;
#elif defined(OS_LINUX)
    int32_t rt = 0;
    do {
        rt = sem_wait(mImpl->_sem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#elif defined(OS_APPLE)
    return dispatch_semaphore_wait(mImpl->_semApple, DISPATCH_TIME_FOREVER) == 0;
#endif
}

bool Semaphore::trywait()
{
#if defined(OS_WINDOWS)
    DWORD status = WaitForSingleObject(mImpl->_sem, 0);
    if (status != WAIT_OBJECT_0) {
        errno = GetLastError();
        return false;
    }
    return true;
#elif defined(OS_LINUX)
    int32_t rt = 0;
    do {
        rt = sem_trywait(mImpl->_sem);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#elif defined(OS_APPLE)
    return dispatch_semaphore_wait(mImpl->_semApple, DISPATCH_TIME_NOW) == 0;
#endif
}

bool Semaphore::timedwait(uint32_t ms)
{
#if defined(OS_WINDOWS)
    DWORD status = WaitForSingleObject(mImpl->_sem, ms);
    if (status != WAIT_OBJECT_0) {
        errno = GetLastError();
        return false;
    }
    return true;
#elif defined(OS_LINUX)
    struct timespec expire;
    clock_gettime(CLOCK_REALTIME, &expire);
    expire.tv_sec += ms / 1000;
    expire.tv_nsec += static_cast<long>(ms % 1000) * 1000000;
    if (expire.tv_nsec >= 1000000000) {
        ++expire.tv_sec;
        expire.tv_nsec -= 1000000000;
    }
    int32_t rt = 0;
    do {
        rt = sem_timedwait(mImpl->_sem, &expire);
    } while (rt == -1 && errno == EINTR);
    return 0 == rt;
#elif defined(OS_APPLE)
    int64_t nanos = static_cast<int64_t>(ms) * 1000000;
    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, nanos);
    return dispatch_semaphore_wait(mImpl->_semApple, timeout) == 0;
#endif
}

} // namespace eular
