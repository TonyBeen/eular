/*************************************************************************
    > File Name: bitmap.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月21日 星期五 14时40分04秒
 ************************************************************************/

#include "bitmap.h"

bool bitmap_create(bitmap_t *bitmap, uint32_t size)
{
    if (bitmap == NULL) {
        return false;
    }

    bitmap->size = size;
    bitmap->capacity = size / 8 + 1;
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

    uint32_t byte_index = index / 8;
    uint32_t bit_index = index % 8;
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

    uint32_t byte_index = index / 8;
    uint32_t bit_index = index % 8;
    uint8_t mask = 1 << bit_index;
    return (bitmap->array[byte_index] & mask) != 0;
}