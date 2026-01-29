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

namespace eular {
namespace utp {
class Config
{
public:
    static Config *Instance() {
        static Config instance;
        return &instance;
    }

    uint16_t    base_mtu = 1400;            // default mtu
    bool        enable_dplpmtud = true;     // enable dplpmtud
    int32_t     recv_buf_size = 1024 * 1024;
    int32_t     send_buf_size = 1024 * 1024;

    // ack
    uint32_t    packet_no_threshold = 10;   // 包号差阈值
    uint32_t    time_threshold_ms = 3;      // 时间阈值 = 3 * rtt
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONFIG_H__
