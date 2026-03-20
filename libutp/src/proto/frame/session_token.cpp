/*************************************************************************
    > File Name: session_token.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:49 AM CST
 ************************************************************************/

#include "proto/frame/session_token.h"

#include <cstring>
#include <limits>
#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameSessionToken::encode(void *buffer, size_t size) const
{
    if (token.size() > std::numeric_limits<uint8_t>::max()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "token size {} exceeds uint8_t max", token.size());
        return -1;
    }

    uint8_t encodeTokenSize = token_size;
    if (encodeTokenSize == 0) {
        encodeTokenSize = static_cast<uint8_t>(token.size());
    }

    if (token.size() != encodeTokenSize) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "token size mismatch: token_size={}, token.size={}",
                      encodeTokenSize,
                      token.size());
        return -1;
    }

    int32_t frameLen = FRAME_SESSION_TOKEN_HDR_SIZE + encodeTokenSize;
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than session token frame size {}", size, frameLen);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameSessionToken);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, encodeTokenSize);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, token_validity_period);

    if (encodeTokenSize > 0) {
        std::memcpy(bufferOffset, token.data(), encodeTokenSize);
    }

    return frameLen;
}

int32_t FrameSessionToken::decode(const void *buffer, size_t size)
{
    if (size < FRAME_SESSION_TOKEN_HDR_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than minimum session token frame size {}",
                      size,
                      FRAME_SESSION_TOKEN_HDR_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != FrameType::kFrameSessionToken) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED, "Invalid frame type: {}", static_cast<uint8_t>(frameType));
        return -1;
    }

    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, token_size);
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, token_validity_period);

    if (size < token_size) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "session token payload truncated: left={}, required={}",
                      size,
                      token_size);
        return -1;
    }

    token.resize(token_size);
    if (token_size > 0) {
        std::memcpy(token.data(), bufferOffset, token_size);
    }

    return FRAME_SESSION_TOKEN_HDR_SIZE + token_size;
}

int32_t FrameSessionToken::frameSize() const
{
    uint8_t size = token_size;
    if (size == 0 && !token.empty()) {
        size = static_cast<uint8_t>(token.size());
    }
    return FRAME_SESSION_TOKEN_HDR_SIZE + size;
}

} // namespace utp
} // namespace eular
