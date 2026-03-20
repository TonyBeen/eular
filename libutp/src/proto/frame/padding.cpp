/*************************************************************************
    > File Name: padding.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:55:20 AM CST
 ************************************************************************/

#include "proto/frame/padding.h"

#include <cstring>
#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FramePadding::encode(void *buffer, size_t size) const
{
    int32_t frameLen = frameSize();
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than padding frame size {}", size, frameLen);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFramePadding);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, padding_length);
    if (padding_length > 0) {
        std::memset(bufferOffset, 0, padding_length);
    }

    return frameLen;
}

int32_t FramePadding::decode(const void *buffer, size_t size)
{
    if (size < FRAME_PADDING_HDR_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than minimum padding frame size {}",
                      size,
                      FRAME_PADDING_HDR_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != FrameType::kFramePadding) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED, "Invalid frame type: {}", static_cast<uint8_t>(frameType));
        return -1;
    }

    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, padding_length);
    if (size < padding_length) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "padding payload truncated: left={}, required={}",
                      size,
                      padding_length);
        return -1;
    }

    (void)bufferOffset;
    return FRAME_PADDING_HDR_SIZE + padding_length;
}

int32_t FramePadding::frameSize() const
{
    return FRAME_PADDING_HDR_SIZE + padding_length;
}

} // namespace utp
} // namespace eular
