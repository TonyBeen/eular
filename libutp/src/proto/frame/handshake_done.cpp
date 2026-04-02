/*************************************************************************
    > File Name: handshake_done.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#include "proto/frame/handshake_done.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameHandshakeDone::encode(void *buffer, size_t size) const
{
    if (size < FRAME_HANDSHAKE_DONE_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than handshake done frame size {}",
                      size,
                      FRAME_HANDSHAKE_DONE_SIZE);
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameHandshakeDone);
    offset = Serialize::SerializeTo(offset, size, ack_handshake_pn);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encode handshake done frame failed");
        return -1;
    }

    return FRAME_HANDSHAKE_DONE_SIZE;
}

int32_t FrameHandshakeDone::decode(const void *buffer, size_t size)
{
    if (size < FRAME_HANDSHAKE_DONE_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than handshake done frame size {}",
                      size,
                      FRAME_HANDSHAKE_DONE_SIZE);
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameHandshakeDone) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "Invalid frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, ack_handshake_pn);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "decode handshake done frame failed");
        return -1;
    }

    return FRAME_HANDSHAKE_DONE_SIZE;
}

int32_t FrameHandshakeDone::frameSize() const
{
    return FRAME_HANDSHAKE_DONE_SIZE;
}

} // namespace utp
} // namespace eular
