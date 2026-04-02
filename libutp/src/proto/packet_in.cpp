/*************************************************************************
    > File Name: packet_in.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include "proto/packet_in.h"

#include <algorithm>

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"
#include "proto/frame/stream.h"
#include "proto/frame/padding.h"
#include "proto/frame/path.h"
#include "proto/frame/version.h"
#include "proto/frame/ack_frequency.h"
#include "proto/frame/transport_params.h"
#include "proto/frame/session_token.h"
#include "proto/frame/connection_close.h"
#include "proto/frame/crypto.h"
#include "proto/frame/reset_stream.h"
#include "logger/logger.h"

namespace {

constexpr size_t kAckFrameHeaderSize = (1 + 1 + 2 + 4 + 8);
constexpr size_t kAckRangeSize = 8;

uint16_t ReadBE16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

} // namespace

namespace eular {
namespace utp {

int32_t PacketIn::decode(const void *buffer, size_t size)
{
    if (buffer == nullptr || size < UTP_HEADER_SIZE) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "packet too small: {}", size);
        return -1;
    }

    raw_data = static_cast<const uint8_t *>(buffer);
    raw_size = size;
    frame_types = 0;

    const uint8_t *offset = raw_data;
    size_t left = size;

    offset = eular::Serialize::DeserializeFrom(offset, left, header.scid);
    if (offset == nullptr) return -1;
    offset = eular::Serialize::DeserializeFrom(offset, left, header.dcid);
    if (offset == nullptr) return -1;
    offset = eular::Serialize::DeserializeFrom(offset, left, header.pn);
    if (offset == nullptr) return -1;
    offset = eular::Serialize::DeserializeFrom(offset, left, header.payload_length);
    if (offset == nullptr) return -1;
    offset = eular::Serialize::DeserializeFrom(offset, left, header.types);
    if (offset == nullptr) return -1;
    offset = eular::Serialize::DeserializeFrom(offset, left, header.reserve);
    if (offset == nullptr) return -1;

    if (left < static_cast<size_t>(header.payload_length)) {
        SetLastErrorV(UTP_ERR_OVERFLOW,
                      "payload truncated: available={}, header payload_length={}",
                      left,
                      header.payload_length);
        return -1;
    }

    payload = offset;
    payload_size = static_cast<size_t>(header.payload_length);

    size_t iter = 0;
    while (iter < payload_size) {
        FrameType frameType = kFrameInvalid;
        size_t frameLen = 0;
        int32_t status = frameLength(payload + iter, payload_size - iter, frameType, frameLen);
        if (status < 0 || frameLen == 0 || frameLen > (payload_size - iter)) {
            SetLastErrorV(UTP_ERR_FRAME_FORMAT_ERROR,
                          "invalid frame layout at offset {} (left={})",
                          iter,
                          payload_size - iter);
            return -1;
        }

        UTP_LOGD_FMT("PacketIn::decode frame found: type={}, len={}", static_cast<uint32_t>(frameType), frameLen);
        if (frameType >= kFrameMax) {
            SetLastErrorV(UTP_ERR_FRAME_FORMAT_ERROR,
                          "invalid frame type {}",
                          static_cast<uint32_t>(frameType));
            return -1;
        }

        frame_types |= (1u << static_cast<uint32_t>(frameType));
        iter += frameLen;
    }

    return UTP_ERR_OK;
}

int32_t PacketIn::nextFrame(size_t &offset,
                            FrameType &frameType,
                            const uint8_t *&frameData,
                            size_t &frameLen) const
{
    if (!valid() || offset >= payload_size) {
        return -1;
    }

    frameData = payload + offset;
    int32_t status = frameLength(frameData, payload_size - offset, frameType, frameLen);
    if (status < 0 || frameLen == 0 || frameLen > (payload_size - offset)) {
        return -1;
    }

    offset += frameLen;
    return static_cast<int32_t>(frameLen);
}

int32_t PacketIn::frameLength(const uint8_t *frameData,
                              size_t payloadLeft,
                              FrameType &frameType,
                              size_t &frameLen)
{
    if (frameData == nullptr || payloadLeft == 0) {
        return -1;
    }

    frameType = static_cast<FrameType>(frameData[0]);
    frameLen = 0;

    switch (frameType) {
    case kFramePing:
    case kFrameHandshakeDone:
        frameLen = 1;
        break;
    case kFramePathChallenge:
    case kFramePathResponse:
        frameLen = FRAME_PATH_FRAME_SIZE;
        break;
    case kFrameVersion:
        frameLen = FRAME_VERSION_SIZE;
        break;
    case kFramePadding:
        if (payloadLeft < FRAME_PADDING_HDR_SIZE) {
            return -1;
        }
        frameLen = FRAME_PADDING_HDR_SIZE + ReadBE16(frameData + 1);
        break;
    case kFrameSessionToken:
        if (payloadLeft < FRAME_SESSION_TOKEN_HDR_SIZE) {
            return -1;
        }
        frameLen = FRAME_SESSION_TOKEN_HDR_SIZE + frameData[1];
        break;
    case kFrameConnectionClose:
        if (payloadLeft < FRAME_CONNECTION_CLOSE_HDR_SIZE) {
            return -1;
        }
        frameLen = FRAME_CONNECTION_CLOSE_HDR_SIZE + ReadBE16(frameData + 3);
        break;
    case kFrameStream:
        if (payloadLeft < FRAME_STREAM_HDR_SIZE) {
            return -1;
        }
        frameLen = FRAME_STREAM_HDR_SIZE + ReadBE16(frameData + 2);
        break;
    case kFrameAck:
        if (payloadLeft < kAckFrameHeaderSize) {
            return -1;
        }
        frameLen = kAckFrameHeaderSize + static_cast<size_t>(frameData[1]) * kAckRangeSize;
        break;
    case kFrameCrypto:
        frameLen = FRAME_CRYPTO_SIZE;
        break;
    case kFrameResetStream:
        frameLen = FRAME_RESET_STREAM_SIZE;
        break;
    case kFrameAckFrequency:
        frameLen = FRAME_ACK_FREQUENCY_SIZE;
        break;
    case kFrameTransportParams:
        frameLen = FRAME_TRANSPORT_PARAMS_SIZE;
        break;
    default:
        return -1;
    }

    if (frameLen > payloadLeft) {
        return -1;
    }

    return static_cast<int32_t>(frameLen);
}

} // namespace utp
} // namespace eular
