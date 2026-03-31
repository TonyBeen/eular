/*************************************************************************
    > File Name: packet_decode_helper.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 30 Mar 2026
 ************************************************************************/

#include "context/packet_decode_helper.h"

#include <cstring>
#include <memory>

#include <utils/serialize.hpp>

#include "utp/errno.h"

namespace eular {
namespace utp {
namespace detail {

bool DecodeUdpPacketWithOptionalAead(const UdpSocket::MsgMetaInfo &msg,
                                     MemoryManager &mm,
                                     const std::shared_ptr<AesGcmContext> &aesCtx,
                                     PacketIn &packet)
{
    if (msg.data == nullptr || msg.len < UTP_HEADER_SIZE || packet.raw_data == nullptr) {
        return false;
    }

    std::memcpy(const_cast<uint8_t *>(packet.raw_data), msg.data, msg.len);
    packet.raw_size = msg.len;
    packet.meta = msg.metaInfo;
    if (packet.decode(packet.raw_data, packet.raw_size) == UTP_ERR_OK) {
        return true;
    }

    if (!aesCtx) {
        return false;
    }

    auto packetReleaser = [&mm] (PacketIn *pkt) {
        mm.putPacketIn(pkt);
    };
    std::unique_ptr<PacketIn, decltype(packetReleaser)> encryptedPacket(
        mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
    if (!encryptedPacket || encryptedPacket->raw_data == nullptr) {
        return false;
    }

    std::memcpy(const_cast<uint8_t *>(encryptedPacket->raw_data), msg.data, msg.len);
    encryptedPacket->raw_size = msg.len;

    const uint8_t *offset = encryptedPacket->raw_data;
    size_t left = encryptedPacket->raw_size;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.scid);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.dcid);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.pn);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.payload_length);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.types);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.reserve);
    if (offset == nullptr) return false;

    if (left < encryptedPacket->header.payload_length) {
        return false;
    }

    encryptedPacket->payload = offset;
    encryptedPacket->payload_size = encryptedPacket->header.payload_length;
    if (aesCtx->decrypt(encryptedPacket.get()) != UTP_ERR_OK) {
        return false;
    }

    // PacketIn::decode overwrites raw_data/raw_size with the input buffer.
    // Decode from packet-owned storage to preserve single ownership semantics.
    std::memcpy(const_cast<uint8_t *>(packet.raw_data),
                encryptedPacket->raw_data,
                encryptedPacket->raw_size);
    packet.raw_size = encryptedPacket->raw_size;
    packet.meta = msg.metaInfo;

    return packet.decode(packet.raw_data, packet.raw_size) == UTP_ERR_OK;
}

} // namespace detail
} // namespace utp
} // namespace eular
