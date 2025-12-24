/*************************************************************************
    > File Name: platform.h
    > Author: hsz
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:59:48 PM CST
 ************************************************************************/

#ifndef __UTP_PLATFORM_H__
#define __UTP_PLATFORM_H__

#if defined(WIN64) || defined(_WIN64)
    #define OS_WIN64
    #define OS_WIN32
    #define OS_WINDOWS
#elif defined(WIN32)|| defined(_WIN32)
    #define OS_WIN32
    #define OS_WINDOWS
#elif defined(linux) || defined(__linux) || defined(__linux__)
    #define OS_LINUX
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if defined(TARGET_OS_MAC) && TARGET_OS_MAC
        #define OS_MAC
    #elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        #define OS_IOS
    #endif
    #define OS_DARWIN
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    #define OS_FREEBSD
    #define OS_BSD
#elif defined(__NetBSD__)
    #define OS_NETBSD
    #define OS_BSD
#elif defined(__OpenBSD__)
    #define OS_OPENBSD
    #define OS_BSD
#elif defined(sun) || defined(__sun) || defined(__sun__)
    #define OS_SOLARIS
#else
    #error "unsupported system platform!"
#endif

#ifdef __cplusplus
    #define EXTERN_C_BEGIN extern "C" {
    #define EXTERN_C_END }
    #define DEFAULT(x) = x
#else
    #define EXTERN_C_BEGIN
    #define EXTERN_C_END
    #define DEFAULT(x)
#endif

#ifdef OS_WINDOWS
    #if defined(UTP_STATIC)
        #define UTP_API
    #else
        #if defined(UTP_EXPORTS)
            #define UTP_API __declspec(dllexport)
        #else
            #define UTP_API __declspec(dllimport)
        #endif
    #endif
#else
    #define UTP_API __attribute__((visibility("default")))
#endif

#if defined(__cplusplus) && __cplusplus >= 201103L
    #define THREAD_LOCAL thread_local
#else
    #ifdef _MSC_VER
        #define THREAD_LOCAL __declspec(thread)
    #else
        #define THREAD_LOCAL __thread
    #endif
#endif

// 让x介于min和max之间, x ∈ [min, max]
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#endif // __UTP_PLATFORM_H__
