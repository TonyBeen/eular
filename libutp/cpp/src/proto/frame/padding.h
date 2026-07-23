/*************************************************************************
    > File Name: padding.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:55:17 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_PADDING_H__
#define __UTP_PROTO_FRAME_PADDING_H__

#include "proto/frame.h"

#define FRAME_PADDING_HDR_SIZE    (1 + 2) // type + padding_length

namespace eular {
namespace utp {

struct FramePadding : public FrameBase {
public:
    FramePadding() : FrameBase(FrameType::kFramePadding) {}
    ~FramePadding() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;

public:
    uint16_t padding_length{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_PADDING_H__
