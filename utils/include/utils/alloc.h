/*************************************************************************
    > File Name: alloc.h
    > Author: hsz
    > Brief:
    > Created Time: Fri 25 Nov 2022 02:39:40 PM CST
 ************************************************************************/

#ifndef __UTILS_ALLOC_H__
#define __UTILS_ALLOC_H__

#include <stdlib.h>
#include <string.h>
#include <exception>

#define CHECK_PTR(p) if (!(p)) { throw std::bad_alloc(); }

void *Malloc(size_t size);
void  Free(void *ptr);
void *Realloc(void *ptr, size_t size);

void *AlignedAlloc(size_t size, size_t alignment);
void *AlignedRealloc(void *oldptr, size_t newsize, size_t oldsize, size_t alignment);
void  AlignedFree(void *ptr);

#endif // __UTILS_ALLOC_H__