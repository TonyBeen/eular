/*************************************************************************
    > File Name: kcp_protocol.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月20日 星期四 10时34分18秒
 ************************************************************************/

#include "kcp_protocol.h"

#include <string.h>
#include <assert.h>

#include "kcp_config.h"
#include "kcp_endian.h"
#include "kcp_error.h"
#include "kcp_mtu.h"
#include "kcp_net_utils.h"
#include "kcp_log.h"

static void on_mtu_probe_completed(kcp_connection_t *kcp_conn, uint32_t mtu, int32_t code)
{
    if (code == NO_ERROR) {
        int32_t ip_header_size = kcp_conn->remote_host.sa.sa_family == AF_INET6 ? IPV6_HEADER_SIZE : IPV4_HEADER_SIZE;
        kcp_conn->mtu = mtu - ip_header_size - UDP_HEADER_SIZE;
        kcp_conn->mss = kcp_conn->mtu - KCP_HEADER_SIZE;
    }
}

// KCP读事件回调, 用于接收数据
static void on_kcp_read_event(struct KcpConnection *kcp_connection, const kcp_proto_header_t *kcp_header, const sockaddr_t *remote_host)
{
    if (!sockaddr_equal(&kcp_connection->remote_host, remote_host)) {
        KCP_LOGW("remote host not match, ignore packet");
        return;
    }

    if (kcp_header->cmd == KCP_CMD_RST) {
        switch (kcp_connection->state) {
        case KCP_STATE_SYN_SENT: // client
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_connected) {
                kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, CONNECTION_REFUSED);
            }
            break;
        case KCP_STATE_SYN_RECEIVED: // server
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_accepted) {
                kcp_connection->kcp_ctx->callback.on_accepted(kcp_connection->kcp_ctx, kcp_connection, CONNECTION_RESET);
            }
            break;
        default:
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_closed) {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, CONNECTION_RESET);
            }
            break;
        }

        kcp_connection_destroy(kcp_connection);
        return;
    }

    bool send_rst = false;
    int32_t kcp_state = kcp_connection->state;
    switch (kcp_connection->state) {
    case KCP_STATE_DISCONNECTED:
        KCP_LOGE("KCP_STATE_DISCONNECTED state, ignore packet");
        break;
    case KCP_STATE_SYN_SENT: // client
    case KCP_STATE_SYN_RECEIVED: { // server
        if (kcp_header->cmd == KCP_CMD_ACK || kcp_header->cmd == KCP_CMD_PUSH) {
            kcp_connection->state = KCP_STATE_CONNECTED;
            // 通知连接建立
            if (kcp_connection->kcp_ctx->callback.on_accepted) {
                kcp_connection->kcp_ctx->callback.on_accepted(kcp_connection->kcp_ctx, kcp_connection, NO_ERROR);
            } else if (kcp_connection->kcp_ctx->callback.on_connected) {
                kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, NO_ERROR);
            }

            // NOTE 服务端开启MTU后, 客户端无须开启
            kcp_connection->need_write_timer_event = true;
            kcp_mtu_probe(kcp_connection, DEFAULT_MTU_PROBE_TIMEOUT, 1);
        } else {
            send_rst = true;
        }
        break;
    }
    case KCP_STATE_CONNECTED: {
        kcp_input_pcaket(kcp_connection, kcp_header);
        break;
    }
    case KCP_STATE_FIN_SENT: {
        if (kcp_header->cmd == KCP_CMD_FIN) {
            kcp_proto_header_t kcp_ack_header;
            kcp_ack_header.conv = kcp_connection->conv;
            kcp_ack_header.cmd = KCP_CMD_ACK;
            kcp_ack_header.frg = 0;
            kcp_ack_header.wnd = 0;
            kcp_ack_header.ack_data.packet_ts = kcp_header->packet_data.ts;
            kcp_ack_header.ack_data.ack_ts = kcp_time_monotonic_us();
            kcp_ack_header.ack_data.sn = kcp_header->packet_data.sn;
            kcp_ack_header.ack_data.una = 0;

            char buffer[KCP_HEADER_SIZE] = {0};
            kcp_proto_header_encode(&kcp_ack_header, buffer, KCP_HEADER_SIZE);
            struct iovec data[1];
            data[0].iov_base = buffer;
            data[0].iov_len = KCP_HEADER_SIZE;

            kcp_send_packet(kcp_connection, &data, sizeof(data));
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_closed) {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
            }
        } else 
        break;
    }
    case KCP_STATE_FIN_RECEIVED: {
        if (kcp_header->cmd == KCP_CMD_FIN) { // 对端未收到FIN包
            kcp_proto_header_t kcp_fin_header;
            kcp_fin_header.conv = kcp_connection->conv;
            kcp_fin_header.cmd = KCP_CMD_FIN;
            kcp_fin_header.frg = 0;
            kcp_fin_header.wnd = 0;
            kcp_fin_header.packet_data.ts = time(NULL);
            kcp_fin_header.packet_data.sn = kcp_fin_header.packet_data.ts;
            kcp_connection->syn_fin_sn = kcp_fin_header.packet_data.sn;
            kcp_fin_header.packet_data.una = 0;
            kcp_fin_header.packet_data.len = 0;
            kcp_fin_header.packet_data.data = NULL;

            char buffer[KCP_HEADER_SIZE] = { 0 };
            kcp_proto_header_encode(&kcp_fin_header, buffer, KCP_HEADER_SIZE);
            struct iovec data[1];
            data[0].iov_base = buffer;
            data[0].iov_len = KCP_HEADER_SIZE;

            kcp_send_packet(kcp_connection, &data, sizeof(data));
        } else if (kcp_header->cmd == KCP_CMD_ACK) {
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_closed) {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
            }
        } else if (kcp_header->cmd == KCP_CMD_RST) {
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_closed) {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, CONNECTION_RESET);
            }
        }
        break;
    }
    default:
        break;
    }

    if (send_rst) {
        kcp_proto_header_t kcp_rst_header;
        kcp_rst_header.conv = kcp_connection->conv;
        kcp_rst_header.cmd = KCP_CMD_RST;
        kcp_rst_header.frg = 0;
        kcp_rst_header.wnd = 0;
        kcp_rst_header.packet_data.ts = time(NULL);
        kcp_rst_header.packet_data.sn = 0;
        kcp_rst_header.packet_data.una = 0;
        kcp_rst_header.packet_data.len = 0;
        kcp_rst_header.packet_data.data = NULL;

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_rst_header, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        kcp_send_packet(kcp_connection, &data, sizeof(data));

        kcp_connection->state = KCP_STATE_DISCONNECTED;

        // 回调用户
        switch (kcp_state) {
        case KCP_STATE_SYN_SENT: // client
            if (kcp_connection->kcp_ctx->callback.on_connected) {
                kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, CONNECTION_REFUSED);
            }
            break;
        case KCP_STATE_SYN_RECEIVED: // server
            if (kcp_connection->kcp_ctx->callback.on_accepted) {
                kcp_connection->kcp_ctx->callback.on_accepted(kcp_connection->kcp_ctx, kcp_connection, CONNECTION_RESET);
            }
            break;
        default:
            break;
        }
        kcp_connection_destroy(kcp_connection);
    }
}

