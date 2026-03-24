/*************************************************************************
    > File Name: config.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:59:29 PM CST
 ************************************************************************/

#ifndef __UTP_CONFIG_H__
#define __UTP_CONFIG_H__

#include <stdint.h>
#include <string>

#include <utp/platform.h>

namespace eular {
namespace utp {
class Config {
public:
    // dplpmtud
    bool        enable_dplpmtud = true;     // 开启dplpmtud
    uint16_t    mtu_min = 1280;             // MTU下探的最小值
    uint16_t    mtu_max = 1500;             // MTU上探的最大值
    uint16_t    mtu_base = 1400;            // 默认mtu
    uint32_t    mtu_probe_interval = 300;   // 探测间隔时间(seconds), 5min
    uint16_t    mtu_probe_step = 16;        // mtu探测步长(增大此值可加快探测速度但会降低精度)
    uint16_t    mtu_probe_timeout = 2000;   // mtu探测超时时间(ms)

    // keepalive
    bool        enable_keepalive = true;    // 开启keepalive
    uint32_t    keepalive_interval = 0;     // 本地保活探测间隔(ms). 0表示使用 max_idle_timeout 作为本地基准；实际发送间隔会与对端TP(max_idle_timeout-RTT裕量)取较小值
    uint32_t    keepalive_timeout = 1500;   // keepalive超时时间(ms)
    uint16_t    keepalive_probes = 3;       // keepalive探测次数
    uint32_t    max_idle_timeout = 30000;   // 本地默认空闲保活阈值(ms), 仅当 keepalive_interval=0 时作为本地基准

    // token / 0-rtt
    uint32_t    zero_rtt_token_max_lifetime = 600; // 0-RTT token 最长时效(s)
    uint32_t    zero_rtt_replay_window = 10;       // 0-RTT 抗重放去重窗口(s)

    // socket
    int32_t     recv_buf_size = 1024 * 1024;
    int32_t     send_buf_size = 1024 * 1024;

    // congestion control
    int32_t     cc_algorithm = 0;           // 拥塞控制算法, 0表示默认算法(bbr), 1表示BBR算法, 2表示Cubic算法
    uint32_t    clock_granularity_us = 1;   // pacer时钟粒度(us), 影响RTT的测量精度和拥塞控制的性能

    // ack
    uint8_t     ack_every_n_packets = 10;  // 连续收到多少个 ack-eliciting 包后立即回 Ack
    uint32_t    time_threshold_ms = 3;      // 时间阈值 = 3 * rtt
    uint8_t     max_ack_range_size = 149;   // 一个Ack帧可容纳的AckRange的数量
    uint8_t     ack_delay_exponent = 3;     // ack延迟指数，ack延迟时间 = FrameAck::ack_delay << ack_dely_exponent us
    uint16_t    ack_delay = 150;            // 最大ack延迟时间(ms)

    // tp
    uint16_t    handshake_timeout = 3000;   // 等待 HandshakeDown 超时时间(ms)
    uint16_t    init_max_streams_bidi = 64; // 初始双向流数量
    uint16_t    init_max_streams_uni = 32;  // 初始单向流数量
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONFIG_H__
