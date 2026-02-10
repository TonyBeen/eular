/*************************************************************************
    > File Name: ack.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:54:48 AM CST
 ************************************************************************/

#include "proto/frame/ack.h"

#include <assert.h>
#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"

int32_t eular::utp::FrameAck::encode(void *buffer, size_t size) const
{
    int32_t ackFrameSize = frameSize();
    if (ackFrameSize > static_cast<int32_t>(size)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than ack frame size {}", size, ackFrameSize);
        return -1;
    }

    if (_history->empty()) { // no ack range to encode
        return 0;
    }

    uint32_t firstAckRange = 0;
    auto it = _history->begin();
    firstAckRange = it->high - it->low + 1; // +1 表示闭区间

    uint8_t rangeCount = rangeSize();
    uint8_t *bufferOffset = static_cast<uint8_t *>(buffer);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, FrameType::kFrameAck);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, rangeCount);
    utp_time_t delta = _now - _history->largestAckedReceived();
    uint16_t ackDelay = static_cast<uint16_t>(delta >> _params->ack_delay_exponent);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, ackDelay);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, firstAckRange);
    bufferOffset = Serialize::SerializeTo(bufferOffset, size, _history->largest());

    int32_t idx = 0;
    utp_packno_t lastPackNo = it->low;
    ++it;
    for (; it != _history->end() && idx < rangeCount; ++it, ++idx) {
        uint32_t gap = lastPackNo - it->high - 1;
        uint32_t ackRangeLength = it->high - it->low + 1;
        bufferOffset = Serialize::SerializeTo(bufferOffset, size, gap);
        bufferOffset = Serialize::SerializeTo(bufferOffset, size, ackRangeLength);
        lastPackNo = it->low;
    }

    return ackFrameSize;
}

int32_t eular::utp::FrameAck::decode(const void *buffer, size_t size)
{
    if (size < FRAME_ACK_HDR_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than minimum ack frame size {}", size, FRAME_ACK_HDR_SIZE);
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    FrameType frameType;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, frameType);
    if (frameType != FrameType::kFrameAck) {
        SetLastErrorV(UTP_ERR_FRAME_UNEXPECTED, "Invalid frame type: {}", static_cast<uint8_t>(frameType));
        return -1;
    }

    size_t sizeOriginal = size;
    uint8_t rangeCount;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, rangeCount);
    _ackInfo->range_size = rangeCount;

    uint16_t ackDelay;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, ackDelay);
    _ackInfo->ack_delay = ackDelay << _params->ack_delay_exponent;

    uint32_t firstAckRange;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, firstAckRange);

    utp_packno_t largestAcked;
    bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, largestAcked);

    uint32_t i = 0;
    _ackInfo->ack_ranges[i].high = largestAcked;
    _ackInfo->ack_ranges[i].low = largestAcked - firstAckRange + 1;
    ++i;
    utp_packno_t lastAcked = _ackInfo->ack_ranges[0].low;
    for (; i < rangeCount; ++i) {
        uint32_t gap;
        bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, gap);
        uint32_t ackRangeLength;
        bufferOffset = Serialize::DeserializeFrom(bufferOffset, size, ackRangeLength);
        _ackInfo->ack_ranges[i].high = lastAcked - gap - 1;
        _ackInfo->ack_ranges[i].low = _ackInfo->ack_ranges[i].high - ackRangeLength + 1;
        lastAcked = _ackInfo->ack_ranges[i].low;
    }

    assert((sizeOriginal - size) == (rangeCount * FRAME_ACK_RANGE_SIZE + FRAME_ACK_HDR_SIZE));
    return sizeOriginal - size;
}

int32_t eular::utp::FrameAck::frameSize() const
{
    uint32_t rangeCount = _history->rangeCount() > _config->max_ack_range_size ?
                          _config->max_ack_range_size : _history->rangeCount();
    uint32_t size = rangeCount * FRAME_ACK_RANGE_SIZE;
    return static_cast<int32_t>(FRAME_ACK_HDR_SIZE + size);
}

int32_t eular::utp::FrameAck::rangeSize() const
{
    uint32_t rangeCount = _history->rangeCount() > _config->max_ack_range_size ?
                          _config->max_ack_range_size : _history->rangeCount();
    return static_cast<int32_t>(rangeCount);
}
