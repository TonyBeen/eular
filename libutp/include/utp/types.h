/*************************************************************************
    > File Name: types.h
    > Author: eular
    > Brief: libutp 基础类型定义。
    > Created Time: Thu 29 Jan 2026 03:32:43 PM CST
 ************************************************************************/

#ifndef __UTP_TYPES_H__
#define __UTP_TYPES_H__

#include <stdint.h>
#include <inttypes.h>

/**
 * @typedef utp_time_t
 * @brief 时间戳类型
 */
typedef uint64_t    utp_time_t;

/**
 * @typedef utp_packno_t
 * @brief 包序号类型
 */
typedef uint64_t    utp_packno_t;

typedef unsigned long long  ull;
typedef long long           ll;

/**
 * @struct Range
 * @brief 包序号范围表示
 */
struct Range {
    utp_packno_t    low;    ///< 起始序号
    utp_packno_t    high;   ///< 结束序号
};

#endif // __UTP_TYPES_H__
