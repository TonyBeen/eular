/*************************************************************************
    > File Name: packet_decode_helper.h
    > Author: eular
    > Brief:
    > Created Time: Mon 30 Mar 2026
 ************************************************************************/

#ifndef __UTP_CONTEXT_PACKET_DECODE_HELPER_H__
#define __UTP_CONTEXT_PACKET_DECODE_HELPER_H__

#include <memory>

#include "socket/udp.h"
#include "proto/packet_in.h"
#include "crypto/aes_gcm_context.h"
#include "util/mm.h"

namespace eular {
namespace utp {
namespace detail {

bool DecodeUdpPacketWithOptionalAead(const UdpSocket::MsgMetaInfo &msg,
                                     MemoryManager &mm,
                                     const std::shared_ptr<AesGcmContext> &aesCtx,
                                     PacketIn &packet);

} // namespace detail
} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_PACKET_DECODE_HELPER_H__
