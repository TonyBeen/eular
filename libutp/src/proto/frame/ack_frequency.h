/*************************************************************************
    > File Name: ack_frequency.h
    > Author: eular
    > Brief:
    > Created Time: Fri 20 Mar 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_ACK_FREQUENCY_H__
#define __UTP_PROTO_FRAME_ACK_FREQUENCY_H__

#include "proto/frame.h"

#define FRAME_ACK_FREQUENCY_SIZE (1 + 1 + 1 + 4 + 8)

namespace eular {
namespace utp {

struct FrameAckFrequency : public FrameBase {
public:
    FrameAckFrequency() : FrameBase(FrameType::kFrameAckFrequency) {}
    ~FrameAckFrequency() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint8_t     ack_eliciting_threshold{10};
    uint8_t     reordering_threshold{3};
    uint32_t    max_ack_delay_ms{150};
    uint64_t    timestamp{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_ACK_FREQUENCY_H__