// KCP 写超时回调
static int32_t on_kcp_connection_timeout(struct KcpConnection *kcp_connection, uint64_t timestamp)
{

    return NO_ERROR;
}

// kcp写事件回调, 主要用于EAGAIN错误
static int32_t on_kcp_write_event(struct KcpConnection *kcp_connection, uint64_t timestamp)
{
    assert(kcp_connection != NULL);
    if (kcp_connection == NULL) {
        return NO_ERROR;
    }

    if (timestamp > 0) {
        return on_kcp_connection_timeout(kcp_connection, timestamp);
    }

    switch (kcp_connection->state) {
    case KCP_STATE_DISCONNECTED:
        return CONNECTION_CLOSED;
    case KCP_STATE_SYN_SENT: // client
    case KCP_STATE_SYN_RECEIVED: { // server
        kcp_proto_header_t *kcp_header_last = list_last_entry(&kcp_connection->node_list, kcp_proto_header_t, node_list);
        kcp_header_last->syn_data.syn_ts = kcp_time_monotonic_us(); // 由于EAGAIN错误不能算到rtt中, 故只更新发送时间戳

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_header_last, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, sizeof(data));
        if (status != 1) {
            int32_t code = get_last_errno();
            if (code == EAGAIN || code == EWOULDBLOCK) {
                return OP_TRY_AGAIN;
            } else {
                return WRITE_ERROR;
            }
        }

        evtimer_del(kcp_connection->syn_timer_event);
        uint32_t timeout_ms = kcp_connection->receive_timeout;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_connection->syn_timer_event, &tv);
        break;
    }
    case KCP_STATE_CONNECTED: {
        // TODO 正常发送数据
        break;
    }
    case KCP_STATE_FIN_SENT: // EAGAIN 重传
    case KCP_STATE_FIN_RECEIVED: {
        kcp_proto_header_t kcp_header;
        kcp_header.conv = kcp_connection->conv;
        kcp_header.cmd = KCP_CMD_FIN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.packet_data.ts = time(NULL);
        kcp_header.packet_data.sn = kcp_header.packet_data.ts;
        kcp_header.packet_data.una = 0;
        kcp_header.packet_data.len = 0;
        kcp_header.packet_data.data = NULL;
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

        struct iovec data[1];
        data->iov_base = buffer;
        data->iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, sizeof(data));
        if (status != 1) {
            int32_t code = get_last_errno();
            if (code == EAGAIN || code == EWOULDBLOCK) {
                return OP_TRY_AGAIN;
            } else {
                return WRITE_ERROR;
            }
        }
        break;
    }
    default:
        break;
    }
}

