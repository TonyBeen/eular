/*************************************************************************
    > File Name: max_data.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#include "proto/frame/max_data.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameMaxData::encode(void *buffer, size_t size) const
{
    if (size < FRAME_MAX_DATA_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than max data frame size {}",
                      size,
                      FRAME_MAX_DATA_SIZE);
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameMaxData);
    offset = Serialize::SerializeTo(offset, size, maximum_data);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encode max data frame failed");
        return -1;
    }

    return FRAME_MAX_DATA_SIZE;
}

int32_t FrameMaxData::decode(const void *buffer, size_t size)
{
    if (size < FRAME_MAX_DATA_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than max data frame size {}",
                      size,
                      FRAME_MAX_DATA_SIZE);
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameMaxData) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "invalid frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, maximum_data);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "decode max data frame failed");
        return -1;
    }

    return FRAME_MAX_DATA_SIZE;
}

int32_t FrameMaxData::frameSize() const
{
    return FRAME_MAX_DATA_SIZE;
}

} // namespace utp
} // namespace eular
