/*************************************************************************
    > File Name: packet_decode_helper.cpp
    > Author: eular
    > Brief:
    > Created Time: Mon 30 Mar 2026
 ************************************************************************/

#include "context/packet_decode_helper.h"

#include <cstring>

#include <utils/serialize.hpp>

#include "utp/errno.h"

namespace eular {
namespace utp {
namespace detail {

namespace {

constexpr size_t kPayloadLengthOffset = 16;

bool ParseHeader(const uint8_t *data, size_t size, UTPHeaderProto &header, const uint8_t *&payload)
{
    if (data == nullptr || size < UTP_HEADER_SIZE) {
        return false;
    }

    const uint8_t *offset = data;
    size_t left = size;
    offset = Serialize::DeserializeFrom(offset, left, header.scid);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, header.dcid);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, header.pn);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, header.payload_length);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, header.types);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, header.reserve);
    if (offset == nullptr) return false;

    if (left < header.payload_length) {
        return false;
    }

    payload = offset;
    return true;
}

} // namespace

bool DecodeUdpPacketWithOptionalAead(const UdpSocket::MsgMetaInfo &msg, MemoryManager &mm,
                                     const std::shared_ptr<AesGcmContext> &aesCtx, PacketIn &packet)
{
    UNUSED(mm);
    if (msg.data == nullptr || msg.len < UTP_HEADER_SIZE || packet.raw_data == nullptr) {
        return false;
    }

    const uint8_t *msgData = static_cast<const uint8_t *>(msg.data);

    UTPHeaderProto header{};
    const uint8_t *payload = nullptr;
    if (!ParseHeader(msgData, msg.len, header, payload)) {
        return false;
    }

    if (!aesCtx) {
        std::memcpy(const_cast<uint8_t *>(packet.raw_data), msgData, msg.len);
        packet.raw_size = msg.len;
        packet.meta = msg.metaInfo;
        return packet.decode(packet.raw_data, packet.raw_size) == UTP_ERR_OK;
    }

    if (header.payload_length < AesGcmContext::GCM_TAG_SIZE) {
        return false;
    }

    if (packet.alloc_size < UTP_HEADER_SIZE) {
        return false;
    }

    const size_t plainCapacity = static_cast<size_t>(packet.alloc_size) - UTP_HEADER_SIZE;
    size_t plainLen = plainCapacity;
    if (plainLen + AesGcmContext::GCM_TAG_SIZE < header.payload_length) {
        return false;
    }

    std::memcpy(const_cast<uint8_t *>(packet.raw_data), msgData, UTP_HEADER_SIZE);
    if (aesCtx->decrypt(payload,
                        header.payload_length,
                        msgData,
                        UTP_HEADER_SIZE,
                        header.pn,
                        const_cast<uint8_t *>(packet.raw_data) + UTP_HEADER_SIZE,
                        &plainLen) != UTP_ERR_OK) {
        return false;
    }

    const size_t decodedSize = UTP_HEADER_SIZE + plainLen;
    if (decodedSize > static_cast<size_t>(packet.alloc_size) || decodedSize > msg.len) {
        return false;
    }

    uint8_t *mutableRaw = const_cast<uint8_t *>(packet.raw_data);
    mutableRaw[kPayloadLengthOffset] = static_cast<uint8_t>((plainLen >> 8) & 0xFFu);
    mutableRaw[kPayloadLengthOffset + 1] = static_cast<uint8_t>(plainLen & 0xFFu);

    packet.raw_size = decodedSize;
    packet.meta = msg.metaInfo;
    return packet.decode(packet.raw_data, decodedSize) == UTP_ERR_OK;
}

} // namespace detail
} // namespace utp
} // namespace eular
