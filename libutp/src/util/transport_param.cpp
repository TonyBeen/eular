/*************************************************************************
    > File Name: transport_param.cpp
    > Author: eular
    > Brief:
    > Created Time: Fri 30 Jan 2026 05:19:17 PM CST
 ************************************************************************/

#include "util/transport_param.h"

namespace eular {
namespace utp {
void TransportParams::clearParam(int32_t param)
{
    switch (param) {
    case kMaxIdleTimeout:
        break;
    case kHandshakeTimeout:
        break;
    case kInitMaxStreamsBidi:
        break;
    case kInitMaxStreamsUni:
        break;
    case kAckDelayExponent:
        break;
    case kMaxAckDelta:
        break;
    case kMaxAckDelay:
        break;
    default:
        return;
    }

    flags &= ~(1 << param);
}
} // namespace utp
} // namespace eular
