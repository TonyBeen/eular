/*************************************************************************
    > File Name: max_stream_data.h
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_MAX_STREAM_DATA_H__
#define __UTP_PROTO_FRAME_MAX_STREAM_DATA_H__

#include "proto/frame.h"

#define FRAME_MAX_STREAM_DATA_SIZE (1 + 4 + 8)

namespace eular {
namespace utp {

struct FrameMaxStreamData : public FrameBase {
public:
    FrameMaxStreamData() : FrameBase(FrameType::kFrameMaxStreamData) {}
    ~FrameMaxStreamData() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint32_t stream_id{0};
    uint64_t maximum_stream_data{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_MAX_STREAM_DATA_H__
