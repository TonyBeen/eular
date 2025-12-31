/*************************************************************************
    > File Name: mutex.h
    > Author: hsz
    > Desc: for mutex
    > Created Time: 2021年04月25日 星期日 21时24分54秒
 ************************************************************************/

#ifndef __UTILS_MUTEX_H__
#define __UTILS_MUTEX_H__

#include <stdint.h>
#include <assert.h>
#include <stdbool.h>

#include <atomic>
#include <memory>
#include <functional>

#include <utils/sysdef.h>

#if defined(OS_LINUX)
#include <semaphore.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#endif

#include <utils/utils.h>
#include <utils/string8.h>

namespace eular {
class NonCopyAble
{
public:
    NonCopyAble() = default;
    ~NonCopyAble() = default;
    NonCopyAble(const NonCopyAble&) = delete;
    NonCopyAble& operator=(const NonCopyAble&) = delete;
};

template<typename MutexType>
class AutoLock final : public NonCopyAble
{
public:
    AutoLock(MutexType& mutex) :
        mMutex(mutex)
    {
        mMutex.lock();
    }

    ~AutoLock()
    {
        mMutex.unlock();
    }

private:
    MutexType& mMutex;
};

class SpinLock final : public NonCopyAble
{
public:
    SpinLock();

    void lock() noexcept;
    bool trylock() noexcept;
    void unlock() noexcept;

private:
    struct SpinLockImpl;
    std::unique_ptr<SpinLockImpl>   m_impl;
};

struct MutexImpl;
class Mutex final : public NonCopyAble
{
public:
    Mutex();
    ~Mutex();

    int32_t lock();
    int32_t trylock();
    void    unlock();

    void setMutexName(const String8 &name);
    const String8 &getMutexName() const { return m_name; }

private:
    friend class Condition;
    std::unique_ptr<MutexImpl>  m_impl;
    String8                     m_name;
};

class RecursiveMutex final : public NonCopyAble
{
public:
    RecursiveMutex();
    ~RecursiveMutex();

    int32_t lock();
    int32_t trylock();
    void    unlock();

    void setMutexName(const String8 &name);
    const String8 &getMutexName() const { return mName; }

private:
    friend class Condition;
    String8                     mName;
    std::unique_ptr<MutexImpl>  mImpl;
};

// 局部写锁
template<typename WRMutexType>
class WRAutoLock final
{
public:
    WRAutoLock(WRMutexType& mtx) : mutex(mtx)
    {
        mutex.wlock();
    }
    ~WRAutoLock()
    {
        mutex.unlock();
    }

private:
    WRMutexType &mutex;
};

// 局部读锁
template<typename RDMutexType>
class RDAutoLock final
{
public:
    RDAutoLock(RDMutexType& mtx) : mutex(mtx)
    {
        mutex.rlock();
    }
    ~RDAutoLock()
    {
        mutex.unlock();
    }

private:
    RDMutexType &mutex;
};

class RWMutex final : public NonCopyAble {
public:
    typedef RDAutoLock<RWMutex> ReadAutoLock;
    typedef WRAutoLock<RWMutex> WriteAutoLock;

    RWMutex();
    ~RWMutex();
    void rlock();
    void wlock();
    void unlock();

private:
    struct RWMutexImpl;
    std::unique_ptr<RWMutexImpl> mImpl;
#ifdef DEBUG
    std::atomic<bool> mReadLocked;
    std::atomic<bool> mWritLocked;
#endif
};

class Sem final : public NonCopyAble {
public:
    Sem(const char *semPath, uint8_t val);      // 此种走有名信号量
    Sem(uint8_t valBase);                       // 此种走无名信号量
    Sem(const Sem &) = delete;
    ~Sem();

    bool post();
    bool wait();
    bool trywait();
    bool timedwait(uint32_t ms);

private:
    struct SemImpl;
    std::unique_ptr<SemImpl> mImpl;

    String8 mFilePath;  // 有名信号量使用
    bool    isNamedSemaphore;
};

struct once_flag_impl;
struct once_flag
{
    template<typename Callable, typename... Args>
    friend void call_once(once_flag& once, Callable&& f, Args&&... args);

    DISALLOW_COPY_AND_ASSIGN(once_flag);
public:
    once_flag();
    ~once_flag();

private:
    std::unique_ptr<once_flag_impl> mImpl;
};

namespace detail {
extern THREAD_LOCAL void* __once_callable;
extern THREAD_LOCAL void (*__once_call)();
extern "C" void call_once_proxy(void);
extern "C" int32_t call_once_internal(once_flag_impl *once, void (*callback)(void));
} // namespace detail

template<typename Callable, typename... Args>
void call_once(once_flag& once, Callable&& f, Args&&... args)
{
    auto __callable = [&] () {
        std::forward<Callable>(f)(std::forward<Args>(args)...);
    };
    detail::__once_callable = std::addressof(__callable);
    detail::__once_call = [] () { (*(decltype(__callable)*)detail::__once_callable)(); };

    int code = detail::call_once_internal(once.mImpl.get(), &detail::call_once_proxy);
    if (code) {
        throw std::runtime_error("call_once_internal return error");
    }

    detail::__once_callable = nullptr;
    detail::__once_call = nullptr;
}

} // namespace eular

#endif // __UTILS_MUTEX_H__
