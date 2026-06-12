/*************************************************************************
    > File Name: base.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 10时52分51秒
 ************************************************************************/

#ifndef __EVENT_BASE_H__
#define __EVENT_BASE_H__

#ifdef _WIN32
#include <winsock2.h>
#define socket_t SOCKET
#else
#include <sys/socket.h>
#define socket_t int32_t
#undef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif

#ifndef DISALLOW_COPY_AND_ASSIGN
#define DISALLOW_COPY_AND_ASSIGN(ClassName)             \
    ClassName(const ClassName&) = delete;               \
    ClassName& operator=(const ClassName&) = delete

#endif

#ifndef DISALLOW_MOVE
#define DISALLOW_MOVE(ClassName)                    \
    ClassName(ClassName &&) = delete;               \
    ClassName& operator=(ClassName &&) = delete

#endif

// libevent struct
struct event;
struct event_base;

#if defined(_WIN32) || defined(_WIN64)
    #ifdef EVENT_WRAPPER_SHARED
        #define EVENT_WRAPPER_API __declspec(dllexport)
    #elif defined(EVENT_WRAPPER_STATIC)
        #define EVENT_WRAPPER_API
    #else
        #define EVENT_WRAPPER_API __declspec(dllimport)
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #define EVENT_WRAPPER_API __attribute__((visibility("default")))
#else
    #define EVENT_WRAPPER_API
#endif

#endif // __EVENT_BASE_H__
