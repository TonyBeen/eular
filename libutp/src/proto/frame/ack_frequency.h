/*************************************************************************
    > File Name: ack_frequency.h
    > Author: eular
    > Brief:
    > Created Time: Fri 20 Mar 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_ACK_FREQUENCY_H__
#define __UTP_PROTO_FRAME_ACK_FREQUENCY_H__

#include "proto/proto.h"
#include "proto/frame.h"

#define FRAME_ACK_FREQUENCY_SIZE (1 + 1 + 1 + 4)

namespace eular {
namespace utp {

class FrameAckFrequency : public FrameBase {
public:
    static constexpr uint8_t    kDefaultAckElicitingThreshold = UTP_DEFAULT_ACK_THRESHOLD;
    static constexpr uint8_t    kDefaultReorderingThreshold = UTP_DEFAULT_REORDER_THRESHOLD;
    static constexpr uint32_t   kDefaultMaxAckDelayMs = UTP_DEFAULT_MAX_ACK_DELAY_MS;
    static constexpr uint8_t    kMaxAckElicitingThreshold = 64;
    static constexpr uint8_t    kMaxReorderingThreshold = 32;
    static constexpr uint32_t   kMaxAckDelayMsClamp = 1000;

    FrameAckFrequency() : FrameBase(FrameType::kFrameAckFrequency) {}
    ~FrameAckFrequency() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;
    void    normalize();

public:
    uint8_t     ack_eliciting_threshold{10};
    uint8_t     reordering_threshold{3};
    uint32_t    max_ack_delay_ms{150};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_ACK_FREQUENCY_H__