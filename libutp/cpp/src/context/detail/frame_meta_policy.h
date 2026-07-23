/*************************************************************************
    > File Name: frame_meta_policy.h
    > Author: eular
    > Brief:
 ************************************************************************/

#ifndef __UTP_CONTEXT_DETAIL_FRAME_META_POLICY_H__
#define __UTP_CONTEXT_DETAIL_FRAME_META_POLICY_H__

#include <cstdint>

#include "proto/frame.h"

namespace eular {
namespace utp {
namespace detail {

class FrameMetaPolicy
{
public:
    static uint8_t DefaultFlags(FrameType frameType, uint16_t transientAckBytes);
};

} // namespace detail
} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_DETAIL_FRAME_META_POLICY_H__
