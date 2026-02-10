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
        kMaxIdleTimeout,        // 最大空闲超时时间(ms), 超过该时间未收到对端UDP包则断开连接
        kHandshakeTimeout,      // 握手超时时间(ms)
        kInitMaxStreamsBidi,    // 初始双向流最大数量
        kInitMaxStreamsUni,     // 初始单向流最大数量
        kAckDelayExponent,      // ack延迟指数
        kMaxAckDelta,           // 包号差值
        kMaxAckDelay,           // 最大ack延迟时间(ms)
        kMaxNumeric,            // 占位符, 表示枚举值的最大数量
    };

    template<typename T>
    void setParam(int32_t param, T value)
    {
        switch (param) {
        case kMaxIdleTimeout:
            break;
        case kHandshakeTimeout:
            break;
        case kInitMaxStreamsBidi:
            break;
        case kInitMaxStreamsUni:
            break;
        case kAckDelayExponent:
            break;
        case kMaxAckDelta:
            break;
        case kMaxAckDelay:
            break;
        default:
            return;
        }

        flags |= (1 << param);
    }

    void clearParam(int32_t param);

public:
    uint16_t    flags;
    uint32_t    max_idle_timeout{600000};   // 10 minutes
    uint16_t    handshake_timeout{5000};    // 对端允许握手超时时间(ms)
    uint16_t    init_max_streams_bidi{64};  // 对端允许的双向流最大数量
    uint16_t    init_max_streams_uni{32};   // 对端允许的单向流最大数量
    uint8_t     ack_delay_exponent{3};      // ack延迟时间的指数, ack_delay = 2^ack_delay_exponent ms
    uint16_t    max_ack_delay{150};         // 150 ms
};

} // namespace utp
} // namespace eular

#endif // __UTP_TRANSPORT_PARAM_H__
