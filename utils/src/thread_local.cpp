/*************************************************************************
    > File Name: thread_local.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年12月18日 星期三 10时58分48秒
 ************************************************************************/

#include "utils/thread_local.h"
namespace eular {

ThreadLocalStorage *ThreadLocalStorage::Current() noexcept
{
    static thread_local ThreadLocalStorage tls;
    return &tls;
}
} // namespace eular
