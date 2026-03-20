/*************************************************************************
    > File Name: connection_close.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:55:45 AM CST
 ************************************************************************/

#include "proto/frame/connection_close.h"

#include <cstring>
#include <limits>
#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameConnectionClose::encode(void *buffer, size_t size) const
{
    if (reason_phrase.size() > std::numeric_limits<uint16_t>::max()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "reason phrase size {} exceeds uint16_t max",
                      reason_phrase.size());
        return -1;
    }

    uint16_t encodeReasonLength = reason_length;
    if (encodeReasonLength == 0) {
        encodeReasonLength = static_cast<uint16_t>(reason_phrase.size());
    }

    if (reason_phrase.size() != encodeReasonLength) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "reason length mismatch: reason_length={}, reason_phrase.size={}",
                      encodeReasonLength,
                      reason_phrase.size());
        return -1;
    }

    int32_t frameLen = FRAME_CONNECTION_CLOSE_HDR_SIZE + encodeReasonLength;
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than connection close frame size {}", size, frameLen);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameConnectionClose);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, error_code);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, encodeReasonLength);

    if (encodeReasonLength > 0) {
        std::memcpy(bufferOffset, reason_phrase.data(), encodeReasonLength);
    }

    return frameLen;
}

int32_t FrameConnectionClose::decode(const void *buffer, size_t size)
{
    if (size < FRAME_CONNECTION_CLOSE_HDR_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than minimum connection close frame size {}",
                      size,
                      FRAME_CONNECTION_CLOSE_HDR_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != FrameType::kFrameConnectionClose) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED, "Invalid frame type: {}", static_cast<uint8_t>(frameType));
        return -1;
    }

    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, error_code);
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, reason_length);

    if (size < reason_length) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "connection close reason truncated: left={}, required={}",
                      size,
                      reason_length);
        return -1;
    }

    reason_phrase.assign(reinterpret_cast<const char *>(bufferOffset), reason_length);
    return FRAME_CONNECTION_CLOSE_HDR_SIZE + reason_length;
}

int32_t FrameConnectionClose::frameSize() const
{
    uint16_t size = reason_length;
    if (size == 0 && !reason_phrase.empty()) {
        size = static_cast<uint16_t>(reason_phrase.size());
    }
    return FRAME_CONNECTION_CLOSE_HDR_SIZE + size;
}

} // namespace utp
} // namespace eular
