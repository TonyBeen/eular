/*************************************************************************
    > File Name: types.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 03:32:43 PM CST
 ************************************************************************/

#ifndef __UTP_TYPES_H__
#define __UTP_TYPES_H__

#include <stdint.h>

typedef uint64_t    utp_time_t;
typedef uint64_t    utp_packno_t;

struct Range {
    utp_packno_t    low;
    utp_packno_t    high;
};

#endif // __UTP_TYPES_H__
