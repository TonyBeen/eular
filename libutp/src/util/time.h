/*************************************************************************
    > File Name: time.h
    > Author: eular
    > Brief:
    > Created Time: Wed 14 Jan 2026 03:31:29 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_TIME_H__
#define __UTP_UTIL_TIME_H__

#include <stdint.h>
#include <atomic>

#include "utp/platform.h"
#include "commom.h"

namespace eular {
namespace utp {
namespace time {

uint64_t MonotonicMs();

uint64_t MonotonicUs();

uint64_t RealtimeMs();

int32_t  Timezone();

} // namespace time
} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_TIME_H__
