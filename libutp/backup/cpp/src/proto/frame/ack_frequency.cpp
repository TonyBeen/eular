/*************************************************************************
    > File Name: ack_frequency.cpp
    > Author: eular
    > Brief:
    > Created Time: Fri 20 Mar 2026
 ************************************************************************/

#include "proto/frame/ack_frequency.h"

#include <algorithm>

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

constexpr uint8_t FrameAckFrequency::kDefaultAckElicitingThreshold;
constexpr uint8_t FrameAckFrequency::kDefaultReorderingThreshold;
constexpr uint32_t FrameAckFrequency::kDefaultMaxAckDelayMs;
constexpr uint8_t FrameAckFrequency::kMaxAckElicitingThreshold;
constexpr uint8_t FrameAckFrequency::kMaxReorderingThreshold;
constexpr uint32_t FrameAckFrequency::kMaxAckDelayMsClamp;

int32_t FrameAckFrequency::encode(void *buffer, size_t size, Status &status) const
{
    FrameAckFrequency normalized = *this;
    normalized.normalize();

    if (size < FRAME_ACK_FREQUENCY_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("buffer size {} is smaller than ack frequency frame size {}",
            size, FRAME_ACK_FREQUENCY_SIZE));
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameAckFrequency);
    offset = Serialize::SerializeTo(offset, size, normalized.ack_eliciting_threshold);
    offset = Serialize::SerializeTo(offset, size, normalized.reordering_threshold);
    offset = Serialize::SerializeTo(offset, size, normalized.max_ack_delay_ms);
    if (offset == nullptr) {
        status = Status::Error(UTP_ERR_OVERFLOW, "encode ack frequency frame failed");
        return -1;
    }

    return FRAME_ACK_FREQUENCY_SIZE;
}

int32_t FrameAckFrequency::decode(const void *buffer, size_t size, Status &status)
{
    if (size < FRAME_ACK_FREQUENCY_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("buffer size {} is smaller than ack frequency frame size {}",
            size, FRAME_ACK_FREQUENCY_SIZE));
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameAckFrequency) {
        status = Status::Error(UTP_ERR_FRAME_UNEXPECTED, fmt::format("invalid frame type: {}",
            static_cast<uint8_t>(frameType)));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, ack_eliciting_threshold);
    offset = Serialize::DeserializeFrom(offset, size, reordering_threshold);
    offset = Serialize::DeserializeFrom(offset, size, max_ack_delay_ms);
    if (offset == nullptr) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "decode ack frequency frame failed");
        return -1;
    }

    normalize();

    return FRAME_ACK_FREQUENCY_SIZE;
}

int32_t FrameAckFrequency::frameSize() const
{
    return FRAME_ACK_FREQUENCY_SIZE;
}

void FrameAckFrequency::normalize()
{
    if (ack_eliciting_threshold == 0) {
        ack_eliciting_threshold = kDefaultAckElicitingThreshold;
    } else {
        ack_eliciting_threshold = std::min<uint8_t>(ack_eliciting_threshold, kMaxAckElicitingThreshold);
    }

    if (reordering_threshold == 0) {
        reordering_threshold = kDefaultReorderingThreshold;
    } else {
        reordering_threshold = std::min<uint8_t>(reordering_threshold, kMaxReorderingThreshold);
    }

    if (max_ack_delay_ms == 0) {
        max_ack_delay_ms = kDefaultMaxAckDelayMs;
    } else {
        max_ack_delay_ms = std::min<uint32_t>(max_ack_delay_ms, kMaxAckDelayMsClamp);
    }
}

} // namespace utp
} // namespace eular