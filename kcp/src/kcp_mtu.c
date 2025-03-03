#include "kcp_mtu.h"

#include <event2/event.h>

#include "kcp_error.h"

#define GRE_HEADER_SIZE     24
#define PPPOE_HEADER_SIZE   8
#define MPPE_HEADER_SIZE    2
// packets have been observed in the wild that were fragmented
// with a payload of 1416 for the first fragment
// There are reports of routers that have MTU sizes as small as 1392
#define FUDGE_HEADER_SIZE       36
#define ETHERNET_MTU_V4_MIN     68   // IPv4 minimum MTU
#define ETHERNET_MTU_V6_MIN     1280 // IPv6 minimum MTU

#define UDP_IPV4_OVERHEAD (IPV4_HEADER_SIZE + UDP_HEADER_SIZE)
#define UDP_IPV6_OVERHEAD (IPV6_HEADER_SIZE + UDP_HEADER_SIZE)
#define UDP_TEREDO_OVERHEAD (UDP_IPV4_OVERHEAD + UDP_IPV6_OVERHEAD)

#define UDP_IPV4_MTU (ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE - PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_IPV6_MTU (ETHERNET_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE - PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_TEREDO_MTU (ETHERNET_MTU_V6_MIN - IPV6_HEADER_SIZE - UDP_HEADER_SIZE)

int32_t kcp_get_min_mss(bool ipv6)
{
    if (ipv6) {
        return ETHERNET_MTU_V6_MIN - IPV6_HEADER_SIZE - UDP_HEADER_SIZE;
    } else {
        return ETHERNET_MTU_V4_MIN - IPV4_HEADER_SIZE - UDP_HEADER_SIZE;
    }
}

int32_t kcp_get_mss(bool ipv6)
{
    if (ipv6) {
        return UDP_TEREDO_MTU;
    } else {
        return UDP_IPV4_MTU;
    }
}

int32_t kcp_get_localhost_mss(bool ipv6)
{
    if (ipv6) {
        return LOCALHOST_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE;
    } else {
        return LOCALHOST_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE;
    }
}

static void kcp_mtu_probe_timeout_cb(evutil_socket_t fd, short event, void *arg)
{
    kcp_connection_t *kcp_conn = (kcp_connection_t *)arg;
    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;
    if (probe_ctx == NULL) {
        return;
    }

    if (probe_ctx->retries == 0) {
        evtimer_del(probe_ctx->probe_timeout_event);
        return;
    }

    probe_ctx->retries--;

    // TODO send probe packet
    // kcp_send_probe_packet(probe_ctx->kcp_conn);

    evtimer_add(probe_ctx->probe_timeout_event, probe_ctx->timeout);
}

int32_t kcp_mtu_probe(kcp_connection_t *kcp_conn, uint32_t timeout, uint16_t retry)
{
    mtu_probe_ctx_t *probe_ctx = kcp_conn->mtu_probe_ctx;
    kcp_conn->mtu_probe_ctx = probe_ctx;
    probe_ctx->mtu_best = ETHERNET_MTU_V4_MIN;
    probe_ctx->mtu_lbound = ETHERNET_MTU_V4_MIN;
    probe_ctx->mtu_ubound = LOCALHOST_MTU;
    probe_ctx->timeout = timeout;
    probe_ctx->retries = retry;
    if (probe_ctx->probe_timeout_event == NULL) {
        probe_ctx->probe_timeout_event = evtimer_new(kcp_conn->kcp_ctx->event_loop, kcp_mtu_probe_timeout_cb, kcp_conn);
    }
    if (probe_ctx->probe_timeout_event == NULL) {
        return NO_MEMORY;
    }

    struct timeval tv = {timeout / 1000, (timeout % 1000) * 1000};
    return evtimer_add(probe_ctx->probe_timeout_event, &tv);
}

int32_t kcp_mtu_probe_received(kcp_connection_t *kcp_conn, const void *buffer, size_t len)
{

}