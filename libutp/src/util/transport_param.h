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
        kMaxIdleTimeout                 = 1u << 0, // 对端声明的空闲保活阈值(ms)
        kHandshakeTimeout               = 1u << 1, // 握手超时时间(ms)
        kInitMaxStreamsBidi             = 1u << 2, // 初始双向流最大数量
        kInitMaxStreamsUni              = 1u << 3, // 初始单向流最大数量
        kAckDelayExponent               = 1u << 4, // ack延迟指数
        kInitialMaxData                 = 1u << 5, // 连接级初始流量控制窗口
        kInitialMaxStreamDataBidiLocal  = 1u << 6, // 对端可向本端发起的双向流初始窗口
        kInitialMaxStreamDataBidiRemote = 1u << 7, // 本端可向对端发起的双向流初始窗口
        kMaxNumeric                     = 8,        // 参数个数
    };
    static const uint16_t kDefaultFlags =
          kMaxIdleTimeout
        | kHandshakeTimeout
        | kInitMaxStreamsBidi
        | kInitMaxStreamsUni
        | kAckDelayExponent
        | kInitialMaxData
        | kInitialMaxStreamDataBidiLocal
        | kInitialMaxStreamDataBidiRemote;

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
        case kInitialMaxData:
            initial_max_data = value;
            break;
        case kInitialMaxStreamDataBidiLocal:
            initial_max_stream_data_bidi_local = value;
            break;
        case kInitialMaxStreamDataBidiRemote:
            initial_max_stream_data_bidi_remote = value;
            break;
        default:
            return;
        }

        flags |= static_cast<uint16_t>(param);
    }

    void clearParam(int32_t mask);

public:
    uint16_t    flags{kDefaultFlags};       // 参数启用标志, 每位表示一个参数是否启用
    uint32_t    max_idle_timeout{600000};   // 协商给对端的空闲保活阈值(ms)，本端发送间隔会参考该值并预留RTT裕量
    uint16_t    handshake_timeout{5000};    // 对端允许握手超时时间(ms)
    uint16_t    init_max_streams_bidi{64};  // 对端允许的双向流最大数量
    uint16_t    init_max_streams_uni{32};   // 对端允许的单向流最大数量
    uint8_t     ack_delay_exponent{3};      // ack延迟时间的指数, ack_delay = 2^ack_delay_exponent ms
    uint64_t    initial_max_data{64ull * 1024ull * 1024ull};                    // 协商给对端的连接级初始流量控制窗口
    uint64_t    initial_max_stream_data_bidi_local{16ull * 1024ull * 1024ull};  // 协商给对端的双向流本地初始接收窗口
    uint64_t    initial_max_stream_data_bidi_remote{16ull * 1024ull * 1024ull}; // 协商给对端的双向流远端初始接收窗口
};

} // namespace utp
} // namespace eular

#endif // __UTP_TRANSPORT_PARAM_H__
