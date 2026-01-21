/*************************************************************************
    > File Name: sysdef.h
    > Author: hsz
    > Mail:
    > Created Time: Mon 27 Sep 2021 08:53:20 AM CST
 ************************************************************************/

#ifndef __UTILS_SYSDEF_H__
#define __UTILS_SYSDEF_H__

#include <stdint.h>
#include <string.h>

#if defined(WIN64) || defined(_WIN64)
    #define OS_WIN64
    #define OS_WIN32
#elif defined(WIN32)|| defined(_WIN32)
    #define OS_WIN32
#elif defined(ANDROID) || defined(__ANDROID__)
    #define OS_ANDROID
    #define OS_LINUX
#elif defined(linux) || defined(__linux) || defined(__linux__)
    #define OS_LINUX
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if defined(TARGET_OS_MAC) && TARGET_OS_MAC
        #define OS_APPLE
    #endif
    #if defined(TARGET_OS_OSX) && TARGET_OS_OSX
        #define OS_MACOS
    #elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
        #define OS_IOS
    #else
        #error "unsupported Apple platform!"
        // TARGET_OS_MAC	    在任意 Apple 平台下都为 1。
        // TARGET_OS_IPHONE	    编译目标为 iOS（含 watchOS、tvOS）
        // TARGET_OS_TV	        编译目标为 tvOS
        // TARGET_OS_WATCH	    编译目标为 watchOS
        // TARGET_OS_SIMULATOR	目标是模拟器（不是真机）
    #endif
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

#if defined(OS_WIN32) || defined(OS_WIN64)
    #undef  OS_UNIX
    #define OS_WINDOWS

    #define DIR_SEPARATOR       '\\'
    #define DIR_SEPARATOR_STR   "\\"
#else
    #define OS_UNIX
    #define DIR_SEPARATOR       '/'
    #define DIR_SEPARATOR_STR   "/"
#endif

#ifndef __FILE_NAME__
    #ifdef __BASE_FILE__
        #define __FILE_NAME__   __BASE_FILE__
    #else
        #define __FILE_NAME__  (strrchr(DIR_SEPARATOR_STR __FILE__, DIR_SEPARATOR) + 1)
    #endif
#endif

// #if defined(__x86_64__) || defined(_M_X64) || defined(__powerpc64__) || defined(__alpha__) ||
// defined(__ia64__) || defined(__s390__) || defined(__s390x__) || defined(_LP64) || defined(__LP64__)
// ARCH
#if defined(__i386) || defined(__i386__) || defined(_M_IX86)
    #define ARCH_X86
    #define ARCH_X86_32
#elif defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(_M_X64) || defined(_M_AMD64)
    #define ARCH_X64
    #define ARCH_X86_64
#elif defined(__arm__)
    #define ARCH_ARM
#elif defined(__aarch64__) || defined(__ARM64__)
    #define ARCH_ARM64
#elif defined(__mips__)
    #define ARCH_MIPS
#elif defined(__mips64__)
    #define ARCH_MIPS64
#else
    #error "unsupported hardware architecture!"
#endif

#ifdef OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef _CRT_NONSTDC_NO_DEPRECATE
    #define _CRT_NONSTDC_NO_DEPRECATE
    #endif
    #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
    #endif
    #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
    #endif

    typedef long ssize_t;
#endif

#define COMPILER_MSVC       1
#define COMPILER_GNUC       2
#define COMPILER_CLANG      3
#define COMPILER_APPLECLANG 4

#if defined( __clang__ )
#if defined(__apple_build_version__)
    #define COMPILER_TYPE       COMPILER_APPLECLANG
#else
    #define COMPILER_TYPE       COMPILER_CLANG
#endif
    #define COMPILER_VERSION    (((__clang_major__) * 100) + (__clang_minor__ * 10) + __clang_patchlevel__)
#elif defined( __GNUC__ )
    #define COMPILER_TYPE       COMPILER_GNUC
    #define COMPILER_VERSION    (((__GNUC__) * 100) + (__GNUC_MINOR__ * 10) + __GNUC_PATCHLEVEL__)

    #ifndef _GNU_SOURCE
    #define _GNU_SOURCE 1
    #endif
