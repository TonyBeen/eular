/*************************************************************************
    > File Name: packet_in.h
    > Author: eular
    > Brief:
    > Created Time: Fri 16 Jan 2026 03:59:14 PM CST
 ************************************************************************/

#ifndef __UTP_PROTO_PACKET_IN_H__
#define __UTP_PROTO_PACKET_IN_H__

#include "proto/proto.h"
#include "proto/frame.h"
#include "socket/packet.h"

namespace eular {
namespace utp {

struct PacketIn {
public:
    int32_t decode(const void *buffer, size_t size);

    bool valid() const { return raw_data != nullptr && raw_size >= UTP_HEADER_SIZE; }
    bool hasFrame(FrameType frameType) const {
        if (frameType >= kFrameMax) {
            return false;
        }
        return (frame_types & (1u << static_cast<uint32_t>(frameType))) != 0;
    }

    int32_t nextFrame(size_t &offset,
                      FrameType &frameType,
                      const uint8_t *&frameData,
                      size_t &frameLen) const;

private:
    static int32_t frameLength(const uint8_t *frameData,
                               size_t payloadLeft,
                               FrameType &frameType,
                               size_t &frameLen);

public:
    const uint8_t*  raw_data{nullptr};
    size_t          raw_size{0};
    uint16_t        alloc_size{0};

    UTPHeaderProto  header{};
    const uint8_t*  payload{nullptr};
    size_t          payload_size{0};

    uint32_t        frame_types{0};
    PacketMetaInfo  meta{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_PROTO_PACKET_IN_H__
