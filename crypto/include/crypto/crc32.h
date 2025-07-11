/*************************************************************************
    > File Name: crc32.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 20 Jul 2023 10:39:55 AM CST
 ************************************************************************/

#ifndef __CRC32_H__
#define __CRC32_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t crc32(uint64_t crc, const void *buf, uint32_t len);

uint64_t crc32_combine64(uint64_t crc1, uint64_t crc2, int64_t len2);

#ifdef __cplusplus
}
#endif

#endif // __CRC32_H__