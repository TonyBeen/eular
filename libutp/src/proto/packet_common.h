/*************************************************************************
    > File Name: packet_common.h
    > Author: eular
    > Brief:
    > Created Time: Thu 11 Dec 2025 10:29:33 AM CST
 ************************************************************************/

#ifndef __PROTO_PACKET_COMMON_H__
#define __PROTO_PACKET_COMMON_H__

#include "proto/frame.h"

#define UTP_MAX_PACKNO      ((1ull << 62 ) - 1) // 2^62 - 1
#define UTP_INVALID_PACKNO  (UTP_MAX_PACKNO + 1)

namespace eular {
namespace utp {
enum PacketFrameTypeBit : uint32_t {
    kFTBitInvalid           = 1 << kFrameInvalid,
    kFTBitStrame            = 1 << kFrameStream,
    kFTBitAck               = 1 << kFrameAck,
    kFTBitPadding           = 1 << kFramePadding,
    kFTBitConnectionClose   = 1 << kFrameConnectionClose,
    kFTBitPing              = 1 << kFramePing,
    kFTBitResetStream       = 1 << kFrameResetStream,
    kFTBitStreamsBlocked    = 1 << kFrameStreamsBlocked,
    kFTBitMaxStreams        = 1 << kFrameMaxStreams,
    kFTBitPathChallenge     = 1 << kFramePathChallenge,
    kFTBitPathResponse      = 1 << kFramePathResponse,
    kFTBitCrypto            = 1 << kFrameCrypto,
    kFTBitSessionToken      = 1 << kFrameSessionToken,
    kFTBitAckFrequency      = 1 << kFrameAckFrequency,
    kFTBitVersion           = 1 << kFrameVersion,
    kFTBitHandshakeDone     = 1 << kFrameHandshakeDone,
    kFTBitTransportParams   = 1 << kFrameTransportParams,
};

#define UTP_FRAME_RETX_MASK (   \
      kFTBitStrame              \
    /* | kFTBitAck */           \
    /* | kFTBitPadding */       \
    | kFTBitConnectionClose     \
    /* | kFTBitPing */          \
    | kFTBitResetStream         \
    | kFTBitStreamsBlocked      \
    | kFTBitMaxStreams          \
    | kFTBitPathChallenge       \
    | kFTBitPathResponse        \
    | kFTBitCrypto              \
    | kFTBitSessionToken        \
    | kFTBitAckFrequency        \
    | kFTBitVersion             \
    | kFTBitHandshakeDone       \
    | kFTBitTransportParams     \
)

static inline bool IsValidPackNo(uint64_t packno) {
    return packno <= UTP_MAX_PACKNO;
}


} // namespace utp
} // namespace eular

#endif // __PROTO_PACKET_COMMON_H__
