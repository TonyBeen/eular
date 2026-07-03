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

void *AlignedAlloc(size_t size, size_t alignment)
{
    return AlignedRealloc(0, size, 0, alignment);
}

void *AlignedRealloc(void *oldptr, size_t newsize, size_t oldsize, size_t alignment)
{
    // QT 5.12.10 source code
    // 1. 找回真实的内存起始地址
    // 如果 oldptr 存在, 则它前一个位置存储的是 realloc 返回的原始指针 actualptr
    void *actualptr = oldptr ? static_cast<void **>(oldptr)[-1] : 0;

    int64_t oldoffset = 0;
    if (oldptr) {
        // 计算旧的用户指针相对于原始内存块起始位置的偏移量
        oldoffset = static_cast<char *>(oldptr) - static_cast<char *>(actualptr);
    }

    // 2. 特殊情况处理: 对齐要求小于等于指针大小 (如 64 位系统下 alignment <= 8)
    if (alignment <= sizeof(void*)) {
        // 只需要额外多申请一个指针的空间来存储 "真实地址"
        void **newptr = static_cast<void **>(realloc(actualptr, newsize + sizeof(void*)));
        if (!newptr)
            return NULL;

        if (newptr == actualptr) {
            // 如果 realloc 就在原地扩容, 地址没变, 直接返回原用户指针
            return oldptr;
        }

        // 在新块头部存下新块自己的地址, 并返回紧随其后的用户空间
        *newptr = newptr;
        return newptr + 1;
    }

    // 3. 通用对齐处理: 申请足够的冗余空间以确保能找到对齐点
    // 申请量 = 目标大小 + 对齐边界 (保证无论如何偏移都能在其中找到 alignment 的倍数地址)
    void *real = realloc(actualptr, newsize + alignment);
    if (!real) {
        return NULL;
    }

    // 4. 计算对齐后的用户指针地址
    uintptr_t faked = reinterpret_cast<uintptr_t>(real) + alignment;
    faked &= ~(alignment - 1); // 位运算: 将地址向下取整到 alignment 的整数倍
    void **faked_ptr = reinterpret_cast<void **>(faked);

    // 5. 关键步骤: 数据修正与迁移
    if (oldptr) {
        // 计算新的对齐偏移量
        int64_t newoffset = reinterpret_cast<char *>(faked_ptr) - static_cast<char *>(real);

        // 如果两次分配的"对齐偏移量"不同, 说明 realloc 虽然搬运了原始数据, 
        // 但数据相对于新对齐点的相对位置变了, 必须手动搬运数据
        if (oldoffset != newoffset)
            memmove(faked_ptr, static_cast<char *>(real) + oldoffset,
                (oldsize < newsize) ? oldsize : newsize);
    }

    // 6. 存储元数据: 在用户指针的前一个位置保存真实的原始内存地址
    // 这样做是为了让后续的释放或重新分配操作能找到 realloc/free 需要的原始指针
    faked_ptr[-1] = real;
    return faked_ptr;
}

void AlignedFree(void *ptr)
{
    if (!ptr) {
        return;
    }

    void **ptr2 = static_cast<void **>(ptr);
    free(ptr2[-1]);
}
