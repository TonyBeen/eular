/*************************************************************************
    > File Name: singleton.h
    > Author: hsz
    > Mail:
    > Created Time: Wed 22 Sep 2021 09:34:09 AM CST
 ************************************************************************/

#ifndef __SINGLETON_H__
#define __SINGLETON_H__

#include <utils/mutex.h>
#include <utils/singleton_object.h>

namespace eular {
template<typename T>
class Singleton {
public:
    // 编译期间检测类型完整性
    static_assert(sizeof(T), "incomplete type");

    template<typename... Args>
    static SObject<T> Get(Args&&... args)
    {
        call_once(mFlag, [&] () {
            WRAutoLock<RWMutex> wlock(mMutex);
            if (mInstance == nullptr) {
                mInstance = new T(std::forward<Args>(args)...);
                mDeleter.registration(); // 模板静态成员变量需要使用才会构造
                // ::atexit(Singleton<T>::free); // 在mian结束后调用free函数
            }
        });

        RDAutoLock<RWMutex> rlock(mMutex);
        SObject<T> obj(mInstance, &mRef);
        return obj;
    }

    /**
     * @brief 重置实例, 会返回一个新的地址，所以原来的会失效，对于单例模式，此方法用的不太多 
     */
    template<typename... Args>
    static SObject<T> Reset(Args&&... args)
    {
        WRAutoLock<RWMutex> wlock(mMutex);
        if (mRef.load() == 0) {
            if (mInstance != nullptr) {
                delete mInstance;
                mInstance = nullptr;
            }
            mInstance = new T(std::forward<Args>(args)...);
        }

        SObject<T> obj(mInstance, &mRef);
        return obj;
    }

    static void Free()
    {
        if (mRef.load() > 0) {
            return;
        }

        WRAutoLock<RWMutex> wlock(mMutex);
        if (mInstance != nullptr) {
            delete mInstance;
            mInstance = nullptr;
        }
    }

private:
    class Deleter {
    public:
        void registration() { }
        ~Deleter()
        {
            Singleton<T>::Free();
        }
    };

private:
    static T*           mInstance;
    static RefCount     mRef;
    static RWMutex      mMutex;
    static Deleter      mDeleter;
    static once_flag    mFlag;

    Singleton() {}
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
};

template<typename T>
T *Singleton<T>::mInstance = nullptr;

template<typename T>
RWMutex Singleton<T>::mMutex;

template<typename T>
RefCount Singleton<T>::mRef;

template<typename T>
typename Singleton<T>::Deleter Singleton<T>::mDeleter;

template<typename T>
once_flag Singleton<T>::mFlag;

} // namespace eular

#endif  // __SINGLETON_H__
