/*************************************************************************
    > File Name: frame_meta_policy.cpp
    > Author: eular
    > Brief:
 ************************************************************************/

#include "context/detail/frame_meta_policy.h"

#include "proto/packet_out.h"

namespace eular {
namespace utp {
namespace detail {

uint8_t FrameMetaPolicy::DefaultFlags(FrameType frameType, uint16_t transientAckBytes)
{
    uint8_t flags = kFMRetransMustKeep;
    if (frameType == kFrameAck && transientAckBytes > 0) {
        flags = static_cast<uint8_t>(kFMTransientOnRetrans | kFMDroppableOnMtu);
    } else if (frameType == kFrameStream) {
        flags = static_cast<uint8_t>(kFMRetransMustKeep | kFMSplittable);
    }
    return flags;
}

} // namespace detail
} // namespace utp
} // namespace eular
