/*************************************************************************
    > File Name: ack_info.h
    > Author: eular
    > Brief:
    > Created Time: Fri 30 Jan 2026 10:37:29 AM CST
 ************************************************************************/

#ifndef __UTP_UTIL_ACK_INFO_H__
#define __UTP_UTIL_ACK_INFO_H__

#include <array>

#include "utp/types.h"

namespace eular {
namespace utp {
class AckInfo
{
public:
    AckInfo() = default;
    ~AckInfo() = default;

public:
    void reset();

public:
    utp_packno_t            largest_ack_packno{0};  // Ack帧中最大的ack包序号
    utp_time_t              ack_delay{0};           // Ack延迟
    uint32_t                range_size{0};          // Ack范围大小
    std::array<Range, 256>  ack_ranges{};           // Ack范围数组
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_ACK_INFO_H__
