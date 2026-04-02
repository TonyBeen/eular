/*************************************************************************
    > File Name: crypto.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:39 AM CST
 ************************************************************************/

#include "proto/frame/crypto.h"

#include <cstring>

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameCrypto::encode(void *buffer, size_t size) const
{
    if (buffer == nullptr || eph_pubkey == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "invalid crypto frame encode args: buffer={}, eph_pubkey={}",
                      buffer != nullptr,
                      eph_pubkey != nullptr);
        return -1;
    }

    const int32_t frameLen = frameSize();
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than crypto frame size {}", size, frameLen);
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameCrypto);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, crypto_type);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, static_cast<uint8_t>(0));

    if (bufferOffset == nullptr || size < FRAME_CRYPTO_EPH_PUBKEY_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "failed to encode crypto frame public key, left={} bytes",
                      size);
        return -1;
    }

    std::memcpy(bufferOffset, eph_pubkey, FRAME_CRYPTO_EPH_PUBKEY_SIZE);
    return frameLen;
}

int32_t FrameCrypto::decode(const void *buffer, size_t size)
{
    if (buffer == nullptr || eph_pubkey == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "invalid crypto frame decode args: buffer={}, eph_pubkey={}",
                      buffer != nullptr,
                      eph_pubkey != nullptr);
        return -1;
    }

    if (size < FRAME_CRYPTO_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than crypto frame size {}",
                      size,
                      FRAME_CRYPTO_SIZE);
        return -1;
    }

    const size_t originalSize = size;
    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    auto deserialize = [&] (auto &value) -> bool {
        bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, value);
        return bufferOffset != nullptr;
    };

    FrameType frameType = FrameType::kFrameInvalid;
    if (!deserialize(frameType) || frameType != FrameType::kFrameCrypto) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "invalid crypto frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    uint8_t reserved = 0;
    if (!deserialize(crypto_type) || !deserialize(reserved)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "failed to decode crypto frame header");
        return -1;
    }

    if (size < FRAME_CRYPTO_EPH_PUBKEY_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "crypto frame truncated before peer public key: left={}, required={}",
                      size,
                      FRAME_CRYPTO_EPH_PUBKEY_SIZE);
        return -1;
    }

    std::memcpy(eph_pubkey, bufferOffset, FRAME_CRYPTO_EPH_PUBKEY_SIZE);
    return static_cast<int32_t>(originalSize - size + FRAME_CRYPTO_EPH_PUBKEY_SIZE);
}

int32_t FrameCrypto::frameSize() const
{
    return FRAME_CRYPTO_SIZE;
}

} // namespace utp
} // namespace eular

