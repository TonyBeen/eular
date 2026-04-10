#ifndef __UTILS_MUTEX_HPP__
#define __UTILS_MUTEX_HPP__

#include "utils/sysdef.h"

#if defined(OS_WINDOWS)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace eular {
struct MutexImpl {
#if defined(OS_WINDOWS)
    CRITICAL_SECTION _mutex{};
#else
    pthread_mutex_t _mutex{};
#endif
};

} // namespace eular

#endif