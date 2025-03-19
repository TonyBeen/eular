/*************************************************************************
    > File Name: kcp_protocol.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月20日 星期四 10时34分18秒
 ************************************************************************/

#include "kcp_protocol.h"

#include <string.h>

#include "kcp_config.h"
#include "kcp_endian.h"
#include "kcp_error.h"
#include "kcp_mtu.h"

static void on_mtu_probe_completed(kcp_connection_t *kcp_conn, uint32_t mtu, int32_t code)
{
    if (code == NO_ERROR) {
        int32_t ip_header_size = kcp_conn->remote_host.sa.sa_family == AF_INET6 ? IPV6_HEADER_SIZE : IPV4_HEADER_SIZE;
        kcp_conn->mtu = mtu - ip_header_size - UDP_HEADER_SIZE;
        kcp_conn->mss = kcp_conn->mtu - KCP_HEADER_SIZE;
    }
}

// KCP读事件回调, 用于接收数据
static void on_kcp_read_event(struct KcpConnection *kcp_conn, const kcp_proto_header_t *kcp_header)
{

}

// kcp写事件回调, 主要用于EAGAIN错误
static void on_kcp_write_event(struct KcpConnection *kcp_conn)
{

}

static void on_write_timeout_event()
{

}

void kcp_connection_init(kcp_connection_t *kcp_conn, const sockaddr_t *remote_host, struct KcpContext* kcp_ctx)
{
    kcp_conn->conv = KCP_CONV_FLAG;
    kcp_conn->kcp_ctx = NULL;
    kcp_conn->mss = kcp_get_mss(remote_host->sa.sa_family == AF_INET6);
    kcp_conn->mtu = kcp_conn->mss + KCP_HEADER_SIZE;
    kcp_conn->kcp_ctx = kcp_ctx;
    kcp_conn->syn_timer_event = NULL;
    kcp_conn->fin_timer_event = NULL;
    kcp_conn->write_timer_event = NULL;
    kcp_conn->ping_timer_event = NULL;
    kcp_conn->syn_retries = g_kcp_syn_retries;
    kcp_conn->fin_retries = g_kcp_syn_retries;
    kcp_conn->state = KCP_STATE_DISCONNECTED;
    kcp_conn->syn_fin_sn = 0;
    kcp_conn->syn_timeout = DEFAULT_SYN_TIMEOUT;
    kcp_conn->fin_timeout = DEFAULT_SYN_TIMEOUT;
    kcp_conn->ping_timeout = DEFAULT_PING_TIMEOUT;
    memcpy(&kcp_conn->remote_host, remote_host, sizeof(sockaddr_t));

    kcp_conn->nodelay = 0;      // 关闭nodelay
    kcp_conn->rx_minrto = IKCP_RTO_MIN;
    kcp_conn->interval = 40;    // 发送间隔 40ms
    kcp_conn->fastresend = 0;   // 关闭快速重传
    kcp_conn->nocwnd = 0;       // 开启拥塞控制

    list_init(&kcp_conn->snd_queue);
    list_init(&kcp_conn->snd_buf);
    list_init(&kcp_conn->snd_buf_unused);

    list_init(&kcp_conn->rcv_queue);
    list_init(&kcp_conn->rcv_buf);
    list_init(&kcp_conn->rcv_buf_unused);

    list_init(&kcp_conn->ack_item);
    list_init(&kcp_conn->ack_unused);

    kcp_conn->buffer = (char *)malloc(ETHERNET_MTU);

    kcp_conn->mtu_probe_ctx = (mtu_probe_ctx_t *)malloc(sizeof(mtu_probe_ctx_t));
    kcp_conn->mtu_probe_ctx->probe_buf = (char *)malloc(KCP_HEADER_SIZE + ETHERNET_MTU);
    kcp_conn->mtu_probe_ctx->mtu_last = ETHERNET_MTU_V4_MIN;
    kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
    kcp_conn->mtu_probe_ctx->on_probe_completed = on_mtu_probe_completed;

    kcp_conn->read_cb = on_kcp_read_event;
    kcp_conn->write_cb = on_kcp_write_event;
}

