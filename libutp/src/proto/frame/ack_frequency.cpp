/*************************************************************************
    > File Name: ack_frequency.cpp
    > Author: eular
    > Brief:
    > Created Time: Fri 20 Mar 2026
 ************************************************************************/

#include "proto/frame/ack_frequency.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameAckFrequency::encode(void *buffer, size_t size) const
{
    if (size < FRAME_ACK_FREQUENCY_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than ack frequency frame size {}",
                      size,
                      FRAME_ACK_FREQUENCY_SIZE);
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameAckFrequency);
    offset = Serialize::SerializeTo(offset, size, ack_eliciting_threshold);
    offset = Serialize::SerializeTo(offset, size, reordering_threshold);
    offset = Serialize::SerializeTo(offset, size, max_ack_delay_ms);
    offset = Serialize::SerializeTo(offset, size, timestamp);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encode ack frequency frame failed");
        return -1;
    }

    return FRAME_ACK_FREQUENCY_SIZE;
}

int32_t FrameAckFrequency::decode(const void *buffer, size_t size)
{
    if (size < FRAME_ACK_FREQUENCY_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than ack frequency frame size {}",
                      size,
                      FRAME_ACK_FREQUENCY_SIZE);
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameAckFrequency) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "Invalid frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, ack_eliciting_threshold);
    offset = Serialize::DeserializeFrom(offset, size, reordering_threshold);
    offset = Serialize::DeserializeFrom(offset, size, max_ack_delay_ms);
    offset = Serialize::DeserializeFrom(offset, size, timestamp);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "decode ack frequency frame failed");
        return -1;
    }

    return FRAME_ACK_FREQUENCY_SIZE;
}

int32_t FrameAckFrequency::frameSize() const
{
    return FRAME_ACK_FREQUENCY_SIZE;
}

} // namespace utp
} // namespace eular