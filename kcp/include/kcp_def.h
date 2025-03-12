#ifndef __KCP_DEF_H__
#define __KCP_DEF_H__

#ifdef __cplusplus
    #define EXTERN_C_BEGIN extern "C" {
    #define EXTERN_C_END }
    #define DEFAULT(x) = x
#else
    #define EXTERN_C_BEGIN
    #define EXTERN_C_END
    #define DEFAULT(x)
#endif

#if defined(WIN64) || defined(_WIN64)
    #define OS_WIN64
    #define OS_WIN32
    #define OS_WINDOWS
#elif defined(WIN32)|| defined(_WIN32)
    #define OS_WIN32
    #define OS_WINDOWS
#elif defined(ANDROID) || defined(__ANDROID__)
    #define OS_ANDROID
    #define OS_LINUX
#elif defined(linux) || defined(__linux) || defined(__linux__)
    #define OS_LINUX
#elif defined(__APPLE__) && (defined(__GNUC__) || defined(__xlC__) || defined(__xlc__))
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

#if defined(__i386) || defined(__i386__) || defined(_M_IX86)
    #define ARCH_X86
    #define ARCH_X86_32
#elif defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(_M_X64)
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

#if defined(OS_WINDOWS)

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#include <Window.h>
typedef SOCKET socket_t;
typedef SSIZE_T ssize_t;

#define LIB_EXPORT __declspec(dllexport)
#define LIB_IMPORT __declspec(dllimport)

#if defined(KCP_LIBRARY)
#define KCP_PORT LIB_EXPORT
#else
#define KCP_PORT LIB_IMPORT
#endif

#else

#define KCP_PORT
typedef int socket_t;
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)

#endif

#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a): (b))
#endif

#ifndef ABS
#define ABS(n)  ((n) > 0 ? (n) : -(n))
#endif

#endif // __KCP_DEF_H__
