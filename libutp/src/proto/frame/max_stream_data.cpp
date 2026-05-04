/*************************************************************************
    > File Name: max_stream_data.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#include "proto/frame/max_stream_data.h"

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

namespace eular {
namespace utp {

int32_t FrameMaxStreamData::encode(void *buffer, size_t size) const
{
    if (size < FRAME_MAX_STREAM_DATA_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than max stream data frame size {}",
                      size,
                      FRAME_MAX_STREAM_DATA_SIZE);
        return -1;
    }

    uint8_t *offset = static_cast<uint8_t *>(buffer);
    offset = Serialize::SerializeTo(offset, size, FrameType::kFrameMaxStreamData);
    offset = Serialize::SerializeTo(offset, size, stream_id);
    offset = Serialize::SerializeTo(offset, size, maximum_stream_data);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "encode max stream data frame failed");
        return -1;
    }

    return FRAME_MAX_STREAM_DATA_SIZE;
}

int32_t FrameMaxStreamData::decode(const void *buffer, size_t size)
{
    if (size < FRAME_MAX_STREAM_DATA_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "buffer size {} is smaller than max stream data frame size {}",
                      size,
                      FRAME_MAX_STREAM_DATA_SIZE);
        return -1;
    }

    const uint8_t *offset = static_cast<const uint8_t *>(buffer);
    FrameType frameType = FrameType::kFrameInvalid;
    offset = Serialize::DeserializeFrom(offset, size, frameType);
    if (offset == nullptr || frameType != FrameType::kFrameMaxStreamData) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED,
                      "invalid frame type: {}",
                      static_cast<uint8_t>(frameType));
        return -1;
    }

    offset = Serialize::DeserializeFrom(offset, size, stream_id);
    offset = Serialize::DeserializeFrom(offset, size, maximum_stream_data);
    if (offset == nullptr) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "decode max stream data frame failed");
        return -1;
    }

    return FRAME_MAX_STREAM_DATA_SIZE;
}

int32_t FrameMaxStreamData::frameSize() const
{
    return FRAME_MAX_STREAM_DATA_SIZE;
}

} // namespace utp
} // namespace eular
