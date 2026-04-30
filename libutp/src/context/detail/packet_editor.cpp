/*************************************************************************
    > File Name: packet_editor.cpp
    > Author: eular
    > Brief:
 ************************************************************************/

#include "context/detail/packet_editor.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <utils/serialize.hpp>

#include "context/connection_impl.h"
#include "proto/proto.h"
#include "proto/packet_out.h"

namespace {

using eular::Serialize;

}

namespace eular {
namespace utp {
namespace detail {

bool PacketEditor::RewritePacketNumber(ConnectionImpl *conn, PacketOut *pkt)
{
    constexpr size_t kPacketNumberOffset = offsetof(UTPHeaderProto, pn);

    if (conn == nullptr || pkt == nullptr || pkt->raw_data == nullptr) {
        return false;
    }
    if (pkt->data_size < UTP_HEADER_SIZE) {
        return false;
    }
    if (pkt->alloc_size < kPacketNumberOffset + sizeof(pkt->packno)) {
        return false;
    }

    pkt->packno = conn->packetNumber();
    uint8_t *offset = pkt->raw_data + kPacketNumberOffset;
    size_t left = pkt->alloc_size - kPacketNumberOffset;
    return Serialize::SerializeTo(offset, left, pkt->packno) != nullptr;
}

bool PacketEditor::RewritePayloadLength(PacketOut *pkt, uint16_t payloadLen)
{
    constexpr size_t kPayloadLengthOffset = offsetof(UTPHeaderProto, payload_length);
    if (pkt == nullptr || pkt->raw_data == nullptr || pkt->alloc_size < kPayloadLengthOffset + sizeof(payloadLen)) {
        return false;
    }

    uint8_t *offset = pkt->raw_data + kPayloadLengthOffset;
    size_t left = pkt->alloc_size - kPayloadLengthOffset;
    return Serialize::SerializeTo(offset, left, payloadLen) != nullptr;
}

bool PacketEditor::StripTransientAckPayload(PacketOut *pkt)
{
    if (pkt == nullptr || pkt->raw_data == nullptr || pkt->transient_ack_size == 0) {
        return true;
    }
    if ((pkt->frame_types & (1u << static_cast<uint32_t>(kFrameAck))) == 0) {
        pkt->transient_ack_size = 0;
        return true;
    }

    uint16_t ackOffset = UTP_HEADER_SIZE;
    uint16_t ackLength = pkt->transient_ack_size;
    bool ackMetaFound = false;
    for (uint8_t i = 0; i < pkt->frame_meta_count; ++i) {
        const bool markedTransient = (pkt->frame_meta[i].frame_flags & kFMTransientOnRetrans) != 0;
        if (pkt->frame_meta[i].frame_type == kFrameAck || markedTransient) {
            ackOffset = pkt->frame_meta[i].offset;
            ackLength = pkt->frame_meta[i].length;
            ackMetaFound = true;
            break;
        }
    }

    if (pkt->data_size < ackOffset + ackLength) {
        return false;
    }

    const size_t payloadLen = static_cast<size_t>(pkt->data_size - UTP_HEADER_SIZE);
    if (ackOffset < UTP_HEADER_SIZE || ackLength == 0) {
        return false;
    }

    const size_t ackPayloadOffset = static_cast<size_t>(ackOffset - UTP_HEADER_SIZE);
    if (payloadLen < ackPayloadOffset + ackLength) {
        return false;
    }

    const size_t remainPayload = payloadLen - ackLength;
    uint8_t *payloadBase = pkt->raw_data + UTP_HEADER_SIZE;
    std::memmove(payloadBase + ackPayloadOffset,
                 payloadBase + ackPayloadOffset + ackLength,
                 payloadLen - (ackPayloadOffset + ackLength));

    if (!RewritePayloadLength(pkt, static_cast<uint16_t>(remainPayload))) {
        return false;
    }

    pkt->data_size = static_cast<uint16_t>(UTP_HEADER_SIZE + remainPayload);
    pkt->transient_ack_size = 0;

    uint32_t frameTypeBits = 0;
    uint8_t writeIndex = 0;
    for (uint8_t i = 0; i < pkt->frame_meta_count; ++i) {
        FrameMetaInfo meta = pkt->frame_meta[i];
        if (meta.frame_type == kFrameAck
            || (meta.frame_flags & kFMTransientOnRetrans) != 0) {
            continue;
        }
        if (meta.offset > ackOffset) {
            meta.offset = static_cast<uint16_t>(meta.offset - ackLength);
        }
        pkt->frame_meta[writeIndex++] = meta;
        if (meta.frame_type < kFrameMax) {
            frameTypeBits |= (1u << static_cast<uint32_t>(meta.frame_type));
            if (meta.frame_type == kFrameHandshakeDone) {
                frameTypeBits |= (1u << static_cast<uint32_t>(kFrameHandshakeDelay));
            }
        }
    }
    pkt->frame_meta_count = writeIndex;
    if (ackMetaFound) {
        pkt->frame_types = frameTypeBits;
    } else {
        pkt->frame_types &= ~(1u << static_cast<uint32_t>(kFrameAck));
    }

    if (pkt->encrypt_data != nullptr && pkt->encrypt_data != pkt->raw_data) {
        std::free(pkt->encrypt_data);
    }
    pkt->encrypt_data = nullptr;
    pkt->encrypt_data_size = 0;

    pkt->slice_count = 0;
    pkt->slices[pkt->slice_count++] = PacketOutSlice{0, static_cast<uint16_t>(UTP_HEADER_SIZE)};
    if (remainPayload > 0 && pkt->slice_count < PACKET_OUT_MAX_SLICES) {
        pkt->slices[pkt->slice_count++] = PacketOutSlice{static_cast<uint16_t>(UTP_HEADER_SIZE),
                                                          static_cast<uint16_t>(remainPayload)};
    }

    return true;
}

} // namespace detail
} // namespace utp
} // namespace eular
