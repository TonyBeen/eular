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

#include <utils/utils.h>
#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#include <windows.h>

typedef DWORD tls_key_t;
inline void tls_init(tls_key_t* k) { *k = TlsAlloc(); assert(*k != TLS_OUT_OF_INDEXES); }
inline void tls_free(tls_key_t k) { TlsFree(k); }
inline void* tls_get(tls_key_t k) { return TlsGetValue(k); }
inline void tls_set(tls_key_t k, void* v) { BOOL r = TlsSetValue(k, v); assert(r); (void)r; }
#else
#include <pthread.h>

typedef pthread_key_t tls_key_t;
inline void tls_init(tls_key_t* k) { int r = pthread_key_create(k, 0); (void)r; assert(r == 0); }
inline void tls_free(tls_key_t k) { int r = pthread_key_delete(k); (void)r; assert(r == 0); }
inline void* tls_get(tls_key_t k) { return pthread_getspecific(k); }
inline void tls_set(tls_key_t k, void* v) { int r = pthread_setspecific(k, v); (void)r; assert(r == 0); }
#endif

namespace eular {
class UTILS_API TLSAbstractSlot
{
public:
    TLSAbstractSlot() {}
    virtual ~TLSAbstractSlot() {}
};

template <typename T>
class UTILS_API TLSSlot: public TLSAbstractSlot
{
    DISALLOW_COPY_AND_ASSIGN(TLSSlot);
public:
    TLSSlot(): m_value() { }
    TLSSlot(T &&value): m_value(std::forward<T>(value)) { }
    ~TLSSlot() = default;

    T &value() {
        return m_value;
    }

    T *pointer() {
        return &m_value;
    }

    T* operator->() const {
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
    TLS() { tls_init(&m_key); }
    ~TLS() { tls_free(m_key); }

    void set(T *value) {
        tls_set(m_key, value);
    }

    T *get() {
        return (T *)tls_get(m_key);
    }

    T *operator->() const {
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
    std::shared_ptr<TLSSlot<T>> get(const std::string &key)
    {
        auto it = m_tlsMap.find(key);
        if (it == m_tlsMap.end()) {
            // it = m_tlsMap.insert(std::make_pair(key, std::make_shared<TLSSlot<T>>())).first;
            return nullptr;
        }

        return std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
    }

    template <typename T>
    std::shared_ptr<TLSSlot<T>> set(const std::string &key, T &&value)
    {
        std::shared_ptr<TLSSlot<T>> result;
        auto it = m_tlsMap.find(key);
        if (it == m_tlsMap.end()) {
            it = m_tlsMap.insert(std::make_pair(key, std::make_shared<TLSSlot<T>>(std::forward<T>(value)))).first;
            result = std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
        } else {
            result = std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
            result->value() = std::forward<T>(value);
        }

        return result;
    }

    static ThreadLocalStorage *Current();

private:
    using TLSMap = std::map<std::string, std::shared_ptr<TLSAbstractSlot>>;

    TLSMap m_tlsMap;
};

} // namespace eular

#endif // __EULAR_UTILS_THREAD_H__
