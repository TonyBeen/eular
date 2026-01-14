/*************************************************************************
    > File Name: frame.h
    > Author: eular
    > Brief:
    > Created Time: Fri 26 Dec 2025 02:12:12 PM CST
 ************************************************************************/

#ifndef __PROTO_PROTO_FRAME_H__
#define __PROTO_PROTO_FRAME_H__

#include <stdint.h>

#include "proto/proto.h"

#define STREAM_IS_FIN(flag)    ((flag) & FrameStream::kFrameStreamFlagFin)
#define STREAM_SET_FIN(flag)   ((flag) | FrameStream::kFrameStreamFlagFin)

namespace eular {
namespace utp {

enum FrameType : uint8_t {
    kFrameInvalid           = 0x00, // 无效帧
    kFrameStream            = 0x01, // 流数据帧
    kFrameAck               = 0x02, // 确认帧
    kFramePadding           = 0x03, // 填充帧
    kFrameResetStream       = 0x04, // 流重置帧
    kFrameConnectionClose   = 0x05, // 连接关闭帧
    kFrameBlocked           = 0x06, // 连接级阻塞帧
    FrameStreamBlocked      = 0x07, // 流级阻塞帧
    kFramePing              = 0x08, // 心跳帧
    kFrameMaxData           = 0x09, // 连接级最大数据帧
    kFrameMaxStreamData     = 0x0A, // 流级最大数据帧
    kFrameMaxStreams        = 0x0B, // 最大流数帧
    kFramePathChallenge     = 0x0C, // 路径校验帧
    kFramePathResponse      = 0x0D, // 路径响应帧
    kFrameCrypto            = 0x0E, // 加密数据帧
    kFrameSessionToken      = 0x0F, // 会话票据帧
    kFrameAckFrequency      = 0x10, // 确认频率帧
    kFrameVersion           = 0x11, // 版本协商帧
    kFrameMax,
};

class FrameHeader {
public:
    FrameHeader() = default;
    virtual ~FrameHeader() = default;

    virtual int32_t Encode(std::string &out) const = 0;
    virtual int32_t Decode(const void *data, size_t len) = 0;

public:
    FrameType   type{kFrameInvalid};
};

class FrameStream : public FrameHeader {
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
    uint16_t        stream_id{0};
    uint8_t         stream_flag{kFrameStreamFlagNone};
    uint64_t        stream_offset{0};
    uint16_t        stream_date_length{0};
    const uint8_t*  stream_data{nullptr}; // aes-gcm tag 在数据后面
    std::string     stream_data_real;
};

// 场景: 接收端收到包号 95-100, 90-92, 80-85
// QUIC 由于存在变长编码, 所以在设计时进行减1处理, 在边界时节省1字节, 这样在连续确认时, 可以节省更多字节. 此处未采用变长编码, 故使用实际数值.
// 包号状态:
//   80   81   82  83  84  85  86  87  88  89  90  91  92   93  94  95  96  97   98  99  100
//   ✓    ✓    ✓   ✓   ✓   ✓  ✗   ✗   ✗  ✗   ✓   ✓   ✓   ✗   ✗   ✓   ✓   ✓   ✓   ✓    ✓
// 
// ACK 帧构造: 
// 
//  Largest Acknowledged = 100
//
//  First ACK Range = 6
//  └── 确认 [95, 100] 共 6 个包
//
//  ACK Range Count = 2
//  └── 有 2 个额外的 ACK Range
//
//  ACK Range 1:
//  ├── Gap = 3
//  │   └── 未确认 [93, 94] 共 2 个包 (Gap)
//  └── ACK Range Length = 4
//      └── 确认 [90, 92] 共 3 个包 (Length)
//
//  ACK Range 2:
//  ├── Gap = 4
//  │   └── 未确认 [86, 89] 共 4 个包 (Gap)
//  └── ACK Range Length = 6
//      └── 确认 [80, 85] 共 6 个包 (Length)
//
class FrameAck : public FrameHeader {
public:
    FrameAck() = default;
    ~FrameAck() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    struct AckRange {
        uint32_t    gap{0};          // gap to the previous ack range
        uint32_t    length{0};       // length of this ack range
    };

    uint8_t         ack_count{0};       // number of ack blocks
    uint16_t        ack_delay{0};       // ms
    uint64_t        ack_largest{0};     // largest acknowledged packet number
    uint64_t        first_ack_range;    // first ack range
    std::vector<AckRange>  ack_ranges;  // additional ack ranges
};

class FramePadding : public FrameHeader {
public:
    FramePadding() = default;
    ~FramePadding() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t        padding_length{0};
    const uint8_t*  padding_data{nullptr};
    std::string     padding_data_real;
};

class FrameResetStream : public FrameHeader {
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
class FrameConnectionClose : public FrameHeader {
public:
    FrameConnectionClose() = default;
    ~FrameConnectionClose() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t        error_code{0};
    uint16_t        reason_length{0};
    const uint8_t*  reason_phrase{nullptr}; // utf-8
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
class FrameBlocked : public FrameHeader {
public:
    FrameBlocked() = default;
    ~FrameBlocked() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint64_t    maximum_data{0}; // 当前被阻塞的偏移量
};

class FrameStreamBlocked : public FrameHeader {
public:
    FrameStreamBlocked() = default;
    ~FrameStreamBlocked() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t    stream_id{0};           // 被阻塞的流ID
    uint64_t    maximum_stream_data{0}; // 被阻塞时的偏移量
};

class FramePing : public FrameHeader {
public:
    FramePing() = default;
    ~FramePing() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;
};

class FrameMaxData : public FrameHeader {
public:
    FrameMaxData() = default;
    ~FrameMaxData() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint64_t    maximum_data{0}; // 连接级别数据最大偏移量
};

class FrameMaxStreamData : public FrameHeader {
public:
    FrameMaxStreamData() = default;
    ~FrameMaxStreamData() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint16_t    stream_id{0};
    uint64_t    maximum_stream_data{0}; // 流级别数据最大偏移量
};

class FrameMaxStreams : public FrameHeader {
public:
    FrameMaxStreams() = default;
    ~FrameMaxStreams() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint8_t     stream_type{0};      // 0: 双向流, 1: 单向流
    uint16_t    maximum_streams{0};  // 最大流数量
};

class FramePathChallenge : public FrameHeader {
public:
    FramePathChallenge() = default;
    ~FramePathChallenge() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;
};

class FramePathResponse : public FrameHeader {
public:
    FramePathResponse() = default;
    ~FramePathResponse() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    uint8_t     token[SESSION_TOKEN_SIZE]; // SHA-256
};

class FrameCrypto : public FrameHeader {
public:
    FrameCrypto() = default;
    ~FrameCrypto() = default;

    int32_t Encode(std::string &out) const override;
    int32_t Decode(const void *data, size_t len) override;

public:
    std::array<uint8_t, 16>   crypto_random{};
    std::array<uint8_t, 32>   crypto_data{};
};

class FrameSessionToken : public FrameHeader {
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

class FrameAckFrequency : public FrameHeader {
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

class FrameVersion : public FrameHeader {
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
