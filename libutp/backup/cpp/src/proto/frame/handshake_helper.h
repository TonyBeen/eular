/*************************************************************************
    > File Name: handshake_helper.h
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#ifndef __UTP_PROTO_FRAME_HANDSHAKE_HELPER_H__
#define __UTP_PROTO_FRAME_HANDSHAKE_HELPER_H__

#include <cstddef>

#include "utp/types.h"

namespace eular {
namespace utp {

int32_t BuildHandshakeDoneFrame(utp_packno_t ackHandshakePn,
                                uint8_t *buffer,
                                size_t size,
                                size_t &outSize);

int32_t BuildHandshakeDelayFrame(utp_time_t delayUs,
                                 uint8_t *buffer,
                                 size_t size,
                                 size_t &outSize);

int32_t BuildHandshakeTrailer(utp_packno_t ackHandshakePn,
                              utp_time_t delayUs,
                              uint8_t *buffer,
                              size_t size,
                              size_t &outSize);

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_FRAME_HANDSHAKE_HELPER_H__
