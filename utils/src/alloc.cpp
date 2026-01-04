/*************************************************************************
    > File Name: alloc.cpp
    > Author: hsz
    > Brief:
    > Created Time: Fri 25 Nov 2022 02:39:52 PM CST
 ************************************************************************/

#include "utils/alloc.h"

#include <stdio.h>
#include <stdint.h>

void *Malloc(size_t size)
{
    return ::malloc(size);
}

void Free(void *ptr)
{
    ::free(ptr);
}

void *Realloc(void *ptr, size_t size)
{
    return ::realloc(ptr, size);
}

void *AlignedMalloc(size_t size, size_t alignment)
{
    return AlignedRelloc(0, size, 0, alignment);
}

void *AlignedRelloc(void *oldptr, size_t newsize, size_t oldsize, size_t alignment)
{
    // QT 5.12.10 source code
    // fake an aligned allocation
    void *actualptr = oldptr ? static_cast<void **>(oldptr)[-1] : 0;
    int64_t oldoffset = 0;
    if (oldptr) {
        oldoffset = static_cast<char *>(oldptr) - static_cast<char *>(actualptr);
    }
    if (alignment <= sizeof(void*)) {
        // special, fast case
        void **newptr = static_cast<void **>(realloc(actualptr, newsize + sizeof(void*)));
        if (!newptr)
            return NULL;
        if (newptr == actualptr) {
            // realloc succeeded without reallocating
            return oldptr;
        }

        *newptr = newptr;
        return newptr + 1;
    }

    // malloc returns pointers aligned at least at sizeof(size_t) boundaries
    // but usually more (8- or 16-byte boundaries).
    // So we overallocate by alignment-sizeof(size_t) bytes, so we're guaranteed to find a
    // somewhere within the first alignment-sizeof(size_t) that is properly aligned.

    // However, we need to store the actual pointer, so we need to allocate actually size +
    // alignment anyway.

    void *real = realloc(actualptr, newsize + alignment);
    if (!real) {
        return NULL;
    }

    uint64_t faked = reinterpret_cast<uint64_t>(real) + alignment;
    faked &= ~(alignment - 1);
    void **faked_ptr = reinterpret_cast<void **>(faked);

    if (oldptr) {
        int64_t newoffset = reinterpret_cast<char *>(faked_ptr) - static_cast<char *>(real);
        if (oldoffset != newoffset)
            memmove(faked_ptr, static_cast<char *>(real) + oldoffset,
                (oldsize < newsize) ? oldsize : newsize);
    }

    // now save the value of the real pointer at faked-sizeof(void*)
    // by construction, alignment > sizeof(void*) and is a power of 2, so
    // faked-sizeof(void*) is properly aligned for a pointer
    faked_ptr[-1] = real;
    return faked_ptr;
}

void  AlignedFree(void *ptr)
{
    if (!ptr) {
        return;
    }

    void **ptr2 = static_cast<void **>(ptr);
    free(ptr2[-1]);
}
