/*************************************************************************
    > File Name: stream_data_blocked.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#include "proto/frame/stream_data_blocked.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameStreamDataBlocked::encode(void *buffer, size_t size) const
{
    if (size < FRAME_STREAM_DATA_BLOCKED_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than stream data blocked frame size {}",
                      size,
                      FRAME_STREAM_DATA_BLOCKED_SIZE);
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameStreamDataBlocked);
    offset = Serialize::SerializeTo(offset, size, stream_id);
    offset = Serialize::SerializeTo(offset, size, stream_data_limit);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encode stream data blocked frame failed");
        return -1;
    }

    return FRAME_STREAM_DATA_BLOCKED_SIZE;
}

int32_t FrameStreamDataBlocked::decode(const void *buffer, size_t size)
{
    if (size < FRAME_STREAM_DATA_BLOCKED_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than stream data blocked frame size {}",
                      size,
                      FRAME_STREAM_DATA_BLOCKED_SIZE);
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameStreamDataBlocked) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "invalid frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, stream_id);
    offset = Serialize::DeserializeFrom(offset, size, stream_data_limit);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "decode stream data blocked frame failed");
        return -1;
    }

    return FRAME_STREAM_DATA_BLOCKED_SIZE;
}

int32_t FrameStreamDataBlocked::frameSize() const
{
    return FRAME_STREAM_DATA_BLOCKED_SIZE;
}

} // namespace utp
} // namespace eular
