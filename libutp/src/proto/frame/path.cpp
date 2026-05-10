/*************************************************************************
    > File Name: path.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:56:25 AM CST
 ************************************************************************/

#include "proto/frame/path.h"

#include <cstring>
#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace {
using eular::utp::FramePathChallenge;
using eular::utp::FramePathResponse;
using eular::utp::FrameType;
using eular::utp::Status;

template <typename FrameT>
int32_t EncodePathFrame(const FrameT &frame, FrameType expectedType, void *buffer, size_t size, Status &status)
{
    if (size < FRAME_PATH_FRAME_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("buffer size {} is smaller than path frame size {}", size, FRAME_PATH_FRAME_SIZE));
        return -1;
    }

    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = eular::Serialize::SerializeTo(bufferOffset, size, expectedType);
    std::memcpy(bufferOffset, frame.data.data(), FRAME_PATH_DATA_SIZE);
    return FRAME_PATH_FRAME_SIZE;
}

template <typename FrameT>
int32_t DecodePathFrame(FrameT &frame, FrameType expectedType, const void *buffer, size_t size, Status &status)
{
    if (size < FRAME_PATH_FRAME_SIZE) {
        status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("buffer size {} is smaller than path frame size {}", size, FRAME_PATH_FRAME_SIZE));
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = eular::Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != expectedType) {
        status = Status::Error(UTP_ERR_FRAME_UNEXPECTED, fmt::format("Invalid frame type: {}", static_cast<uint8_t>(frameType)));
        return -1;
    }

    std::memcpy(frame.data.data(), bufferOffset, FRAME_PATH_DATA_SIZE);
    return FRAME_PATH_FRAME_SIZE;
}

} // namespace

namespace eular {
namespace utp {

int32_t FramePathChallenge::encode(void *buffer, size_t size, Status &status) const
{
    return EncodePathFrame(*this, FrameType::kFramePathChallenge, buffer, size, status);
}

int32_t FramePathChallenge::decode(const void *buffer, size_t size, Status &status)
{
    return DecodePathFrame(*this, FrameType::kFramePathChallenge, buffer, size, status);
}

int32_t FramePathResponse::encode(void *buffer, size_t size, Status &status) const
{
    return EncodePathFrame(*this, FrameType::kFramePathResponse, buffer, size, status);
}

int32_t FramePathResponse::decode(const void *buffer, size_t size, Status &status)
{
    return DecodePathFrame(*this, FrameType::kFramePathResponse, buffer, size, status);
}

} // namespace utp
} // namespace eular