void kcp_connection_init(kcp_connection_t *kcp_conn, const sockaddr_t *remote_host, struct KcpContext* kcp_ctx)
{
    memset(&kcp_conn->node_rbtree, 0, sizeof(struct rb_node));
    list_init(&kcp_conn->node_list);

    kcp_conn->conv = 0;
    kcp_conn->mss_min = kcp_conn->mss = kcp_get_min_mss(remote_host->sa.sa_family == AF_INET6);
    kcp_conn->mtu = kcp_conn->mss + KCP_HEADER_SIZE;

    kcp_conn->snd_una = 0;
    kcp_conn->snd_nxt = 0;
    kcp_conn->rcv_nxt = 0;
    kcp_conn->ts_recent = 0;
    kcp_conn->ts_lastack = 0;
    kcp_conn->ssthresh = IKCP_THRESH_INIT;
    kcp_conn->rx_rttval = 0;
    kcp_conn->rx_srtt = 0;
    kcp_conn->rx_rto = IKCP_RTO_DEF;
    kcp_conn->rx_minrto = IKCP_RTO_MIN;
    kcp_conn->snd_wnd = IKCP_WND_SND;
    kcp_conn->rcv_wnd = IKCP_WND_RCV;
    kcp_conn->rmt_wnd = IKCP_WND_RCV;
    kcp_conn->cwnd = 0;
    kcp_conn->probe = 0;
    kcp_conn->current = 0;
    kcp_conn->interval = IKCP_INTERVAL;
    kcp_conn->ts_flush = IKCP_INTERVAL;
    kcp_conn->nrcv_buf = 0;
    kcp_conn->nsnd_buf = 0;
    kcp_conn->nrcv_que = 0;
    kcp_conn->nsnd_que = 0;
    kcp_conn->ts_probe = 0;
    kcp_conn->probe_wait = 0;

    kcp_conn->kcp_ctx = kcp_ctx;
    kcp_conn->syn_timer_event = NULL;
    kcp_conn->fin_timer_event = NULL;
    kcp_conn->ping_timer_event = NULL;
    kcp_conn->need_write_timer_event = false;
    kcp_conn->syn_retries = DEFAULT_SYN_FIN_RETRIES;
    kcp_conn->fin_retries = DEFAULT_SYN_FIN_RETRIES;
    kcp_conn->state = KCP_STATE_DISCONNECTED;
    kcp_conn->syn_fin_sn = 0;
    kcp_conn->receive_timeout = DEFAULT_RECEIVE_TIMEOUT;
    kcp_conn->keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT;
    memcpy(&kcp_conn->remote_host, remote_host, sizeof(sockaddr_t));

    kcp_conn->nodelay = 0;      // 关闭nodelay
    kcp_conn->interval = 30;    // 发送间隔 30ms
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

    kcp_conn->syn_node = NULL;
    list_init(&kcp_conn->kcp_proto_header_list);

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

    // 移除写事件
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

int32_t kcp_proto_parse(kcp_proto_header_t *kcp_header, const char **data, size_t data_size)
{
    const char *data_offset = *data;
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
    kcp_header->packet_data.ts = le32toh(*(uint32_t *)(data_offset)); // 时间戳
    data_offset += 4;
    kcp_header->packet_data.sn = le32toh(*(uint32_t *)(data_offset)); // 序列号
    data_offset += 4;
    kcp_header->packet_data.una = le32toh(*(uint32_t *)(data_offset)); // 未确认序列号
    data_offset += 4;
    kcp_header->packet_data.len = le32toh(*(uint32_t *)(data_offset)); // 数据长度
    data_offset += 4;
    kcp_header->packet_data.data = NULL;
    if (kcp_header->packet_data.len > 0) {
        kcp_header->packet_data.data = data_offset; // 数据
    }
    data_offset += kcp_header->packet_data.len;
    *data = data_offset;

    if (kcp_header->packet_data.len > (data_size - KCP_HEADER_SIZE)) {
        return INVALID_KCP_HEADER;
    }

    return NO_ERROR;
}

int32_t kcp_proto_header_encode(const kcp_proto_header_t *kcp_header, char *buffer, size_t buffer_size)
{
    if (buffer_size < (KCP_HEADER_SIZE + kcp_header->packet_data.len)) {
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

    uint32_t lengeth = 0;
    if (kcp_header->cmd == KCP_CMD_ACK) {
        *(uint64_t *)buffer_offset = htole64(kcp_header->ack_data.packet_ts);
        buffer_offset += sizeof(uint64_t);
        *(uint64_t *)buffer_offset = htole64(kcp_header->ack_data.ack_ts);
        buffer_offset += sizeof(uint64_t);
        *(uint32_t *)buffer_offset = htole32(kcp_header->ack_data.sn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->ack_data.una);
        buffer_offset += 4;
    } else {
        *(uint64_t *)buffer_offset = htole64(kcp_header->packet_data.ts);
        buffer_offset += 8;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.sn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.una);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.len);
        buffer_offset += 4;
        memcpy(buffer_offset, kcp_header->packet_data.data, kcp_header->packet_data.len);

        lengeth = kcp_header->packet_data.len;
    }

    return (KCP_HEADER_SIZE + lengeth);
}

