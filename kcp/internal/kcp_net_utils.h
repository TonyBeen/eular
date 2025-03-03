#ifndef __KCP_NET_UTILS_H__
#define __KCP_NET_UTILS_H__

#include <stdint.h>
#include <kcp_def.h>
#include <kcp_net_def.h>

EXTERN_C_BEGIN

int32_t set_socket_nonblock(socket_t fd);
int32_t set_socket_reuseaddr(socket_t fd);
int32_t set_socket_reuseport(socket_t fd);
int32_t set_socket_dont_fragment(socket_t fd);
int32_t set_socket_bind_nic(socket_t fd, const char *nic);
int32_t set_socket_recverr(socket_t fd, const sockaddr_t *addr);

EXTERN_C_END

#endif // __KCP_NET_UTILS_H__
