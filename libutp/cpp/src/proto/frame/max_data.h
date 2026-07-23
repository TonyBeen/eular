/*************************************************************************
    > File Name: max_data.h
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_MAX_DATA_H__
#define __UTP_PROTO_FRAME_MAX_DATA_H__

#include "proto/frame.h"

#define FRAME_MAX_DATA_SIZE (1 + 8)

namespace eular {
namespace utp {

struct FrameMaxData : public FrameBase {
public:
    FrameMaxData() : FrameBase(FrameType::kFrameMaxData) {}
    ~FrameMaxData() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;

public:
    uint64_t maximum_data{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_MAX_DATA_H__