void kcp_connection_destroy(kcp_connection_t *kcp_conn)
{
    // 从红黑树中移除连接
    connection_set_erase(kcp_conn->kcp_ctx, kcp_conn->conv);
    int32_t index = (~KCP_CONV_FLAG) & kcp_conn->conv;
    bitmap_set(&kcp_conn->kcp_ctx->conv_bitmap, index, false);

    // 移除读事件
    if (!list_empty(&kcp_conn->node_list)) {
        list_del_init(&kcp_conn->node_list);
    }

    // 释放MTU探测资源
    if (kcp_conn->mtu_probe_ctx) {
        if (kcp_conn->mtu_probe_ctx->probe_buf) {
            free(kcp_conn->mtu_probe_ctx->probe_buf);
            kcp_conn->mtu_probe_ctx->probe_buf = NULL;
        }

        if (kcp_conn->mtu_probe_ctx->probe_timeout_event) {
            free(kcp_conn->mtu_probe_ctx->probe_timeout_event);
            kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
        }

        free(kcp_conn->mtu_probe_ctx);
        kcp_conn->mtu_probe_ctx = NULL;
    }

    // 释放超时事件
    if (kcp_conn->syn_timer_event) {
        event_del(kcp_conn->syn_timer_event);
        event_free(kcp_conn->syn_timer_event);
        kcp_conn->syn_timer_event = NULL;
    }

    free(kcp_conn);
}

int32_t kcp_proto_parse(kcp_proto_header_t *kcp_header, const char *data, size_t data_size)
{
    const char *data_offset = data;
    kcp_header->conv = le32toh(*(uint32_t *)data_offset); // 会话ID
    if (!(kcp_header->conv & KCP_CONV_FLAG)) {
        return INVALID_KCP_HEADER;
    }

    data_offset += 4;
    kcp_header->cmd = *(uint8_t *)(data_offset); // 命令
    data_offset += 1;
    kcp_header->frg = *(uint8_t *)(data_offset); // 分片
    data_offset += 1;
    kcp_header->wnd = le16toh(*(uint16_t *)(data_offset)); // 窗口大小
    data_offset += 2;
    kcp_header->ts = le32toh(*(uint32_t *)(data_offset)); // 时间戳
    data_offset += 4;
    kcp_header->sn = le32toh(*(uint32_t *)(data_offset)); // 序列号
    data_offset += 4;
    kcp_header->una = le32toh(*(uint32_t *)(data_offset)); // 未确认序列号
    data_offset += 4;
    kcp_header->len = le32toh(*(uint32_t *)(data_offset)); // 数据长度
    data_offset += 4;
    kcp_header->data = data_offset; // 数据

    if (kcp_header->len != (data_size - KCP_HEADER_SIZE)) {
        return INVALID_KCP_HEADER;
    }

    return NO_ERROR;
}

int32_t kcp_proto_header_encode(const kcp_proto_header_t *kcp_header, char *buffer, size_t buffer_size)
{
    if (buffer_size < (KCP_HEADER_SIZE + kcp_header->len)) {
        return BUFFER_TOO_SMALL;
    }

    char *buffer_offset = buffer;
    *(uint32_t *)buffer_offset = htole32(kcp_header->conv);
    buffer_offset += 4;
    *(uint8_t *)buffer_offset = kcp_header->cmd;
    buffer_offset += 1;
    *(uint8_t *)buffer_offset = kcp_header->frg;
    buffer_offset += 1;
    *(uint16_t *)buffer_offset = htole16(kcp_header->wnd);
    buffer_offset += 2;
    *(uint32_t *)buffer_offset = htole32(kcp_header->ts);
    buffer_offset += 4;
    *(uint32_t *)buffer_offset = htole32(kcp_header->sn);
    buffer_offset += 4;
    *(uint32_t *)buffer_offset = htole32(kcp_header->una);
    buffer_offset += 4;
    *(uint32_t *)buffer_offset = htole32(kcp_header->len);
    buffer_offset += 4;
    memcpy(buffer_offset, kcp_header->data, kcp_header->len);

    return (KCP_HEADER_SIZE + kcp_header->len);
}
