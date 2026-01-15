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

    uint16_t    base_mtu = 1200;
    int32_t     recv_buf_size = 1024 * 1024;
    int32_t     send_buf_size = 1024 * 1024;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONFIG_H__
