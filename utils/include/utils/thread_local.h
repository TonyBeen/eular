/*************************************************************************
    > File Name: thread_local.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年12月18日 星期三 10时58分43秒
 ************************************************************************/

#ifndef __EULAR_UTILS_THREAD_H__
#define __EULAR_UTILS_THREAD_H__

#include <assert.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include <utils/utils.h>
#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#include <windows.h>

typedef DWORD tls_key_t;
inline void tls_init(tls_key_t* k) noexcept { *k = TlsAlloc(); assert(*k != TLS_OUT_OF_INDEXES); }
inline void tls_free(tls_key_t k) noexcept { TlsFree(k); }
inline void* tls_get(tls_key_t k) noexcept { return TlsGetValue(k); }
inline void tls_set(tls_key_t k, void* v) noexcept { BOOL r = TlsSetValue(k, v); assert(r); (void)r; }
#else
#include <pthread.h>

typedef pthread_key_t tls_key_t;
inline void tls_init(tls_key_t* k) noexcept { int r = pthread_key_create(k, 0); (void)r; assert(r == 0); }
inline void tls_free(tls_key_t k) noexcept { int r = pthread_key_delete(k); (void)r; assert(r == 0); }
inline void* tls_get(tls_key_t k) noexcept { return pthread_getspecific(k); }
inline void tls_set(tls_key_t k, void* v) noexcept { int r = pthread_setspecific(k, v); (void)r; assert(r == 0); }
#endif

namespace eular {
class UTILS_API TLSAbstractSlot
{
public:
    TLSAbstractSlot() noexcept {}
    virtual ~TLSAbstractSlot() noexcept {}
};

template <typename T>
class UTILS_API TLSSlot: public TLSAbstractSlot
{
    DISALLOW_COPY_AND_ASSIGN(TLSSlot);
public:
    TLSSlot(): m_value() { }
    TLSSlot(const T &value): m_value(value) { }
    TLSSlot(T &&value): m_value(std::forward<T>(value)) { }
    ~TLSSlot() = default;

    T &value() noexcept {
        return m_value;
    }

    T *pointer() noexcept {
        return &m_value;
    }

    T* operator->() noexcept {
        return &m_value;
    }

private:
    T m_value;
};

template <typename T>
class TLS
{
    DISALLOW_COPY_AND_ASSIGN(TLS);
public:
    TLS() noexcept { tls_init(&m_key); }
    ~TLS() noexcept { tls_free(m_key); }

    void set(T *value) noexcept {
        tls_set(m_key, value);
    }

    T *get() const noexcept {
        return (T *)tls_get(m_key);
    }

    T *operator->() const noexcept {
        T* const o = this->get();
        assert(o);
        return o;
    }

private:
    tls_key_t   m_key;
};

class UTILS_API ThreadLocalStorage
{
public:
    ThreadLocalStorage() = default;
    ~ThreadLocalStorage() = default;

    template <typename T>
    std::shared_ptr<TLSSlot<T>> get(const std::string &key) noexcept
    {
        auto it = m_tlsMap.find(key);
        if (it == m_tlsMap.end()) {
            // it = m_tlsMap.insert(std::make_pair(key, std::make_shared<TLSSlot<T>>())).first;
            return nullptr;
        }

        return std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
    }

    template <typename T>
    std::shared_ptr<TLSSlot<T>> set(const std::string &key, const T &value) noexcept
    {
        return setValue<T>(key, value);
    }

    template <typename T>
    std::shared_ptr<TLSSlot<T>> set(const std::string &key, T &&value) noexcept
    {
        return setValue<T>(key, std::forward<T>(value));
    }

private:
    template <typename T, typename Value>
    std::shared_ptr<TLSSlot<T>> setValue(const std::string &key, Value &&value) noexcept
    {
        try {
            std::shared_ptr<TLSSlot<T>> result;
            auto it = m_tlsMap.find(key);
            if (it == m_tlsMap.end()) {
                it = m_tlsMap.insert(std::make_pair(key, std::make_shared<TLSSlot<T>>(std::forward<Value>(value)))).first;
                result = std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
            } else {
                result = std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
                if (result == nullptr) {
                    return nullptr;
                }
                result->value() = std::forward<Value>(value);
            }

            return result;
        } catch (...) {
            return nullptr;
        }
    }

public:
    static ThreadLocalStorage *Current() noexcept;

private:
    using TLSMap = std::map<std::string, std::shared_ptr<TLSAbstractSlot>>;

    TLSMap m_tlsMap;
};

} // namespace eular

#endif // __EULAR_UTILS_THREAD_H__
