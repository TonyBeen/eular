/*************************************************************************
    > File Name: reset_stream.h
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_RESET_STREAM_H__
#define __UTP_PROTO_FRAME_RESET_STREAM_H__

#include "proto/frame.h"

#define FRAME_RESET_STREAM_SIZE   (1 + 2 + 4 + 8) // type + error_code + stream_id + final_size

namespace eular {
namespace utp {

struct FrameResetStream : public FrameBase {
public:
    FrameResetStream() : FrameBase(FrameType::kFrameResetStream) {}
    ~FrameResetStream() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;

public:
    uint16_t    error_code{0};
    uint32_t    stream_id{0};
    uint64_t    final_size{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_RESET_STREAM_H__
