/*************************************************************************
    > File Name: packet_in.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include "proto/packet_in.h"

#include <algorithm>
#include <utils/serialize.hpp>

#include "logger/logger.h"
#include "proto/frame/ack_frequency.h"
#include "proto/frame/connection_close.h"
#include "proto/frame/crypto.h"
#include "proto/frame/data_blocked.h"
#include "proto/frame/handshake_delay.h"
#include "proto/frame/handshake_done.h"
#include "proto/frame/max_data.h"
#include "proto/frame/max_stream_data.h"
#include "proto/frame/padding.h"
#include "proto/frame/path.h"
#include "proto/frame/reset_stream.h"
#include "proto/frame/session_token.h"
#include "proto/frame/stream.h"
#include "proto/frame/stream_data_blocked.h"
#include "proto/frame/transport_params.h"
#include "proto/frame/version.h"
#include "util/error.h"
#include "utp/errno.h"

namespace {

constexpr size_t kAckFrameHeaderSize = (1 + 1 + 2 + 4 + 8);
constexpr size_t kAckRangeSize = 8;

uint16_t ReadBE16(const uint8_t *p) { return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]); }

}  // namespace

namespace eular {
namespace utp {

Status PacketIn::decode(const void *buffer, size_t size) {
    if (buffer == nullptr || size < UTP_HEADER_SIZE) {
        return Status::Error(UTP_ERR_OVERFLOW, fmt::format("packet too small: {}", size));
    }

    raw_data = static_cast<const uint8_t *>(buffer);
    raw_size = size;
    frame_types = 0;

    const uint8_t *offset = raw_data;
    size_t left = size;

    offset = eular::Serialize::DeserializeFrom(offset, left, header.scid);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "packet header scid deserialize failed");
    offset = eular::Serialize::DeserializeFrom(offset, left, header.dcid);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "packet header dcid deserialize failed");
    offset = eular::Serialize::DeserializeFrom(offset, left, header.pn);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "packet header pn deserialize failed");
    offset = eular::Serialize::DeserializeFrom(offset, left, header.payload_length);
    if (offset == nullptr)
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "packet header payload_length deserialize failed");
    offset = eular::Serialize::DeserializeFrom(offset, left, header.types);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "packet header types deserialize failed");
    offset = eular::Serialize::DeserializeFrom(offset, left, header.reserve);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "packet header reserve deserialize failed");

    if (left < static_cast<size_t>(header.payload_length)) {
        return Status::Error(UTP_ERR_OVERFLOW, fmt::format("payload truncated: available={}, header payload_length={}",
                                                           left, header.payload_length));
    }

    payload = offset;
    payload_size = static_cast<size_t>(header.payload_length);

    size_t iter = 0;
    while (iter < payload_size) {
        FrameType frameType = kFrameInvalid;
        size_t frameLen = 0;
        Status st = frameLength(payload + iter, payload_size - iter, frameType, frameLen);
        if (!st.ok() || frameLen == 0 || frameLen > (payload_size - iter)) {
            return Status::Error(UTP_ERR_FRAME_FORMAT_ERROR,
                                 fmt::format("invalid frame layout at offset {} (left={})", iter, payload_size - iter));
        }

        UTP_LOGD_FMT("PacketIn::decode frame found: type={}, len={}", static_cast<uint32_t>(frameType), frameLen);
        if (frameType >= kFrameMax) {
            return Status::Error(UTP_ERR_FRAME_FORMAT_ERROR,
                                 fmt::format("invalid frame type {}", static_cast<uint32_t>(frameType)));
        }

        frame_types |= (1u << static_cast<uint32_t>(frameType));
        iter += frameLen;
    }

    return Status::OK();
}

int32_t PacketIn::nextFrame(size_t &offset, FrameType &frameType, const uint8_t *&frameData, size_t &frameLen,
                            Status &status) const {
    if (!valid() || offset >= payload_size) {
        status = Status::ErrorLiteral(UTP_ERR_OVERFLOW, "invalid packet or offset beyond payload");
        return -1;
    }

    frameData = payload + offset;
    Status st = frameLength(frameData, payload_size - offset, frameType, frameLen);
    if (!st.ok() || frameLen == 0 || frameLen > (payload_size - offset)) {
        status = Status::ErrorLiteral(UTP_ERR_FRAME_FORMAT_ERROR, "invalid frame in payload");
        return -1;
    }

    offset += frameLen;
    return static_cast<int32_t>(frameLen);
}

Status PacketIn::frameLength(const uint8_t *frameData, size_t payloadLeft, FrameType &frameType, size_t &frameLen) {
    if (frameData == nullptr || payloadLeft == 0) {
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "frameLength: null data or empty payload");
    }

    frameType = static_cast<FrameType>(frameData[0]);
    frameLen = 0;

    switch (frameType) {
        case kFramePing:
            frameLen = 1;
            break;
        case kFrameHandshakeDone:
            frameLen = FRAME_HANDSHAKE_DONE_SIZE;
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
                return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "padding frame too short");
            }
            frameLen = FRAME_PADDING_HDR_SIZE + ReadBE16(frameData + 1);
            break;
        case kFrameSessionToken:
            if (payloadLeft < FRAME_SESSION_TOKEN_HDR_SIZE) {
                return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "session token frame too short");
            }
            frameLen = FRAME_SESSION_TOKEN_HDR_SIZE + frameData[1];
            break;
        case kFrameConnectionClose:
            if (payloadLeft < FRAME_CONNECTION_CLOSE_HDR_SIZE) {
                return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "connection close frame too short");
            }
            frameLen = FRAME_CONNECTION_CLOSE_HDR_SIZE + ReadBE16(frameData + 3);
            break;
        case kFrameStream:
            if (payloadLeft < FRAME_STREAM_HDR_SIZE) {
                return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "stream frame too short");
            }
            frameLen = FRAME_STREAM_HDR_SIZE + ReadBE16(frameData + 2);
            break;
        case kFrameAck:
            if (payloadLeft < kAckFrameHeaderSize) {
                return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "ack frame too short");
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
        case kFrameHandshakeDelay:
            frameLen = FRAME_HANDSHAKE_DELAY_SIZE;
            break;
        case kFrameMaxData:
            frameLen = FRAME_MAX_DATA_SIZE;
            break;
        case kFrameMaxStreamData:
            frameLen = FRAME_MAX_STREAM_DATA_SIZE;
            break;
        case kFrameDataBlocked:
            frameLen = FRAME_DATA_BLOCKED_SIZE;
            break;
        case kFrameStreamDataBlocked:
            frameLen = FRAME_STREAM_DATA_BLOCKED_SIZE;
            break;
        default:
            return Status::ErrorLiteral(UTP_ERR_FRAME_UNEXPECTED, "unknown frame type");
    }

    if (frameLen > payloadLeft) {
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "frame exceeds payload remainder");
    }

    return Status::OK();
}

}  // namespace utp
}  // namespace eular
