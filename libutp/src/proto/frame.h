/*************************************************************************
    > File Name: frame.h
    > Author: eular
    > Brief:
    > Created Time: Fri 26 Dec 2025 02:12:12 PM CST
 ************************************************************************/

#ifndef __PROTO_PROTO_FRAME_H__
#define __PROTO_PROTO_FRAME_H__

#include <stdint.h>
#include <string>

#include <utils/endian.hpp>

#include "proto/proto.h"

#define STREAM_IS_FIN(flag)    ((flag) & kFrameStreamFlagFin)
#define STREAM_SET_FIN(flag)   ((flag) | kFrameStreamFlagFin)

namespace eular {
namespace utp {

enum FrameType : uint8_t {
    kFrameInvalid,          // 无效帧
    kFrameStream,           // 流数据帧
    kFrameAck,              // 确认帧
    kFramePadding,          // 填充帧
    kFrameConnectionClose,  // 连接关闭帧
    kFramePing,             // 心跳帧
    kFrameResetStream,      // 流重置帧
    kFrameStreamsBlocked,   // 流ID阻塞帧
    kFrameMaxStreams,       // 最大流数帧
    kFramePathChallenge,    // 路径校验帧
    kFramePathResponse,     // 路径响应帧
    kFrameCrypto,           // 加密数据帧
    kFrameSessionToken,     // 会话票据帧
    kFrameAckFrequency,     // 确认频率帧
    kFrameVersion,          // 版本协商帧
    kFrameHandshakeDone,    // 握手完成帧
    kFrameTransportParams,  // 传输参数帧
    kFrameMax,
};

std::string FrameTypeToString(uint32_t type);

class FrameBase {
public:
    FrameBase() = default;
    FrameBase(FrameType t) : type(t) {}
    virtual ~FrameBase() = default;

public:
    FrameType   type{kFrameInvalid};
};

enum FrameStreamFlags : uint8_t {
    kFrameStreamFlagNone    = 0x00,
    kFrameStreamFlagFin     = 0x01, // bit0 流结束标志, 该帧是流的最后一帧
};

enum FrameCryptoType : uint8_t {
    kFrameCryptoAESGCM128,  // AES-GCM-128加密数据
    kFrameCryptoAESGCM256,  // AES-GCM-256加密数据
};

} // namespace utp
} // namespace eular

#endif // __PROTO_PROTO_FRAME_H__
