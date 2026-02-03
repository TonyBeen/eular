/*************************************************************************
    > File Name: frame.h
    > Author: eular
    > Brief:
    > Created Time: Fri 26 Dec 2025 02:12:12 PM CST
 ************************************************************************/

#ifndef __PROTO_PROTO_FRAME_H__
#define __PROTO_PROTO_FRAME_H__

#include <stdint.h>

#include <utils/endian.hpp>

#include "proto/proto.h"

#define STREAM_IS_FIN(flag)    ((flag) & FrameStream::kFrameStreamFlagFin)
#define STREAM_SET_FIN(flag)   ((flag) | FrameStream::kFrameStreamFlagFin)

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
    kFrameMax,
};

static inline std::string FrameTypeToString(uint32_t type);

class FrameBase {
public:
    FrameBase() = default;
    FrameBase(FrameType t) : type(t) {}
    virtual ~FrameBase() = default;

public:
    FrameType   type{kFrameInvalid};
};

#define FRAME_STREAM_HDR_SIZE   (1 + 1 + 2 + 4 + 8) // type + stream_flag + stream_data_length + stream_id + stream_offset
class FrameStream : public FrameBase {
public:
    enum FrameStreamFlags {
        kFrameStreamFlagNone    = 0x00,
        kFrameStreamFlagFin     = 0x80,
    };

    FrameStream() = default;
    ~FrameStream() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint8_t         stream_flag{kFrameStreamFlagNone};
    uint16_t        stream_data_length{0};
    uint32_t        stream_id{0};
    uint64_t        stream_offset{0};
    const uint8_t*  stream_data{nullptr}; // aes-gcm tag 在数据后面
};


class FramePadding : public FrameBase {
public:
    FramePadding() = default;
    ~FramePadding() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t        padding_length{0};
};

class FrameResetStream : public FrameBase {
public:
    FrameResetStream() = default;
    ~FrameResetStream() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t    stream_id{0};
    uint16_t    error_code{0};
    uint64_t    final_offset{0};
};

//    Client                                  Server
//       │                                       │
//       │ 应用层请求关闭                          │
//       │                                       │
//       │─── CONNECTION_CLOSE ─────────────────>│
//       │     error=NO_ERROR                    │
//       │                                       │
//       │     [进入 CLOSING]                     │ [收到 CLOSE]
//       │                                       │
//       │<── CONNECTION_CLOSE ──────────────────│
//       │     error=NO_ERROR                    │
//       │                                       │
//       │     [进入 DRAINING]                    │
//       │                                       │
//       │     (静默期, 不发送任何包)               │
//       │                                       │
//       ~~~    3 * PTO 超时    ~~~~   3 * PTO   ~~~
//       │                                       │
//       │     [CLOSED]                 [CLOSED] │
//       │                                       │
//       │     释放资源                  释放资源   │
//       │                                       │
class FrameConnectionClose : public FrameBase {
public:
    FrameConnectionClose() = default;
    ~FrameConnectionClose() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t        error_code{0};
    uint16_t        reason_length{0};
    const uint8_t*  reason_phrase{nullptr}; // ascii
    std::string     reason_phrase_real;
};

/*
 * 使用绝对偏移量的优点：
 *
 * 1. 幂等性：重复收到相同的 MAX_STREAM_DATA 不会累加
 *    - 如果用增量：收到两次 "+32KB" 会变成 "+64KB" (错误!)
 *    - 用绝对值：收到两次 "max=128KB" 仍是 128KB (正确!)
 *
 * 2. 简单性：不需要跟踪"上次发送了多少"
 *
 * 3. 乱序安全：包乱序到达不会导致错误计算
 *
 * 4. 丢包安全：帧丢失后重传不会重复累加
 */
class FrameBlocked : public FrameBase {
public:
    FrameBlocked() = default;
    ~FrameBlocked() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint64_t    maximum_data{0}; // 当前被阻塞的偏移量
};

class FrameStreamBlocked : public FrameBase {
public:
    FrameStreamBlocked() = default;
    ~FrameStreamBlocked() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t    stream_id{0};           // 被阻塞的流ID
    uint64_t    maximum_stream_data{0}; // 被阻塞时的偏移量
};

class FramePing : public FrameBase {
public:
    FramePing() = default;
    ~FramePing() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;
};

class FrameMaxData : public FrameBase {
public:
    FrameMaxData() = default;
    ~FrameMaxData() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint64_t    maximum_data{0}; // 连接级别数据最大偏移量
};

class FrameMaxStreamData : public FrameBase {
public:
    FrameMaxStreamData() = default;
    ~FrameMaxStreamData() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t    stream_id{0};
    uint64_t    maximum_stream_data{0}; // 流级别数据最大偏移量
};

class FrameMaxStreams : public FrameBase {
public:
    FrameMaxStreams() = default;
    ~FrameMaxStreams() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint8_t     stream_type{0};      // 0: 双向流, 1: 单向流
    uint16_t    maximum_streams{0};  // 最大流数量
};

class FramePathChallenge : public FrameBase {
public:
    FramePathChallenge() = default;
    ~FramePathChallenge() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;
};

class FramePathResponse : public FrameBase {
public:
    FramePathResponse() = default;
    ~FramePathResponse() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint8_t     token[SESSION_TOKEN_SIZE]; // SHA-256
};

class FrameCrypto : public FrameBase {
public:
    FrameCrypto() = default;
    ~FrameCrypto() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    std::array<uint8_t, 16>   crypto_random{};
    std::array<uint8_t, 32>   crypto_data{};
};

class FrameSessionToken : public FrameBase {
public:
    FrameSessionToken() = default;
    ~FrameSessionToken() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    // TODO seeion token 在服务端需要保持一定的时间, 以便验证客户端重连时使用
    uint16_t    token_effective_time{0}; // 有效时间, 单位秒, 最大18.204小时
    std::array<uint8_t, SESSION_TOKEN_SIZE> session_token{};
};

class FrameAckFrequency : public FrameBase {
public:
    FrameAckFrequency() = default;
    ~FrameAckFrequency() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint8_t     sn;                         // AckFrequency 包序号, 取最大值作为最新配置, 防止乱序导致旧配置覆盖新配置
    uint8_t     ack_eliciting_threshold;    // 收到几个包后发 ACK
    uint8_t     reordering_threshold;       // 乱序阈值
    uint32_t    max_ack_delay_ms;           // 最大 ACK 延迟, 单位毫秒
};

class FrameVersion : public FrameBase {
public:
    FrameVersion() = default;
    ~FrameVersion() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint32_t    version{0};
};
} // namespace utp
} // namespace eular

#endif // __PROTO_PROTO_FRAME_H__
