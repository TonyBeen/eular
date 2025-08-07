#ifndef __UTILS_MUTEX_HPP__
#define __UTILS_MUTEX_HPP__

#include <pthread.h>
namespace eular {
struct MutexImpl {
    pthread_mutex_t _mutex{};
};

} // namespace eular

#endif