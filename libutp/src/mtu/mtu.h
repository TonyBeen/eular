/*************************************************************************
    > File Name: mtu.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 08:04:13 PM CST
 ************************************************************************/

#ifndef __UTP_MTU_MTU_H__
#define __UTP_MTU_MTU_H__

// 即只上探到 1500 字节的 MTU
#define UTP_ETHERNET_MTU    1500
#define IPV4_HEADER_SIZE    20
#define IPV6_HEADER_SIZE    40
#define UDP_HEADER_SIZE     8

#define ETHERNET_MTU_MIN    1280 // IPv6 minimum MTU
#define ETHERNET_MTU_MID    1400 // middle MTU

namespace eular {
namespace utp {

} // namespace utp
} // namespace eular

#endif // __UTP_MTU_MTU_H__
