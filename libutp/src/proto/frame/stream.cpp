/*************************************************************************
    > File Name: stream.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:54:59 AM CST
 ************************************************************************/

#include "proto/frame/stream.h"

#include <cstring>
#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameStream::encode(void *buffer, size_t size) const
{
    int32_t frameLen = frameSize();
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than stream frame size {}", size, frameLen);
        return -1;
    }

    if (stream_data_length > 0 && stream_data == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "stream_data is null while stream_data_length={}", stream_data_length);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameStream);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, stream_flag);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, stream_data_length);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, stream_id);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, stream_offset);

    if (stream_data_length > 0) {
        std::memcpy(bufferOffset, stream_data, stream_data_length);
    }

    return frameLen;
}

int32_t FrameStream::decode(const void *buffer, size_t size)
{
    if (size < FRAME_STREAM_HDR_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than minimum stream frame size {}",
                      size,
                      FRAME_STREAM_HDR_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != FrameType::kFrameStream) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED, "Invalid frame type: {}", static_cast<uint8_t>(frameType));
        return -1;
    }

    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, stream_flag);
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, stream_data_length);
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, stream_id);
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, stream_offset);

    if (size < stream_data_length) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "stream payload truncated: left={}, required={}",
                      size,
                      stream_data_length);
        return -1;
    }

    stream_data = const_cast<uint8_t *>(bufferOffset);
    return FRAME_STREAM_HDR_SIZE + stream_data_length;
}

int32_t FrameStream::frameSize() const
{
    return FRAME_STREAM_HDR_SIZE + stream_data_length;
}

} // namespace utp
} // namespace eular
