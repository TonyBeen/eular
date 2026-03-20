/*************************************************************************
    > File Name: path.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:22 AM CST
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_PATH_H__
#define __UTP_PROTO_FRAME_PATH_H__

#include <array>

#include "proto/frame.h"

#define FRAME_PATH_DATA_SIZE      (8)                     // challenge data size
#define FRAME_PATH_HDR_SIZE       (1)                     // type
#define FRAME_PATH_FRAME_SIZE     (FRAME_PATH_HDR_SIZE + FRAME_PATH_DATA_SIZE)

namespace eular {
namespace utp {

struct FramePathChallenge : public FrameBase {
public:
    FramePathChallenge() : FrameBase(FrameType::kFramePathChallenge) {}
    ~FramePathChallenge() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);

public:
    std::array<uint8_t, FRAME_PATH_DATA_SIZE> data{};
};

struct FramePathResponse : public FrameBase {
public:
    FramePathResponse() : FrameBase(FrameType::kFramePathResponse) {}
    ~FramePathResponse() = default;

    int32_t encode(void *buffer, size_t size) const;
    int32_t decode(const void *buffer, size_t size);

public:
    std::array<uint8_t, FRAME_PATH_DATA_SIZE> data{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_PATH_H__
