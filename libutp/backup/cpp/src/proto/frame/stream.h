/*************************************************************************
    > File Name: stream.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:54:57 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_STREAM_H__
#define __UTP_PROTO_FRAME_STREAM_H__

#include "utp/config.h"
#include "proto/frame.h"

#define FRAME_STREAM_HDR_SIZE   (1 + 1 + 2 + 4 + 8) // type + stream_flag + stream_data_length + stream_id + stream_offset

namespace eular {
namespace utp {
struct FrameStream : public FrameBase {
public:
    FrameStream() : FrameBase(FrameType::kFrameStream) {}
    ~FrameStream() = default;

    int32_t encode(void *buffer, size_t size, Status &status) const;
    int32_t decode(const void *buffer, size_t size, Status &status);
    int32_t frameSize() const;

public:
    uint8_t     stream_flag{0};
    uint16_t    stream_data_length{0};
    uint32_t    stream_id{0};
    uint64_t    stream_offset{0};
    void*       stream_data{nullptr};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_STREAM_H__
