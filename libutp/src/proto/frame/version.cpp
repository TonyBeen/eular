/*************************************************************************
    > File Name: version.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:57:02 AM CST
 ************************************************************************/

#include "proto/frame/version.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameVersion::encode(void *buffer, size_t size) const
{
    if (size < FRAME_VERSION_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than version frame size {}", size, FRAME_VERSION_SIZE);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameVersion);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, version);
    (void)bufferOffset;
    return FRAME_VERSION_SIZE;
}

int32_t FrameVersion::decode(const void *buffer, size_t size)
{
    if (size < FRAME_VERSION_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than version frame size {}", size, FRAME_VERSION_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != FrameType::kFrameVersion) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED, "Invalid frame type: {}", static_cast<uint8_t>(frameType));
        return -1;
    }

    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, version);
    (void)bufferOffset;
    return FRAME_VERSION_SIZE;
}

int32_t FrameVersion::frameSize() const
{
    return FRAME_VERSION_SIZE;
}

} // namespace utp
} // namespace eular
