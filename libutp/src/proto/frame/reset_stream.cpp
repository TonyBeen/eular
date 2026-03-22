/*************************************************************************
    > File Name: reset_stream.cpp
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include "proto/frame/reset_stream.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameResetStream::encode(void *buffer, size_t size) const
{
    const int32_t frameLen = frameSize();
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than reset stream frame size {}",
                      size,
                      frameLen);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameResetStream);
    if (bufferOffset == nullptr) return -1;
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, error_code);
    if (bufferOffset == nullptr) return -1;
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, stream_id);
    if (bufferOffset == nullptr) return -1;
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, final_size);
    if (bufferOffset == nullptr) return -1;

    return frameLen;
}

int32_t FrameResetStream::decode(const void *buffer, size_t size)
{
    if (size < FRAME_RESET_STREAM_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than reset stream frame size {}",
                      size,
                      FRAME_RESET_STREAM_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (bufferOffset == nullptr || frameType != FrameType::kFrameResetStream) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "invalid frame type for reset stream: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, error_code);
    if (bufferOffset == nullptr) return -1;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, stream_id);
    if (bufferOffset == nullptr) return -1;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, final_size);
    if (bufferOffset == nullptr) return -1;

    return FRAME_RESET_STREAM_SIZE;
}

int32_t FrameResetStream::frameSize() const
{
    return FRAME_RESET_STREAM_SIZE;
}

} // namespace utp
} // namespace eular
