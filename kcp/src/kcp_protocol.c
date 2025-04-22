/*************************************************************************
    > File Name: kcp_protocol.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年02月20日 星期四 10时34分18秒
 ************************************************************************/

#include "kcp_protocol.h"

#include <string.h>
#include <assert.h>
#include <event2/event.h>

#include "kcp_config.h"
#include "kcp_endian.h"
#include "kcp_error.h"
#include "kcp_time.h"
#include "kcp_mtu.h"
#include "kcp_net_utils.h"
#include "kcp_log.h"

static int32_t on_kcp_ack_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp);
static int32_t on_kcp_push_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp);
static int32_t on_kcp_fin_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp);

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
        case KCP_STATE_DISCONNECTED:
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
    case KCP_STATE_SYN_RECEIVED: { // server
        if (kcp_header->cmd == KCP_CMD_ACK || kcp_header->cmd == KCP_CMD_PUSH) {
            kcp_connection->state = KCP_STATE_CONNECTED;
            // 通知连接建立
            if (kcp_connection->kcp_ctx->callback.on_accepted) {
                kcp_connection->kcp_ctx->callback.on_accepted(kcp_connection->kcp_ctx, kcp_connection, NO_ERROR);
            } else if (kcp_connection->kcp_ctx->callback.on_connected) {
                kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, NO_ERROR);
            }

            kcp_connection->need_write_timer_event = true;
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

            kcp_send_packet(kcp_connection, &data, 1);
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_closed) {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
            }
        }
        break;
    }
    case KCP_STATE_FIN_RECEIVED: {
        if (kcp_header->cmd == KCP_CMD_FIN) { // 对端未收到FIN包
            kcp_proto_header_t kcp_fin_header;
            kcp_fin_header.conv = kcp_connection->conv;
            kcp_fin_header.cmd = KCP_CMD_FIN;
            kcp_fin_header.frg = 0;
            kcp_fin_header.wnd = 0;
            kcp_fin_header.packet_data.ts = kcp_time_monotonic_us();
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

            kcp_send_packet(kcp_connection, &data, 1);
        } else if (kcp_header->cmd == KCP_CMD_ACK) {
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            if (kcp_connection->kcp_ctx->callback.on_closed) {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
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
        kcp_send_packet(kcp_connection, &data, 1);

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
    if (kcp_connection->state != KCP_STATE_CONNECTED) {
        return NO_ERROR;
    }

    // TODO 完善写超时回调
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
        kcp_header_last->syn_fin_data.ts = kcp_time_monotonic_us(); // 由于EAGAIN错误不能算到rtt中, 故只更新发送时间戳

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_header_last, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, 1);
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
        timestamp = kcp_time_monotonic_us();
        return on_kcp_connection_timeout(kcp_connection, timestamp);
    }
    case KCP_STATE_FIN_SENT: // EAGAIN 重传
    case KCP_STATE_FIN_RECEIVED: {
        kcp_proto_header_t *kcp_fin_header = list_last_entry(&kcp_connection->node_list, kcp_proto_header_t, node_list);
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);

        struct iovec data[1];
        data->iov_base = buffer;
        data->iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, 1);
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
    kcp_conn->need_write_timer_event = false;
    kcp_conn->syn_retries = DEFAULT_SYN_FIN_RETRIES;
    kcp_conn->fin_retries = DEFAULT_SYN_FIN_RETRIES;
    kcp_conn->state = KCP_STATE_DISCONNECTED;
    kcp_conn->syn_fin_sn = 0;
    kcp_conn->receive_timeout = DEFAULT_RECEIVE_TIMEOUT;
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
    kcp_conn->mtu_probe_ctx->mtu_current = ETHERNET_MTU_V4_MIN;
    kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
    kcp_conn->mtu_probe_ctx->on_probe_completed = on_mtu_probe_completed;

    kcp_conn->ping_ctx = (ping_ctx_t *)malloc(sizeof(ping_ctx_t));
    memset(kcp_conn->ping_ctx, 0, sizeof(ping_ctx_t));
    kcp_conn->ping_ctx->keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT;
    kcp_conn->ping_ctx->keepalive_interval = DEFAULT_KEEPALIVE_INTERVAL;
    kcp_conn->ping_ctx->keepalive_retries = DEFAULT_KEEPALIVE_RETRIES;

    kcp_conn->read_cb = on_kcp_read_event;
    kcp_conn->write_cb = on_kcp_write_event;
    kcp_conn->read_event_cb = NULL;
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

    // 清理SYN包
    if (!list_empty(&kcp_conn->kcp_proto_header_list)) {
        kcp_proto_header_t *pos = NULL;
        kcp_proto_header_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->kcp_proto_header_list, node_list) {
            list_del_init(&pos->node_list);
            free(pos);
        }
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

