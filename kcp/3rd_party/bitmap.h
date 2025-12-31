/*************************************************************************
    > File Name: bitmap.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月21日 星期五 14时40分02秒
 ************************************************************************/

#ifndef __KCP_3RD_PARTY_BITMAP_H__
#define __KCP_3RD_PARTY_BITMAP_H__

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct BitMap {
    uint8_t*    array;
    uint32_t    size;
    uint32_t    capacity;
} bitmap_t;

bool bitmap_create(bitmap_t *bitmap, uint32_t size);

void bitmap_destroy(bitmap_t *bitmap);

void bitmap_clear(bitmap_t *bitmap);

void bitmap_fill(bitmap_t *bitmap);

bool bitset_copy(bitmap_t *pthis, const bitmap_t *other);

void bitmap_set(bitmap_t *bitmap, uint32_t index, bool value);

bool bitmap_get(bitmap_t *bitmap, uint32_t index);

uint32_t bitmap_count(bitmap_t *bitmap);

#endif // __KCP_3RD_PARTY_BITMAP_H__
