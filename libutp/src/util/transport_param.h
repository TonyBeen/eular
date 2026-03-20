/*************************************************************************
    > File Name: transport_param.h
    > Author: eular
    > Brief:
    > Created Time: Fri 30 Jan 2026 05:19:12 PM CST
 ************************************************************************/

#ifndef __UTP_TRANSPORT_PARAM_H__
#define __UTP_TRANSPORT_PARAM_H__

#include <stdint.h>

namespace eular {
namespace utp {
struct TransportParams {
public:
    enum {
        kMaxIdleTimeout     = 1u << 0, // 最大空闲超时时间(ms), 超过该时间未收到对端UDP包则断开连接
        kHandshakeTimeout   = 1u << 1, // 握手超时时间(ms)
        kInitMaxStreamsBidi = 1u << 2, // 初始双向流最大数量
        kInitMaxStreamsUni  = 1u << 3, // 初始单向流最大数量
        kAckDelayExponent   = 1u << 4, // ack延迟指数
        kMaxAckDelta        = 1u << 5, // 包号差值
        kMaxAckDelay        = 1u << 6, // 最大ack延迟时间(ms)
        kMaxNumeric         = 7,        // 参数个数
    };
    static const uint16_t kDefaultFlags =
          kMaxIdleTimeout
        | kHandshakeTimeout
        | kInitMaxStreamsBidi
        | kInitMaxStreamsUni
        | kAckDelayExponent
        | kMaxAckDelta
        | kMaxAckDelay;

    template<typename T>
    void setParam(int32_t param, T value)
    {
        switch (param) {
        case kMaxIdleTimeout:
            max_idle_timeout = value;
            break;
        case kHandshakeTimeout:
            handshake_timeout = value;
            break;
        case kInitMaxStreamsBidi:
            init_max_streams_bidi = value;
            break;
        case kInitMaxStreamsUni:
            init_max_streams_uni = value;
            break;
        case kAckDelayExponent:
            ack_delay_exponent = value;
            break;
        case kMaxAckDelta:
            max_ack_delta = value;
            break;
        case kMaxAckDelay:
            max_ack_delay = value;
            break;
        default:
            return;
        }

        flags |= static_cast<uint16_t>(param);
    }

    void clearParam(int32_t mask);

public:
    uint16_t    flags{kDefaultFlags};       // 参数启用标志, 每位表示一个参数是否启用
    uint32_t    max_idle_timeout{600000};   // 10 minutes
    uint16_t    handshake_timeout{5000};    // 对端允许握手超时时间(ms)
    uint16_t    init_max_streams_bidi{64};  // 对端允许的双向流最大数量
    uint16_t    init_max_streams_uni{32};   // 对端允许的单向流最大数量
    uint8_t     ack_delay_exponent{3};      // ack延迟时间的指数, ack_delay = 2^ack_delay_exponent ms
    uint16_t    max_ack_delta{10};          // 包号差值, 超过该值的ack将被丢弃
    uint16_t    max_ack_delay{150};         // 150 ms
};

} // namespace utp
} // namespace eular

#endif // __UTP_TRANSPORT_PARAM_H__
