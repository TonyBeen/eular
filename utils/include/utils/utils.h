/*************************************************************************
    > File Name: utils.h
    > Author: hsz
    > Mail:
    > Created Time: Wed May  5 14:55:59 2021
 ************************************************************************/

#ifndef __UTILS_FUNCTION_H__
#define __UTILS_FUNCTION_H__

#include <stdint.h>

#include <utils/sysdef.h>

#if defined(OS_LINUX) || defined(OS_APPLE)
#include <unistd.h>
#include <sys/syscall.h>
#else
#include <windows.h>
#endif

#include <utils/string8.h>

#ifndef gettid
    #if defined(OS_LINUX) || defined(OS_APPLE)
        #define gettid() (int32_t)syscall(__NR_gettid)
    #elif defined(OS_WINDOWS)
        #define gettid() (int32_t)GetCurrentThreadId()
    #endif
#endif

#ifdef DISALLOW_COPY_AND_ASSIGN
#undef DISALLOW_COPY_AND_ASSIGN
#endif
#define DISALLOW_COPY_AND_ASSIGN(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete;

#ifdef DISALLOW_MOVE
#undef DISALLOW_MOVE
#endif
#define DISALLOW_MOVE(ClassName) \
    ClassName(ClassName&&) = delete; \
    ClassName& operator=(ClassName&&) = delete;

#if COMPILER_TYPE == COMPILER_MSVC
#define eular_likely(cond)          (cond)
#define eular_unlikely(cond)        (cond)

#define eular_atomic_or(P, V)       InterlockedOr((volatile long*)(P), (V))             // p: 地址 V: 值，P指向的内容与V相或
#define eular_atomic_and(P, V)      InterlockedAnd((volatile long*)(P), (V))
#define eular_atomic_add(P, V)      InterlockedExchangeAdd((volatile long*)(P), (V))    // 前置++
#define eular_atomic_load(P)        InterlockedExchangeAdd((volatile long*)(P), (0))
#define eular_atomic_xadd(P, V)     InterlockedExchangeAdd((volatile long*)(P), (V))    // 后置++

 // P: 地址 O: 旧值 N: 新值; if (O == *P) { *p = N; return O} else { return *P }
#define cmpxchg(P, O, N)            InterlockedCompareExchange((volatile long*)(P), (N), (O))

#else

#define eular_likely(cond)          __builtin_expect(!!(cond), 1)       // 编译器优化，条件大概率成立
#define eular_unlikely(cond)        __builtin_expect(!!(cond), 0)       // 编译器优化，条件大概率不成立

#define eular_atomic_or(P, V)       __sync_or_and_fetch((P), (V))       // p: 地址 V: 值，P指向的内容与V相或
#define eular_atomic_and(P, V)      __sync_and_and_fetch((P), (V))
#define eular_atomic_add(P, V)      __sync_add_and_fetch((P), (V))      // 前置++
#define eular_atomic_load(P)        __sync_add_and_fetch((P), (0))
#define eular_atomic_xadd(P, V)     __sync_fetch_and_add((P), (V))      // 后置++

 // P: 地址 O: 旧值 N: 新值; if (O == *P) { *p = N; return O} else { return *P }
#define cmpxchg(P, O, N)            __sync_val_compare_and_swap((P), (O), (N))
#endif

#if defined(__x86_64__)
#define CPU_RELAX_NOP()             asm volatile("rep; nop\n": : :"memory")
#define CPU_RELAX_PAUSE()           asm volatile("pause" ::: "memory")
#else
#define CPU_RELAX_NOP()             Sleep(0)
#define CPU_RELAX_PAUSE()           Sleep(0)
#endif

#ifndef UNUSED
#define UNUSED(x) (void)x;
#endif

#endif // __UTILS_FUNCTION_H__
