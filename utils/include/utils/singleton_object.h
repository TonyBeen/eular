/*************************************************************************
    > File Name: singleton_object.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年05月07日 星期二 11时25分39秒
 ************************************************************************/

#ifndef __UTILS_SINGLETON_OBJECT_H__
#define __UTILS_SINGLETON_OBJECT_H__

#include <utils/exception.h>
#include <utils/refcount.h>

namespace eular {
template <typename T>
class Singleton;

template<typename T>
struct SObject
{
private:
    friend class Singleton<T>;
    SObject(T *obj, RefCount *ref) :
        mObj(obj),
        mRefPtr(ref)
    {
        mRefPtr->ref();
    }

public:
    SObject(const SObject &other)
    {
        mObj = other.mObj;
        mRefPtr = other.mRefPtr;
        mRefPtr->ref();
    }

    SObject &operator=(const SObject &other)
    {
        mObj = other.mObj;
        mRefPtr = other.mRefPtr;
        mRefPtr->ref();
        return *this;
    }

    ~SObject()
    {
        mObj = nullptr;
        mRefPtr->deref();
        mRefPtr = nullptr;
    }

    T *operator->()
    {
        if (mObj == nullptr) {
            throw Exception("nullptr object");
        }
        return mObj;
    }

    T &operator*()
    {
        if (mObj == nullptr) {
            throw Exception("nullptr object");
        }
        return *mObj;
    }

    operator T*()
    {
        return mObj;
    }

    operator const T*() const
    {
        return mObj;
    }

private:
    T *mObj;
    RefCount *mRefPtr;
};
} // namespace eular

#endif // __UTILS_SINGLETON_OBJECT_H__
