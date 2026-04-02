/*************************************************************************
    > File Name: handshake_done.h
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_HANDSHAKE_DONE_H__
#define __UTP_PROTO_FRAME_HANDSHAKE_DONE_H__

#include "proto/frame.h"

namespace eular {
namespace utp {

struct FrameHandshakeDone : public FrameBase {
public:
    FrameHandshakeDone() : FrameBase(FrameType::kFrameHandshakeDone) {}
    ~FrameHandshakeDone() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint64_t ack_handshake_pn{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_HANDSHAKE_DONE_H__
