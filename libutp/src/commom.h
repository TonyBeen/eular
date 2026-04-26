/*************************************************************************
    > File Name: commom.h
    > Author: eular
    > Brief:
    > Created Time: Thu 08 Jan 2026 08:42:27 PM CST
 ************************************************************************/

#ifndef __UTP_COMMON_H__
#define __UTP_COMMON_H__

#include <memory>

#include "utp/platform.h"

#if defined(OS_WINDOWS)
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <ws2ipdef.h>
    #include <iphlpapi.h>
    #include <mswsock.h>
    #include "queue.h" // 3rd

    #ifdef X509_NAME
    #undef X509_NAME
    #endif

    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "mswsock.lib")

    typedef SOCKET      socket_t;
    typedef WSAMSG      msghdr_t;

#elif defined(OS_LINUX)
    #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
    #endif

    #ifndef __USE_GNU
    #define __USE_GNU
    #endif

    typedef int32_t     socket_t;
    typedef struct msghdr msghdr_t;
    #ifndef INVALID_SOCKET
    #define INVALID_SOCKET  (-1)
    #endif
    #ifndef SOCKET_ERROR
    #define SOCKET_ERROR    (-1)
    #endif

    #include <time.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <net/if.h>
    #include <ifaddrs.h>
    #include <linux/errqueue.h>
    #include <netinet/ip_icmp.h>
    #include <sys/queue.h>

    #ifndef SO_REUSEPORT
    #define SO_REUSEPORT 15
    #endif

    #ifndef SO_BINDTODEVICE
    #define SO_BINDTODEVICE	25
    #endif
#elif defined(OS_APPLE)
    typedef int32_t     socket_t;
    typedef struct msghdr msghdr_t;
    #ifndef INVALID_SOCKET
    #define INVALID_SOCKET  (-1)
    #endif
    #ifndef SOCKET_ERROR
    #define SOCKET_ERROR    (-1)
    #endif

    #include <time.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <net/if.h>
    #include <ifaddrs.h>
    #include <netinet/ip_icmp.h>
    #include <mach/mach_time.h>
    #include <sys/queue.h>
#endif

#endif // __UTP_COMMON_H__
