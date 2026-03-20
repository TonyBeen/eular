/*************************************************************************
    > File Name: version.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:57:00 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_VERSION_H__
#define __UTP_PROTO_FRAME_VERSION_H__

#include "proto/frame.h"

#define FRAME_VERSION_SIZE    (1 + 4) // type + version

namespace eular {
namespace utp {

struct FrameVersion : public FrameBase {
public:
    FrameVersion() : FrameBase(FrameType::kFrameVersion) {}
    ~FrameVersion() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);
    int32_t frameSize() const;

public:
    uint32_t version{0};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_VERSION_H__
