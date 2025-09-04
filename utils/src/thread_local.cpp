/*************************************************************************
    > File Name: thread_local.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年12月18日 星期三 10时58分48秒
 ************************************************************************/

#include "utils/thread_local.h"
#include "utils/sysdef.h"
#include "utils/mutex.h"

#include <assert.h>



namespace eular {
// class TLSPrivate {
// public:
//     tls_key_t m_key;

//     ~TLSPrivate() { tls_free(m_key); }
//     TLSPrivate() { tls_init(&m_key); }

//     void *get() { return tls_get(m_key); }
//     void set(void *v) { tls_set(m_key, v); }
// };

// TLS::TLS() :
//     m_type(&typeid(void))
// {
//     m_slot = std::make_shared<TLSPrivate>();
// }

// TLS::~TLS()
// {
//     m_slot.reset();
// }

// void *TLS::getKey()
// {
//     return m_slot->get();
// }

// void TLS::setKey(void *key)
// {
//     m_slot->set(key);
// }

static thread_local std::shared_ptr<ThreadLocalStorage> g_tls = nullptr;

ThreadLocalStorage *ThreadLocalStorage::Current()
{
    if (g_tls == nullptr) {
        g_tls = std::make_shared<ThreadLocalStorage>();
    }

    return g_tls.get();
}
} // namespace eular
