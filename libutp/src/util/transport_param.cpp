/*************************************************************************
    > File Name: transport_param.cpp
    > Author: eular
    > Brief:
    > Created Time: Fri 30 Jan 2026 05:19:17 PM CST
 ************************************************************************/

#include "util/transport_param.h"

namespace eular {
namespace utp {
void TransportParams::clearParam(int32_t mask)
{
    while (mask) {
        uint32_t bits = mask & (~mask + 1u);
        flags &= ~bits; // 清掉对应位
        mask &= (mask - 1u); // 清掉最低位 1
    }
}
} // namespace utp
} // namespace eular
