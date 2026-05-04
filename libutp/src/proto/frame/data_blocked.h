/*************************************************************************
    > File Name: data_blocked.h
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_DATA_BLOCKED_H__
#define __UTP_PROTO_FRAME_DATA_BLOCKED_H__

#include "proto/frame.h"

#define FRAME_DATA_BLOCKED_SIZE (1 + 8)

namespace eular {
namespace utp {

struct FrameDataBlocked : public FrameBase {
public:
    FrameDataBlocked() : FrameBase(FrameType::kFrameDataBlocked) {}
    ~FrameDataBlocked() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint64_t data_limit{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_DATA_BLOCKED_H__
