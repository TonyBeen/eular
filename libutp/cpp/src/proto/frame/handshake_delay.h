/*************************************************************************
    > File Name: handshake_delay.h
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_HANDSHAKE_DELAY_H__
#define __UTP_PROTO_FRAME_HANDSHAKE_DELAY_H__

#include "proto/frame.h"

namespace eular {
namespace utp {

struct FrameHandshakeDelay : public FrameBase {
public:
    FrameHandshakeDelay() : FrameBase(FrameType::kFrameHandshakeDelay) {}
    ~FrameHandshakeDelay() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;

public:
    uint32_t delay_time_us{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_HANDSHAKE_DELAY_H__
