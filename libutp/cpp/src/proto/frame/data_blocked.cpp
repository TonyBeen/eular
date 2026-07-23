/*************************************************************************
    > File Name: data_blocked.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#include "proto/frame/data_blocked.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameDataBlocked::encode(void *buffer, size_t size, Status &status) const
{
    if (size < FRAME_DATA_BLOCKED_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW,
                               fmt::format("buffer size {} is smaller than data blocked frame size {}",
                                           size,
                                           FRAME_DATA_BLOCKED_SIZE));
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameDataBlocked);
    offset = Serialize::SerializeTo(offset, size, data_limit);
    if (offset == nullptr) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "encode data blocked frame failed");
        return -1;
    }

    return FRAME_DATA_BLOCKED_SIZE;
}

int32_t FrameDataBlocked::decode(const void *buffer, size_t size, Status &status)
{
    if (size < FRAME_DATA_BLOCKED_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW,
                               fmt::format("buffer size {} is smaller than data blocked frame size {}",
                                           size,
                                           FRAME_DATA_BLOCKED_SIZE));
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameDataBlocked) {
        status = Status::Error(UTP_ERR_FRAME_UNEXPECTED,
                               fmt::format("invalid frame type: {}",
                                           static_cast<uint8_t>(frameType)));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, data_limit);
    if (offset == nullptr) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "decode data blocked frame failed");
        return -1;
    }

    return FRAME_DATA_BLOCKED_SIZE;
}

int32_t FrameDataBlocked::frameSize() const
{
    return FRAME_DATA_BLOCKED_SIZE;
}

} // namespace utp
} // namespace eular