/**
 * @brief 连接状态下收包回调
 * 
 * @param kcp_conn 
 * @param kcp_header 
 * @return int32_t 
 */
int32_t kcp_input_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header)
{
    uint64_t timestamp = kcp_time_monotonic_us();
    switch (kcp_header->cmd) {
    case KCP_CMD_ACK:
        return on_kcp_ack_pcaket(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_PUSH:
        return on_kcp_push_pcaket(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_WASK:
        kcp_conn->probe |= KCP_ASK_TELL;
        break;
    case KCP_CMD_WINS:
        kcp_conn->rmt_wnd = kcp_header->wnd;
        break;
    case KCP_CMD_PING:
        /*
         * @note ping包由server发送, client接收
         * 故, keepalive_sn在serve侧是记录的发送的随机序列, 在client侧记录是server发送的ping包的sn
         * 用以响应ping包
         */
        kcp_conn->ping_ctx->keepalive_packet_ts = timestamp;
        kcp_conn->ping_ctx->keepalive_sn = kcp_header->ping_data.sn;
        kcp_conn->probe |= KCP_PING_RECV;
        break;
    case KCP_CMD_PONG: // 计算RTT
        if (kcp_conn->ping_ctx->keepalive_sn == kcp_header->ping_data.sn) {
            uint64_t rtt = (timestamp - kcp_conn->ping_ctx->keepalive_packet_ts) -
                           (kcp_header->ping_data.ping_ts - kcp_header->ping_data.packet_ts);
            kcp_conn->ping_ctx->keepalive_rtt = rtt;
            kcp_conn->ping_ctx->keepalive_sn = 0;
            kcp_conn->ping_ctx->keepalive_packet_ts = 0;
            kcp_conn->ping_ctx->keepalive_next_ts = timestamp + kcp_conn->ping_ctx->keepalive_interval;
            kcp_conn->ping_ctx->keepalive_xretries = 0;
        }
        break;
    case KCP_CMD_MTU_PROBE: // NOTE client发送的MTU探测包
        return kcp_mtu_probe_received(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_MTU_ACK: // NOTE server响应的MTU探测包
        return kcp_mtu_ack_received(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_FIN:
        return on_kcp_fin_pcaket(kcp_conn, kcp_header, timestamp);
    // case KCP_CMD_RST:
    //     break;
    default:
        break;
    }

    return NO_ERROR;
}

int32_t kcp_flush(kcp_connection_t *kcp_conn)
{

    return 0;
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
            list_for_each_entry_safe(pos, next, &kcp_connection->kcp_proto_header_list, node_list) {
                if (pos->cmd == KCP_CMD_SYN && pos->syn_fin_data.rand_sn == syn_packet->packet_sn) {
                    // 检验发送的sn与server响应的sn是否一致
                    kcp_connection->conv = syn_packet->conv;
                    memcpy(&kcp_connection->remote_host, addr, sizeof(sockaddr_t));

                    uint64_t current_ts = kcp_time_monotonic_us();
                    kcp_connection->rx_srtt = (current_ts - pos->syn_fin_data.ts) - (syn_packet->ts - syn_packet->packet_ts);
                    kcp_connection->rx_rttval = kcp_connection->rx_srtt / 2;

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

                    if (kcp_send_packet(kcp_connection, &data, 1) < 0) {
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
                            list_del_init(&pos->node_list); // 其余的由server发送SYN包来删除或者destroy的时候删除
                            free(pos);

                            kcp_connection->state = KCP_STATE_CONNECTED;
                            status = true;
                            kcp_ctx->callback.on_connected(kcp_connection, NO_ERROR);
                            kcp_connection->need_write_timer_event = true;
                            kcp_mtu_probe(kcp_connection, DEFAULT_MTU_PROBE_TIMEOUT, 2);
                        }
                    }

                    break;
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

#define HANDLE_SND_BUF(kcp_conn) \
    do { \
        list_add_tail(&pos->node, &kcp_conn->snd_buf_unused); \
        ++kcp_conn->nsnd_buf_unused; \
        --kcp_conn->nsnd_buf; \
    } while (false)

static int32_t on_kcp_ack_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    if (kcp_conn->state == KCP_STATE_DISCONNECTED) {
        return NO_ERROR;
    }

    kcp_segment_t *pos = NULL;
    kcp_segment_t *next = NULL;
    list_for_each_entry_safe(pos, next, &kcp_conn->snd_buf, node) {
        if (pos->sn == kcp_header->ack_data.sn) {
            int32_t timestamp_tmp = timestamp;
            int32_t ts_tmp = pos->ts;
            int32_t ack_ts_tmp = kcp_header->ack_data.ack_ts;
            int32_t packet_ts_tmp = kcp_header->ack_data.packet_ts;

            // 计算RTT
            if (kcp_conn->rx_srtt == 0) {
                kcp_conn->rx_srtt = (timestamp_tmp - ts_tmp) - (ack_ts_tmp - packet_ts_tmp);
                kcp_conn->rx_rttval = kcp_conn->rx_srtt / 2;
            } else {
                int32_t rtt = (timestamp_tmp - ts_tmp) - (ack_ts_tmp - packet_ts_tmp);
                int64_t delta = ABS(rtt - kcp_conn->rx_srtt);
                kcp_conn->rx_rttval = (3 * kcp_conn->rx_rttval + delta) / 4;
                kcp_conn->rx_srtt = (kcp_conn->rx_srtt * 7 + rtt) / 8;
            }

            // 计算RTO
            int32_t rto = kcp_conn->rx_srtt + MAX(kcp_conn->interval, 4 * kcp_conn->rx_rttval);
            kcp_conn->rx_rto = CLAMP(rto, kcp_conn->rx_minrto * 1000, KCP_RTO_MAX * 1000);

            HANDLE_SND_BUF(kcp_conn);
            break;
        }
    }

    // 解析una
    list_for_each_entry_safe(pos, next, &kcp_conn->snd_buf, node) {
        if (pos->sn < kcp_header->ack_data.una) {
            HANDLE_SND_BUF(kcp_conn);
        } else {
            break;
        }
    }

    // 收缩snd_buf_unused空间
    int32_t nsnd_buf_diff = (int32_t)kcp_conn->nsnd_buf_unused - (int32_t)kcp_conn->snd_wnd;
    for (int32_t i = 0; i < nsnd_buf_diff; ++i) {
        if (list_empty(&kcp_conn->snd_buf_unused)) {
            break;
        }

        pos = list_first_entry(&kcp_conn->snd_buf_unused, kcp_segment_t, node);
        list_del_init(&pos->node);
        free(pos);
    }
    return NO_ERROR;
}

int32_t on_kcp_push_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    if (kcp_header->packet_data.sn >= (kcp_conn->rcv_nxt + kcp_conn->rcv_wnd)) {
        // NOTE 如果收到PUSH包的序号超过接收窗口大小时, 发送可接收包大小, 并丢弃此包
        kcp_conn->probe |= KCP_ASK_TELL;
        return NO_ERROR;
    }

    if (kcp_header->packet_data.sn < kcp_conn->rcv_nxt) { // 此包已被接收过
        return NO_ERROR;
    }

    // push ack
    kcp_ack_t *ack_item = (kcp_ack_t *)malloc(sizeof(kcp_ack_t));
    if (ack_item == NULL) {
        return NO_MEMORY;
    }
    list_init(&ack_item->node);
    kcp_segment_t *kcp_segment = (kcp_segment_t *)malloc(sizeof(kcp_segment_t) + ETHERNET_MTU);
    if (kcp_segment == NULL) {
        free(ack_item);
        return NO_MEMORY;
    }
    list_init(&kcp_segment->node);

    ack_item->sn = kcp_header->packet_data.sn;
    ack_item->psn = kcp_header->packet_data.psn;
    ack_item->ts = timestamp;
    list_add_tail(&ack_item->node, &kcp_conn->ack_item);

    // push data
    kcp_segment->conv = kcp_header->conv;
    kcp_segment->cmd = kcp_header->cmd;
    kcp_segment->frg = kcp_header->frg;
    kcp_segment->wnd = kcp_header->wnd;
    kcp_segment->ts = kcp_header->packet_data.ts;
    kcp_segment->sn = kcp_header->packet_data.sn;
    kcp_segment->psn = kcp_header->packet_data.psn;
    kcp_segment->una = kcp_header->packet_data.una;
    kcp_segment->len = kcp_header->packet_data.len;
    if (kcp_segment->len > 0) {
        memcpy(kcp_segment->data, kcp_header->packet_data.data, kcp_segment->len);
    }

    bool repeat = false;
    kcp_segment_t *pos = NULL;
    kcp_segment_t *next = NULL;
    list_for_each_entry_safe(pos, next, &kcp_conn->rcv_buf, node) {
        if (pos->psn == kcp_segment->psn && pos->frg == kcp_segment->frg) { // 相同包
            repeat = true;
            break;
        }

        if (kcp_segment->sn > pos->sn) {
            break;
        }
    }

    if (repeat) {
        free(kcp_segment);
        list_del_init(&ack_item->node);
        free(ack_item);
        return NO_ERROR;
    }

    kcp_segment_t *last = NULL;
    list_for_each_entry_safe(pos, next, &kcp_conn->rcv_buf, node) {
        if (pos->psn == kcp_segment->psn) {
            last = pos;
            break;
        }
    }
    if (last) { // 属于同一包
        pos = last;
        list_for_each_entry_safe_from(pos, next, &kcp_conn->rcv_buf, node) {
            if (pos->psn != kcp_segment->psn) { // 同一个包的最后一个节点
                break;
            }
            if (pos->frg > kcp_segment->frg) { // 同一包的不同分片
                break;
            }

            last = pos;
        }

        list_add(&last->node, &kcp_segment->node);
    } else { // 新包
        list_add_tail(&kcp_segment->node, &kcp_conn->rcv_buf);
    }
    ++kcp_conn->nrcv_buf;

    // 解析rcv_buf, 组成完整数据包
    kcp_segment_t *first = NULL;
    uint16_t packet_count = 0;
    list_for_each_entry_safe(first, next, &kcp_conn->rcv_buf, node) {
        if (first->frg == 0) {
            packet_count = (uint16_t)first->wnd; // NOTE 起始packet, 表示packet个数
            pos = first;
            uint16_t count = 0;
            list_for_each_entry_safe_from(pos, next, &kcp_conn->rcv_buf, node) {
                if (first->psn != pos->psn) { // 下一个包
                    break;
                }
                ++count;
            }

            if (packet_count == count) { // 完整的一包
                pos = first;
                int32_t size = 0;
                list_for_each_entry_safe_from(pos, next, &kcp_conn->rcv_buf, node) {
                    if (first->psn != pos->psn) { // 下一个包
                        break;
                    }
                    size += pos->len;
                    list_add_tail(&pos->node, &kcp_conn->rcv_queue);
                    --kcp_conn->nrcv_buf;
                }

                if (kcp_conn->read_event_cb) {
                    kcp_conn->read_event_cb(kcp_conn, size);
                }
                break;
            }
        }
    }

    return NO_ERROR;
}

static void on_fin_packet_timeout_cb(int fd, short event, void *arg)
{
    kcp_connection_t *kcp_conn = (kcp_connection_t *)arg;
    assert(kcp_conn->state == KCP_STATE_FIN_RECEIVED);

    if (kcp_conn->fin_retries--) {
        kcp_proto_header_t *pos = NULL;
        kcp_proto_header_t *next = NULL;
        uint32_t packet_sn = 0;
        list_for_each_entry_safe(pos, next, &kcp_conn->kcp_proto_header_list, node_list) {
            if (pos->cmd == KCP_CMD_FIN) {
                packet_sn = pos->syn_fin_data.packet_sn;
                break;
            }
        }

        kcp_proto_header_t *kcp_fin_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
        kcp_fin_header->conv = kcp_conn->conv;
        kcp_fin_header->cmd = KCP_CMD_FIN;
        kcp_fin_header->frg = 0;
        kcp_fin_header->wnd = 0;
        kcp_fin_header->syn_fin_data.packet_ts = kcp_time_monotonic_us();
        kcp_fin_header->syn_fin_data.ts = kcp_fin_header->syn_fin_data.packet_ts;
        kcp_fin_header->syn_fin_data.packet_sn = packet_sn;
        kcp_fin_header->syn_fin_data.rand_sn = kcp_fin_header->syn_fin_data.packet_ts;

        list_add_tail(&kcp_fin_header->node_list, &kcp_conn->kcp_proto_header_list);

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        kcp_send_packet(kcp_conn, &data, 1);

        evtimer_del(kcp_conn->fin_timer_event);
        uint32_t timeout_ms = kcp_conn->receive_timeout;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_conn->fin_timer_event, &tv);
        return;
    }

    if (kcp_conn->kcp_ctx->callback.on_closed) {
        kcp_conn->kcp_ctx->callback.on_closed(kcp_conn, TIMED_OUT);
    }
}

int32_t on_kcp_fin_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    assert(kcp_conn->state == KCP_STATE_CONNECTED);

    // 1、响应FIN包, 修改状态
    kcp_proto_header_t *kcp_fin_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
    kcp_fin_header->conv = kcp_conn->conv;
    kcp_fin_header->cmd = KCP_CMD_FIN;
    kcp_fin_header->frg = 0;
    kcp_fin_header->wnd = 0;
    kcp_fin_header->syn_fin_data.packet_ts = timestamp;
    kcp_fin_header->syn_fin_data.ts = timestamp;
    kcp_fin_header->syn_fin_data.packet_sn = kcp_header->packet_data.sn;
    kcp_fin_header->syn_fin_data.rand_sn = timestamp;

    list_add_tail(&kcp_fin_header->node_list, &kcp_conn->kcp_proto_header_list);

    char buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);
    struct iovec data[1];
    data[0].iov_base = buffer;
    data[0].iov_len = KCP_HEADER_SIZE;

    // 发送失败或丢包靠超时重传
    kcp_send_packet(kcp_conn, &data, 1);
    kcp_conn->state = KCP_STATE_FIN_RECEIVED;

    if (kcp_conn->fin_timer_event == NULL) {
        kcp_conn->fin_timer_event = evtimer_new(kcp_conn->kcp_ctx->event_loop, on_fin_packet_timeout_cb, kcp_conn);
        if (kcp_conn->fin_timer_event == NULL) {
            return NO_MEMORY;
        }
    }
    uint32_t timeout_ms = kcp_conn->receive_timeout;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    evtimer_add(kcp_conn->fin_timer_event, &tv);
}