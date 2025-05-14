/*************************************************************************
    > File Name: kcp_net_def.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月14日 星期五 17时15分37秒
 ************************************************************************/

#ifndef __KCP_NET_DEF_H__
#define __KCP_NET_DEF_H__

#include <errno.h>

#include <kcp_def.h>

#if defined(OS_WINDOWS)
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#define SOCKADDR_STRING_LEN 128

typedef union {
    struct sockaddr     sa;
    struct sockaddr_in  sin;
    struct sockaddr_in6 sin6;
} sockaddr_t;

#endif // __KCP_NET_DEF_H__
