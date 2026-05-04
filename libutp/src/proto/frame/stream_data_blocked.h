/*************************************************************************
    > File Name: stream_data_blocked.h
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_STREAM_DATA_BLOCKED_H__
#define __UTP_PROTO_FRAME_STREAM_DATA_BLOCKED_H__

#include "proto/frame.h"

#define FRAME_STREAM_DATA_BLOCKED_SIZE (1 + 4 + 8)

namespace eular {
namespace utp {

struct FrameStreamDataBlocked : public FrameBase {
public:
    FrameStreamDataBlocked() : FrameBase(FrameType::kFrameStreamDataBlocked) {}
    ~FrameStreamDataBlocked() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint32_t stream_id{0};
    uint64_t stream_data_limit{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_STREAM_DATA_BLOCKED_H__
