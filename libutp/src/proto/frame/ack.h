/*************************************************************************
    > File Name: ack.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:54:46 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_ACK_H__
#define __UTP_PROTO_FRAME_ACK_H__

#include "utp/config.h"
#include "proto/frame.h"
#include "util/ack_info.h"
#include "util/receive_history.h"

#define FRAME_ACK_HDR_SIZE      (1 + 1 + 2 + 4 + 8) // type + ack_count + ack_delay + first_ack_range + ack_largest
#define FRAME_ACK_RANGE_SIZE    (8)                 // ack_range size

namespace eular {
namespace utp {
struct FrameAck : public FrameBase {
public:
    FrameAck() : FrameBase(FrameType::kFrameAck) {}
    ~FrameAck() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    const ReceiveHistory*   _history{nullptr};
    const AckInfo*          _ackInfo{nullptr};
    utp_time_t              _now{0};
    Config*                 _config{nullptr};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_ACK_H__
