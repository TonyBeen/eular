/*************************************************************************
    > File Name: mutex.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年05月10日 星期五 18时51分26秒
 ************************************************************************/

#ifndef __LOG_MUTEX_H__
#define __LOG_MUTEX_H__

#include <assert.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>

#include "log_write.h"

class ProcessMutex : public NonCopyAndAssign
{
public:
    ProcessMutex()
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
#if defined(_POSIX_THREAD_PROCESS_SHARED)
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        _mutex = (pthread_mutex_t *)mmap(nullptr, sizeof(pthread_mutex_t),
                                         PROT_WRITE|PROT_READ, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
#else
        _mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
#endif
        assert(_mutex);
        pthread_mutex_init(_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    ~ProcessMutex()
    {
        if (_mutex != nullptr) {
            pthread_mutex_destroy(_mutex);
#if defined(_POSIX_THREAD_PROCESS_SHARED)
            munmap(_mutex, sizeof(pthread_mutex_t));
#else
            free(_mutex);
#endif
            _mutex = nullptr;
        }
    }

    void lock()
    {
        TEMP_FAILURE_RETRY(pthread_mutex_lock(_mutex));
    }

    void unlock()
    {
        TEMP_FAILURE_RETRY(pthread_mutex_unlock(_mutex));
    }

private:
    pthread_mutex_t *_mutex;
};

template<typename T>
class AutoLock : public NonCopyAndAssign
{
public:
    AutoLock(T &mtx) : _mutex(mtx)
    {
        _mutex.lock();
    }

    AutoLock(std::shared_ptr<T> mtx) : _mutex(*mtx.get())
    {
        _mutex.lock();
    }

    ~AutoLock()
    {
        _mutex.unlock();
    }

private:
    T &_mutex;
};

#endif // __LOG_MUTEX_H__
