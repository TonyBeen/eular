/*************************************************************************
    > File Name: handshake_delay.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#include "proto/frame/handshake_delay.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameHandshakeDelay::encode(void *buffer, size_t size, Status &status) const
{
    if (size < FRAME_HANDSHAKE_DELAY_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW,
                               fmt::format("buffer size {} is smaller than handshake delay frame size {}",
                                           size,
                                           FRAME_HANDSHAKE_DELAY_SIZE));
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameHandshakeDelay);
    offset = Serialize::SerializeTo(offset, size, delay_time_us);
    if (offset == nullptr) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "encode handshake delay frame failed");
        return -1;
    }

    return FRAME_HANDSHAKE_DELAY_SIZE;
}

int32_t FrameHandshakeDelay::decode(const void *buffer, size_t size, Status &status)
{
    if (size < FRAME_HANDSHAKE_DELAY_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW,
                               fmt::format("buffer size {} is smaller than handshake delay frame size {}",
                                           size,
                                           FRAME_HANDSHAKE_DELAY_SIZE));
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameHandshakeDelay) {
        status = Status::Error(UTP_ERR_FRAME_UNEXPECTED,
                               fmt::format("Invalid frame type: {}",
                                           static_cast<uint8_t>(frameType)));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, delay_time_us);
    if (offset == nullptr) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "decode handshake delay frame failed");
        return -1;
    }

    return FRAME_HANDSHAKE_DELAY_SIZE;
}

int32_t FrameHandshakeDelay::frameSize() const
{
    return FRAME_HANDSHAKE_DELAY_SIZE;
}

} // namespace utp
} // namespace eular
