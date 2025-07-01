/*************************************************************************
    > File Name: endian.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年06月15日 星期六 11时17分53秒
 ************************************************************************/

#include "utils/endian.hpp"

#include "utils/mutex.h"

static bool g_isLittleEngine = false;

#ifndef BYTE_ORDER
static eular::once_flag g_endianOnceFlag;

static inline void __IsLittleEngine()
{
    const uint16_t NUMBER = 0x1122;
    const uint8_t  FIRST_BYTE = 0x22;
    union {
        uint8_t     oneByte;
        uint16_t    twoByte;
    } container;
    container.twoByte = NUMBER;

    g_isLittleEngine = (container.oneByte == FIRST_BYTE);
}
#endif

namespace runtime {
static inline bool IsLittleEngine()
{
#ifndef BYTE_ORDER
    eular::call_once(g_endianOnceFlag, __IsLittleEngine);
#else
#if BYTE_ORDER == LITTLE_EDNIAN
    g_isLittleEngine = true;
#endif
#endif
    return g_isLittleEngine;
}
} // namespace runtime
