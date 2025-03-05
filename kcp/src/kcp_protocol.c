/*************************************************************************
    > File Name: kcp_protocol.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月20日 星期四 10时34分18秒
 ************************************************************************/

#include "kcp_protocol.h"
#include "kcp_endian.h"
#include "kcp_error.h"
#include "kcp_mtu.h"

void kcp_connection_init(kcp_connection_t *kcp_conn, const sockaddr_t *remote_host, struct KcpContext* kcp_ctx)
{
    kcp_conn->kcp_ctx = kcp_ctx;
    kcp_conn->syn_timer_event = NULL;
    kcp_conn->syn_retries = 0;
    kcp_conn->state = KCP_STATE_DISCONNECTED;
    memcpy(&kcp_conn->remote_host, remote_host, sizeof(sockaddr_t));

    kcp_conn->mtu_probe_ctx = (mtu_probe_ctx_t *)malloc(sizeof(mtu_probe_ctx_t));
    kcp_conn->mtu_probe_ctx->mtu_last = ETHERNET_MTU_V4_MIN;
    kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
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
