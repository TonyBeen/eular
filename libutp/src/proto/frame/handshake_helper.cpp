/*************************************************************************
    > File Name: handshake_helper.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#include "proto/frame/handshake_helper.h"

#include <algorithm>
#include <limits>

#include "proto/frame/handshake_done.h"
#include "proto/frame/handshake_delay.h"
#include "utp/errno.h"

namespace eular {
namespace utp {

int32_t BuildHandshakeDoneFrame(utp_packno_t ackHandshakePn,
                                uint8_t *buffer,
                                size_t size,
                                size_t &outSize)
{
    FrameHandshakeDone done;
    done.ack_handshake_pn = ackHandshakePn;

    const int32_t encoded = done.encode(buffer, size);
    if (encoded < 0) {
        return -1;
    }

    outSize = static_cast<size_t>(encoded);
    return UTP_ERR_OK;
}

int32_t BuildHandshakeDelayFrame(utp_time_t delayUs,
                                 uint8_t *buffer,
                                 size_t size,
                                 size_t &outSize)
{
    FrameHandshakeDelay delay;
    delay.delay_time_us = static_cast<uint32_t>(
        std::min<utp_time_t>(delayUs, static_cast<utp_time_t>(std::numeric_limits<uint32_t>::max())));

    const int32_t encoded = delay.encode(buffer, size);
    if (encoded < 0) {
        return -1;
    }

    outSize = static_cast<size_t>(encoded);
    return UTP_ERR_OK;
}

int32_t BuildHandshakeTrailer(utp_packno_t ackHandshakePn,
                              utp_time_t delayUs,
                              uint8_t *buffer,
                              size_t size,
                              size_t &outSize)
{
    outSize = 0;

    size_t doneSize = 0;
    if (BuildHandshakeDoneFrame(ackHandshakePn, buffer, size, doneSize) != UTP_ERR_OK) {
        return -1;
    }

    size_t delaySize = 0;
    if (BuildHandshakeDelayFrame(delayUs,
                                 buffer + doneSize,
                                 size - doneSize,
                                 delaySize) != UTP_ERR_OK) {
        return -1;
    }

    outSize = doneSize + delaySize;
    return UTP_ERR_OK;
}

} // namespace utp
} // namespace eular
