/*************************************************************************
    > File Name: kcp_protocol.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月20日 星期四 10时34分18秒
 ************************************************************************/

#include "kcp_protocol.h"

void kcp_connection_init(kcp_connection_t *kcp_conn, sockaddr_t remote_host, struct KcpContext* kcp_ctx)
{
    kcp_conn->kcp_ctx = kcp_ctx;
    kcp_conn->syn_timer_event = NULL;
    kcp_conn->syn_retries = 0;
    kcp_conn->state = KCP_STATE_DISCONNECTED;
    memcpy(&kcp_conn->remote_host, &remote_host, sizeof(sockaddr_t));
}