int32_t kcp_input_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header)
{
    uint64_t timestamp = kcp_time_realtime_ms();
    switch (kcp_header->cmd) {
    case KCP_CMD_ACK: {
        

        break;
    }
    case KCP_CMD_PUSH:
        break;
    case KCP_CMD_WASK:
        break;
    case KCP_CMD_WINS:
        break;
    case KCP_CMD_PING:
        break;
    case KCP_CMD_PONG:
        break;
    case KCP_CMD_MTU_PROBE:
        break;
    case KCP_CMD_MTU_ACK:
        break;
    case KCP_CMD_FIN:
        break;
    case KCP_CMD_RST:
        break;
    default:
        break;
    }

    return NO_ERROR;
}

void on_kcp_syn_received(struct KcpContext *kcp_ctx, const sockaddr_t *addr)
{
    bool is_sever = kcp_ctx->callback.on_connect != NULL;
    do {
        if (is_sever) {
            if (!kcp_ctx->callback.on_connect(kcp_ctx, addr)) { // 拒绝连接
                kcp_proto_header_t kcp_rst_header;
                kcp_rst_header.conv = KCP_CONV_FLAG;
                kcp_rst_header.cmd = KCP_CMD_RST;
                kcp_rst_header.frg = 0;
                kcp_rst_header.wnd = 0;
                kcp_rst_header.packet_data.ts = kcp_time_monotonic_us();
                kcp_rst_header.packet_data.sn = 0;
                kcp_rst_header.packet_data.una = 0;
                kcp_rst_header.packet_data.len = 0;
                kcp_rst_header.packet_data.data = NULL;

                char buffer[KCP_HEADER_SIZE] = {0};
                kcp_proto_header_encode(&kcp_rst_header, buffer, KCP_HEADER_SIZE);
                struct iovec data[1];
                data[0].iov_base = buffer;
                data[0].iov_len = KCP_HEADER_SIZE;
                kcp_send_packet_raw(kcp_ctx->sock, addr, &data, sizeof(data));
            }
        } else {
            kcp_connection_t *kcp_connection = connection_first(&kcp_ctx->connection_set);
            if (kcp_connection == NULL) {
                break;
            }

            bool status = false;
            kcp_syn_node_t *syn_packet = list_first_entry(&kcp_ctx->syn_queue, kcp_syn_node_t, node);

            bool found = false;
            kcp_proto_header_t *pos = NULL;
            kcp_proto_header_t *next = NULL;
            list_for_each_entry_safe(pos, next, &kcp_connection->node_list, node_list) {
                if (pos->syn_data.rand_sn == syn_packet->packet_sn) { // 检验发送的sn与server响应的sn是否一致
                    kcp_connection->conv = syn_packet->conv;
                    memcpy(&kcp_connection->remote_host, addr, sizeof(sockaddr_t));

                    uint64_t current_ts = kcp_time_monotonic_us();
                    kcp_connection->rx_srtt = (current_ts - pos->syn_data.syn_ts) - (syn_packet->syn_ts - syn_packet->packet_ts);

                    kcp_proto_header_t kcp_header;
                    kcp_header.conv = syn_packet->conv;
                    kcp_header.cmd = KCP_CMD_ACK;
                    kcp_header.frg = 0;
                    kcp_header.wnd = 0;
                    kcp_header.ack_data.packet_ts = kcp_time_monotonic_us();
                    kcp_header.ack_data.ack_ts = kcp_header.ack_data.packet_ts;
                    kcp_header.ack_data.sn = syn_packet->rand_sn;
                    kcp_header.ack_data.una = 0;

                    char buffer[KCP_HEADER_SIZE] = { 0 };
                    kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

                    struct iovec data[1];
                    data[0].iov_base = buffer;
                    data[0].iov_len = KCP_HEADER_SIZE;

                    if (kcp_send_packet(kcp_connection, &data, sizeof(data)) < 0) {
                        // NOTE 此处不会触发 EAGAIN
                        int32_t code = get_last_errno();
                        KCP_LOGE("kcp send packet error. [%d, %s]", code, errno_string(code));
                        kcp_connection->state = KCP_STATE_DISCONNECTED;
                        kcp_ctx->callback.on_connected(kcp_connection, WRITE_ERROR);
                        kcp_connection_destroy(kcp_connection);
                    } else {
                        // NOTE 对端未收到ACK, 会重发SYN
                        if (kcp_connection->syn_timer_event) {
                            evtimer_del(kcp_connection->syn_timer_event);
                            evtimer_free(kcp_connection->syn_timer_event);
                            kcp_connection->syn_timer_event = NULL;
                        }

                        if (kcp_connection->state == KCP_STATE_SYN_SENT) {
                            kcp_connection->state = KCP_STATE_CONNECTED;
                            status = true;
                            kcp_ctx->callback.on_connected(kcp_connection, NO_ERROR);
                            kcp_connection->need_write_timer_event = true;
                        }
                    }

                    found = true;
                    break;
                }
            }

            if (found) {
                list_for_each_entry_safe(pos, next, &kcp_connection->node_list, node_list) {
                    list_del_init(&pos->node_list);
                    free(pos);
                }
            }
        }
    } while (false);

    // 清理已处理的syn节点
    kcp_syn_node_t *syn_node = list_first_entry(&kcp_ctx->syn_queue, kcp_syn_node_t, node);
    if (syn_node) {
        list_del_init(&syn_node->node);
        free(syn_node);
    }
}
