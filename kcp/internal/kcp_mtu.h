#ifndef __KCP_MTU_H__
#define __KCP_MTU_H__

#include <stdint.h>
#include <stdbool.h>

#include <event2/event.h>

#include "kcp_def.h"
#include "kcp_net_def.h"
#include "kcp_protocol.h"

#define LOCALHOST_MTU       65536
#define ETHERNET_MTU        1500

#define ETHERNET_MTU_V4_MIN 576  // IPv4 minimum MTU
#define ETHERNET_MTU_V6_MIN 1280 // IPv6 minimum MTU

#define IPV4_HEADER_SIZE    20
#define IPV6_HEADER_SIZE    40
#define UDP_HEADER_SIZE     8

EXTERN_C_BEGIN

int32_t kcp_get_min_mss(bool ipv6);

int32_t kcp_get_mss(bool ipv6);

int32_t kcp_get_localhost_mss(bool ipv6);

int32_t kcp_mtu_probe(kcp_connection_t *kcp_conn, uint32_t timeout, uint16_t retry);

int32_t kcp_mtu_probe_received(kcp_connection_t *kcp_conn, const void *buffer, size_t len);

/**
 * @brief 当收到 ICMP Fragmentation Needed 错误时，调用此函数处理
 * 
 * @param kcp_ctx 
 * @param buffer 
 * @param len 
 */
void    kcp_process_icmp_fragmentation(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr, uint16_t next_hop_mtu);

/**
 * @brief 目标不可达
 * 
 * @param kcp_ctx 
 * @param buffer 
 * @param len 
 * @param remote_addr 
 */
void    kcp_process_icmp_unreach(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr);

void    kcp_process_icmp_error(struct KcpContext *kcp_ctx, const void *buffer, size_t len, const sockaddr_t *remote_addr);

EXTERN_C_END

#endif // __KCP_MTU_H__