/*************************************************************************
    > File Name: bitmap.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月21日 星期五 14时40分04秒
 ************************************************************************/

#include "bitmap.h"

#include <string.h>

#ifdef __CHAR_BIT__
#define BITS_PEER_BYTE      __CHAR_BIT__
#else
#define BITS_PEER_BYTE      (8)
#endif


bool bitmap_create(bitmap_t *bitmap, uint32_t size)
{
    if (bitmap == NULL) {
        return false;
    }

    bitmap->size = size;
    bitmap->capacity = size / BITS_PEER_BYTE + 1;
    bitmap->array = (uint8_t *)malloc(bitmap->capacity);
    if (bitmap->array == NULL) {
        return false;
    }

    return true;
}

void bitmap_destroy(bitmap_t *bitmap)
{
    if (bitmap == NULL) {
        return;
    }

    if (bitmap->array != NULL) {
        free(bitmap->array);
    }
    free(bitmap);
}

void bitmap_clear(bitmap_t *bitmap)
{
    memset(bitmap->array, 0, bitmap->capacity);
}

void bitmap_fill(bitmap_t *bitmap)
{
    memset(bitmap->array, 0xFF, bitmap->capacity);
}

bool bitset_copy(bitmap_t *pthis, const bitmap_t *other)
{
    if (pthis == NULL || other == NULL) {
        return false;
    }

    pthis->array = (uint8_t *)malloc(other->capacity);
    if (pthis->array == NULL) {
        return false;
    }

    pthis->size = other->size;
    pthis->capacity = other->capacity;
    memcpy(pthis->array, other->array, other->capacity);
    return true;
}

void bitmap_set(bitmap_t *bitmap, uint32_t index, bool value)
{
    if (index >= bitmap->size) {
        return;
    }

    uint32_t byte_index = index / BITS_PEER_BYTE;
    uint32_t bit_index = index % BITS_PEER_BYTE;
    uint8_t mask = 1 << bit_index;
    if (value) {
        bitmap->array[byte_index] |= mask;
    } else {
        bitmap->array[byte_index] &= ~mask;
    }
}

bool bitmap_get(bitmap_t *bitmap, uint32_t index)
{
    if (index >= bitmap->size) {
        return false;
    }

    uint32_t byte_index = index / BITS_PEER_BYTE;
    uint32_t bit_index = index % BITS_PEER_BYTE;
    uint8_t mask = 1 << bit_index;
    return (bitmap->array[byte_index] & mask) != 0;
}

static inline uint32_t popcount(uint32_t n)
{
#if defined(__linux__)
    return __builtin_popcount(n);
#else
    // https://blog.csdn.net/github_38148039/article/details/109598368
    n = (n & 0x55555555) + ((n >> 1) & 0x55555555);
    n = (n & 0x33333333) + ((n >> 2) & 0x33333333);
    n = (n & 0x0F0F0F0F) + ((n >> 4) & 0x0F0F0F0F);
    n = (n & 0x00FF00FF) + ((n >> 8) & 0x00FF00FF);
    n = (n & 0x0000FFFF) + ((n >> 16) & 0x0000FFFF);
    return n;
#endif
}

static inline uint64_t count_bits64(uint64_t v)
{
    return (popcount(v & UINT32_MAX) + popcount(v >> 32));
}

static inline uint64_t count_bits8(uint8_t v)
{
    return popcount(v);
}

uint32_t bitmap_count(bitmap_t *bitmap)
{
    uint32_t bytes =  bitmap->size / BITS_PEER_BYTE;
    uint32_t len = bytes / sizeof(uint64_t);
    uint32_t reserveSize = bytes % sizeof(uint64_t);
    uint32_t count = 0;
    uint32_t offset = 0;

    for (uint32_t i = 0; i < len; ++i) {
        uint64_t *pU64Vec = (uint64_t *)bitmap->array;
        uint64_t temp = pU64Vec[i];
        count += count_bits64(temp);
        offset += sizeof(uint64_t);
    }

    for (uint32_t i = 0; i < reserveSize; ++i) {
        if (bitmap->array[offset + i]) {
            count += count_bits8(bitmap->array[offset + i]);
        }
    }

    return count;
}
