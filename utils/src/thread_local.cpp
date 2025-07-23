/*************************************************************************
    > File Name: thread_local.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年12月18日 星期三 10时58分48秒
 ************************************************************************/

#include "utils/thread_local.h"
#include "utils/sysdef.h"
#include "utils/mutex.h"

#if defined(OS_LINUX)
#include <pthread.h>
#endif

namespace eular {

static thread_local std::shared_ptr<ThreadLocalStorage> g_tls = nullptr;

ThreadLocalStorage *ThreadLocalStorage::Current()
{
    if (g_tls == nullptr) {
        g_tls = std::make_shared<ThreadLocalStorage>();
    }

    return g_tls.get();
}
} // namespace eular
