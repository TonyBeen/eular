/*************************************************************************
    > File Name: ack.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 10:54:48 AM CST
 ************************************************************************/

#include "proto/frame/ack.h"
#include "utp/errno.h"
#include "util/error.h"

int32_t eular::utp::FrameAck::encode(void *buffer, size_t size) const
{
    int32_t ackFrameSize = frameSize();
    if (ackFrameSize > static_cast<int32_t>(size)) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "buffer size {} is smaller than ack frame size {}", size, ackFrameSize);
        return -1;
    }

    uint8_t *ptr = static_cast<uint8_t *>(buffer);
    *ptr = static_cast<uint8_t>(FrameType::kFrameAck);
    ptr += 1;
    *ptr = static_cast<uint8_t>(ack_count);
    ptr += 1;
    utp_time_t delta = _now - _history->largestAckedReceived();
    uint16_t ack_delay = static_cast<uint16_t>(delta / UTP_CONFIG_ACK_DELAY_FACTOR);
    *(uint16_t *)ptr = htobe16(ack_delay);
    ptr += 2;
    *(uint32_t *)ptr = htobe32(first_ack_range);
    ptr += 4;
    *(uint64_t *)ptr = htobe64(ack_largest);
    ptr += 8;

    int32_t idx = 0;
    for (const auto &ack_range : ack_ranges) {
        *(uint32_t *)ptr = htobe32(ack_range.gap);
        ptr += 4;
        *(uint32_t *)ptr = htobe32(ack_range.length);
        ptr += 4;
        idx++;

    }

    return ackFrameSize;
}

int32_t eular::utp::FrameAck::decode(const void *buffer, size_t size)
{
    return 0;
}

int32_t eular::utp::FrameAck::frameSize() const
{
    uint32_t size = _history->rangeCount() * FRAME_ACK_RANGE_SIZE;
    return static_cast<int32_t>(FRAME_ACK_HDR_SIZE + size);
}
