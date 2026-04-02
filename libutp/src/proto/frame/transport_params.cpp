/*************************************************************************
    > File Name: transport_params.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 01 Apr 2026
 ************************************************************************/

#include "proto/frame/transport_params.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameTransportParams::encode(void *buffer, size_t size) const
{
    if (buffer == nullptr || params == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "invalid transport params frame encode args: buffer={}, params={}",
                      buffer != nullptr,
                      params != nullptr);
        return -1;
    }

    const int32_t frameLen = frameSize();
    if (size < static_cast<size_t>(frameLen)) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than transport params frame size {}",
                      size,
                      frameLen);
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameTransportParams);
    offset = Serialize::SerializeTo(offset, size, params->flags);
    offset = Serialize::SerializeTo(offset, size, params->max_idle_timeout);
    offset = Serialize::SerializeTo(offset, size, params->handshake_timeout);
    offset = Serialize::SerializeTo(offset, size, params->init_max_streams_bidi);
    offset = Serialize::SerializeTo(offset, size, params->init_max_streams_uni);
    offset = Serialize::SerializeTo(offset, size, params->ack_delay_exponent);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "failed to encode transport params frame");
        return -1;
    }

    return frameLen;
}

int32_t FrameTransportParams::decode(const void *buffer, size_t size)
{
    if (buffer == nullptr || params == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "invalid transport params frame decode args: buffer={}, params={}",
                      buffer != nullptr,
                      params != nullptr);
        return -1;
    }

    if (size < static_cast<size_t>(frameSize())) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than transport params frame size {}",
                      size,
                      frameSize());
        return -1;
    }

    const size_t originalSize = size;
    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameTransportParams) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "invalid transport params frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, params->flags);
    offset = Serialize::DeserializeFrom(offset, size, params->max_idle_timeout);
    offset = Serialize::DeserializeFrom(offset, size, params->handshake_timeout);
    offset = Serialize::DeserializeFrom(offset, size, params->init_max_streams_bidi);
    offset = Serialize::DeserializeFrom(offset, size, params->init_max_streams_uni);
    offset = Serialize::DeserializeFrom(offset, size, params->ack_delay_exponent);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "failed to decode transport params frame");
        return -1;
    }

    return static_cast<int32_t>(originalSize - size);
}

int32_t FrameTransportParams::frameSize() const
{
    return FRAME_TRANSPORT_PARAMS_SIZE;
}

} // namespace utp
} // namespace eular