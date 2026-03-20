/*************************************************************************
    > File Name: connection_close.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:55:43 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_CONNECTION_CLOSE_H__
#define __UTP_PROTO_FRAME_CONNECTION_CLOSE_H__

#include <string>

#include "proto/frame.h"

#define FRAME_CONNECTION_CLOSE_HDR_SIZE    (1 + 2 + 2) // type + error_code + reason_length

namespace eular {
namespace utp {

struct FrameConnectionClose : public FrameBase {
public:
    FrameConnectionClose() : FrameBase(FrameType::kFrameConnectionClose) {}
    ~FrameConnectionClose() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint16_t    error_code{0};
    uint16_t    reason_length{0};
    std::string reason_phrase;
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_CONNECTION_CLOSE_H__
