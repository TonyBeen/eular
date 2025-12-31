/*************************************************************************
    > File Name: packet_common.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 11 Dec 2025 10:29:33 AM CST
 ************************************************************************/

#ifndef __PROTO_PACKET_COMMON_H__
#define __PROTO_PACKET_COMMON_H__

#define UTP_MAX_PACKNO ((1ull << 62 ) - 1) // 2^62 - 1
#define UTP_INVALID_PACKNO (UTP_MAX_PACKNO + 1)

static inline bool IsValidPackNo(uint64_t packno) {
    return packno <= UTP_MAX_PACKNO;
}

#endif // __PROTO_PACKET_COMMON_H__
