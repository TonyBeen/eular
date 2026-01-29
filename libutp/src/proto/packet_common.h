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
enum PacketFrameTypeBit {
    kFTBitInvalid           = 1 << kFrameInvalid,
    kFTBitStrame            = 1 << kFrameStream,
    kFTBitAck               = 1 << kFrameAck,
    KFTBitPadding           = 1 << kFramePadding,
    KFTBitResetStream       = 1 << kFrameResetStream,
    KFTBitConnectionClose   = 1 << kFrameConnectionClose,
    KFTBitBlocked           = 1 << kFrameBlocked,
    KFTBittreamBlocked      = 1 << FrameStreamBlocked,
    KFTBitPing              = 1 << kFramePing,
    KFTBitMaxData           = 1 << kFrameMaxData,
    KFTBitMaxStreamData     = 1 << kFrameMaxStreamData,
    KFTBitMaxStreams        = 1 << kFrameMaxStreams,
    KFTBitPathChallenge     = 1 << kFramePathChallenge,
    KFTBitPathResponse      = 1 << kFramePathResponse,
    KFTBitCrypto            = 1 << kFrameCrypto,
    KFTBitSessionToken      = 1 << kFrameSessionToken,
    KFTBitAckFrequency      = 1 << kFrameAckFrequency,
    KFTBitVersion           = 1 << kFrameVersion,
};

static inline bool IsValidPackNo(uint64_t packno) {
    return packno <= UTP_MAX_PACKNO;
}


} // namespace utp
} // namespace eular

#endif // __PROTO_PACKET_COMMON_H__
