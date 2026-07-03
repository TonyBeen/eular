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

namespace {
template <typename T>
inline bool DeserializeHelper(const uint8_t *&offset, size_t &sz, T &value) {
    offset = eular::Serialize::DeserializeFrom(offset, sz, value);
    return offset != nullptr;
}
}

int32_t eular::utp::FrameAck::encode(void *buffer, size_t size, Status &status) const
{
    int32_t ackFrameSize = frameSize();
    if (ackFrameSize > static_cast<int32_t>(size)) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "buffer too small for ack frame");
        return -1;
    }

    if (_history->empty()) { // no ack range to encode
        return 0;
    }

    uint32_t firstAckRange = 0;
    auto it = _history->begin();
    firstAckRange = it->high - it->low + 1; // +1 表示闭区间
    if (firstAckRange == 0) {
        status = Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid first ack range");
        return -1;
    }

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

int32_t eular::utp::FrameAck::decode(const void *buffer, size_t size, Status &status)
{
    if (buffer == nullptr || _ackInfo == nullptr || _params == nullptr) {
        status = Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid ack decode args");
        return -1;
    }

    if (size < FRAME_ACK_HDR_SIZE) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "buffer too small to parse ack frame");
        return -1;
    }

    const uint8_t *bufferOffset = static_cast<const uint8_t *>(buffer);
    const size_t sizeOriginal = size;

    FrameType frameType;
    if (!DeserializeHelper(bufferOffset, size,frameType)) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "failed to decode ack frame type");
        return -1;
    }
    if (frameType != FrameType::kFrameAck) {
        status = Status::Error(UTP_ERR_FRAME_UNEXPECTED, fmt::format("Invalid frame type: {}", static_cast<uint8_t>(frameType)));
        return -1;
    }

    uint8_t rangeCount;
    if (!DeserializeHelper(bufferOffset, size,rangeCount)) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "failed to decode ack range count");
        return -1;
    }

    // range_count 表示 additional ACK ranges 的数量（不含 first_ack_range）
    if (rangeCount >= _ackInfo->ack_ranges.size()) {
        status = Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("invalid ack range count {}", rangeCount));
        return -1;
    }

    const size_t expectedSize = FRAME_ACK_HDR_SIZE + static_cast<size_t>(rangeCount) * FRAME_ACK_RANGE_SIZE;
    if (sizeOriginal < expectedSize) {
        status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("ack frame too short: size={}, expected={}", sizeOriginal, expectedSize));
        return -1;
    }

    _ackInfo->range_size = static_cast<uint32_t>(rangeCount) + 1;

    uint16_t ackDelay;
    if (!DeserializeHelper(bufferOffset, size,ackDelay)) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "failed to decode ack delay");
        return -1;
    }
    _ackInfo->ack_delay = ackDelay << _params->ack_delay_exponent;

    uint32_t firstAckRange;
    if (!DeserializeHelper(bufferOffset, size,firstAckRange)) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "failed to decode first ack range");
        return -1;
    }

    if (firstAckRange == 0) {
        status = Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid first ack range 0");
        return -1;
    }

    utp_packno_t largestAcked;
    if (!DeserializeHelper(bufferOffset, size,largestAcked)) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "failed to decode largest acked packno");
        return -1;
    }

    if (largestAcked < static_cast<utp_packno_t>(firstAckRange - 1)) {
        status = Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("invalid first ack range {} for largest packno {}",
            firstAckRange, largestAcked));
        return -1;
    }

    _ackInfo->largest_ack_packno = largestAcked;

    uint32_t i = 0;
    _ackInfo->ack_ranges[i].high = largestAcked;
    _ackInfo->ack_ranges[i].low = largestAcked - firstAckRange + 1;
    ++i;
    utp_packno_t lastAcked = _ackInfo->ack_ranges[0].low;
    for (; i <= rangeCount; ++i) {
        uint32_t gap;
        if (!DeserializeHelper(bufferOffset, size,gap)) {
            status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("failed to decode ack gap at index {}", i));
            return -1;
        }

        if (lastAcked <= static_cast<utp_packno_t>(gap)) {
            status = Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("invalid ack gap {} at index {}", gap, i));
            return -1;
        }

        uint32_t ackRangeLength;
        if (!DeserializeHelper(bufferOffset, size,ackRangeLength)) {
            status = Status::Error(UTP_ERR_OVERFLOW, fmt::format("failed to decode ack range length at index {}", i));
            return -1;
        }

        if (ackRangeLength == 0) {
            status = Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("invalid ack range length 0 at index {}", i));
            return -1;
        }

        _ackInfo->ack_ranges[i].high = lastAcked - gap - 1;
        if (_ackInfo->ack_ranges[i].high + 1 < ackRangeLength) {
            status = Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("invalid ack range length {} at index {}", ackRangeLength, i));
            return -1;
        }

        _ackInfo->ack_ranges[i].low = _ackInfo->ack_ranges[i].high - ackRangeLength + 1;
        lastAcked = _ackInfo->ack_ranges[i].low;
    }

    assert((sizeOriginal - size) == (rangeCount * FRAME_ACK_RANGE_SIZE + FRAME_ACK_HDR_SIZE));
    return sizeOriginal - size;
}

int32_t eular::utp::FrameAck::frameSize() const
{
    uint32_t rangeCount = rangeSize();
    uint32_t size = rangeCount * FRAME_ACK_RANGE_SIZE;
    return static_cast<int32_t>(FRAME_ACK_HDR_SIZE + size);
}

int32_t eular::utp::FrameAck::rangeSize() const
{
    if (_history == nullptr || _history->empty()) {
        return 0;
    }

    uint32_t historyRangeCount = _history->rangeCount();
    uint32_t maxAckRanges = _config != nullptr ? _config->max_ack_range_size : historyRangeCount;
    uint32_t boundedRangeCount = historyRangeCount > maxAckRanges ? maxAckRanges : historyRangeCount;
    if (boundedRangeCount == 0) {
        return 0;
    }

    // range_count 表示 additional ACK ranges 的数量（不含 first_ack_range）
    return static_cast<int32_t>(boundedRangeCount - 1);
}