#elif defined( _MSC_VER )
    #define COMPILER_TYPE       COMPILER_MSVC
    #define COMPILER_VERSION    _MSC_VER

    #if (_MSC_VER < 1200) // Visual C++ 6.0
    #define MSVS_VERSION    1998
    #define MSVC_VERSION    60
    #elif (_MSC_VER >= 1200) && (_MSC_VER < 1300) // Visual Studio 2002, MSVC++ 7.0
    #define MSVS_VERSION    2002
    #define MSVC_VERSION    70
    #elif (_MSC_VER >= 1300) && (_MSC_VER < 1400) // Visual Studio 2003, MSVC++ 7.1
    #define MSVS_VERSION    2003
    #define MSVC_VERSION    71
    #elif (_MSC_VER >= 1400) && (_MSC_VER < 1500) // Visual Studio 2005, MSVC++ 8.0
    #define MSVS_VERSION    2005
    #define MSVC_VERSION    80
    #elif (_MSC_VER >= 1500) && (_MSC_VER < 1600) // Visual Studio 2008, MSVC++ 9.0
    #define MSVS_VERSION    2008
    #define MSVC_VERSION    90
    #elif (_MSC_VER >= 1600) && (_MSC_VER < 1700) // Visual Studio 2010, MSVC++ 10.0
    #define MSVS_VERSION    2010
    #define MSVC_VERSION    100
    #elif (_MSC_VER >= 1700) && (_MSC_VER < 1800) // Visual Studio 2012, MSVC++ 11.0
    #define MSVS_VERSION    2012
    #define MSVC_VERSION    110
    #elif (_MSC_VER >= 1800) && (_MSC_VER < 1900) // Visual Studio 2013, MSVC++ 12.0
    #define MSVS_VERSION    2013
    #define MSVC_VERSION    120
    #elif (_MSC_VER >= 1900) && (_MSC_VER < 1910) // Visual Studio 2015, MSVC++ 14.0
    #define MSVS_VERSION    2015
    #define MSVC_VERSION    140
    #elif (_MSC_VER >= 1910) && (_MSC_VER < 1920) // Visual Studio 2017, MSVC++ 15.0
    #define MSVS_VERSION    2017
    #define MSVC_VERSION    150
    #elif (_MSC_VER >= 1920) && (_MSC_VER < 2000) // Visual Studio 2019, MSVC++ 16.0
    #define MSVS_VERSION    2019
    #define MSVC_VERSION    160
    #endif

    #pragma warning (disable: 4018) // signed/unsigned comparison
    #pragma warning (disable: 4100) // unused param
    #pragma warning (disable: 4102) // unreferenced label
    #pragma warning (disable: 4244) // conversion loss of data
    #pragma warning (disable: 4267) // size_t => int
    #pragma warning (disable: 4819) // Unicode
    #pragma warning (disable: 4996) // _CRT_SECURE_NO_WARNINGS
    #pragma warning (disable: 4251) // STL dll
    #pragma warning (disable: 4275) // dll-interface
#else
    #error "Unknown compiler."
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

#if COMPILER_TYPE == COMPILER_MSVC
    #ifndef UTILS_STATIC
        #ifdef UTILS_EXPORTS
            #define UTILS_API __declspec(dllexport)
        #else
            #define UTILS_API __declspec(dllimport)
        #endif
    #else
        #define UTILS_API
    #endif
#elif COMPILER_TYPE == COMPILER_GNUC || COMPILER_TYPE == COMPILER_CLANG || COMPILER_TYPE == COMPILER_APPLECLANG
    #ifndef UTILS_STATIC
        #define UTILS_API __attribute__((visibility("default")))
    #else
        #define UTILS_API
    #endif
#else
    #error "Unknown compiler type."
#endif

#if defined(OS_LINUX) || defined(OS_APPLE)
    #define PRETTY_FUNCTION     __PRETTY_FUNCTION__
    #define THREAD_LOCAL        __thread
#elif defined(OS_WINDOWS)
    #define PRETTY_FUNCTION     __FUNCSIG__
    #define THREAD_LOCAL        __declspec(thread)
#endif

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
    #ifndef WEAK_FUNCTION
    #define WEAK_FUNCTION __attribute__((weak))
    #endif

    #ifndef NORETURN
    #define NORETURN __attribute__((__noreturn__))
    #endif
#else
    #define NORETURN
    #define WEAK_FUNCTION
#endif

#ifdef __has_builtin
#define EULAR_HAVE_BUILTIN(x) __has_builtin(x)
#else
#define EULAR_HAVE_BUILTIN(x) 0
#endif

#ifdef __has_feature
#define EULAR_HAVE_FEATURE(f) __has_feature(f)
#else
#define EULAR_HAVE_FEATURE(f) 0
#endif

#if EULAR_HAVE_BUILTIN(__builtin_expect)
    #define eular_likely(cond)          __builtin_expect(!!(cond), 1)   // 编译器优化，条件大概率成立
    #define eular_unlikely(cond)        __builtin_expect(!!(cond), 0)   // 编译器优化，条件大概率不成立
#else
    #define eular_likely(cond)          (cond)
    #define eular_unlikely(cond)        (cond)
#endif

#endif // __UTILS_SYSDEF_H__
