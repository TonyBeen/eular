/*************************************************************************
    > File Name: kcp_protocol.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月20日 星期四 10时34分18秒
 ************************************************************************/

#include "kcp_protocol.h"

#include <string.h>

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

void kcp_connection_init(kcp_connection_t *kcp_conn, const sockaddr_t *remote_host, struct KcpContext* kcp_ctx)
{
    kcp_conn->conv = KCP_CONV_FLAG;
    kcp_conn->mss = kcp_get_mss(remote_host->sa.sa_family == AF_INET6);
    kcp_conn->mtu = kcp_conn->mss + KCP_HEADER_SIZE;
    kcp_conn->kcp_ctx = kcp_ctx;
    kcp_conn->syn_timeout_event = NULL;
    kcp_conn->syn_retries = 2;
    kcp_conn->state = KCP_STATE_DISCONNECTED;
    memcpy(&kcp_conn->remote_host, remote_host, sizeof(sockaddr_t));

    kcp_conn->mtu_probe_ctx = (mtu_probe_ctx_t *)malloc(sizeof(mtu_probe_ctx_t));
    kcp_conn->mtu_probe_ctx->probe_buf = (char *)malloc(KCP_HEADER_SIZE + ETHERNET_MTU);
    kcp_conn->mtu_probe_ctx->mtu_last = ETHERNET_MTU_V4_MIN;
    kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
    kcp_conn->mtu_probe_ctx->on_probe_completed = on_mtu_probe_completed;

    // TODO 实现内部读写回调
    kcp_conn->read_cb = NULL;
    kcp_conn->write_cb = NULL;
}

void kcp_connection_destroy(kcp_connection_t *kcp_conn)
{
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

    if (kcp_conn->syn_timeout_event) {
        event_del(kcp_conn->syn_timeout_event);
        event_free(kcp_conn->syn_timeout_event);
        kcp_conn->syn_timeout_event = NULL;
    }
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
