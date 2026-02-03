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
    uint32_t    keepalive_interval = 0;     // keepalive间隔时间(ms), 指定时间内未发送数据则发送keepalive包. 0表示采用 kMaxIdleTimeout - 3 * RTT
    uint32_t    keepalive_timeout = 1500;   // keepalive超时时间(ms)
    uint16_t    keepalive_probes = 3;       // keepalive探测次数

    // socket
    int32_t     recv_buf_size = 1024 * 1024;
    int32_t     send_buf_size = 1024 * 1024;

    // ack
    uint32_t    packet_no_delta = 10;       // 包号差阈值
    uint32_t    time_threshold_ms = 3;      // 时间阈值 = 3 * rtt
    uint32_t    max_ack_range_size = 149;   // 一个Ack帧可容纳的AckRange的数量
    uint8_t     ack_dely_exponent = 3;      // ack延迟指数，ack延迟时间 = ack_delay << ack_dely_exponent us
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONFIG_H__
