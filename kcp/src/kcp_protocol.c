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

#include <xxhash.h>

#include "kcp_config.h"
#include "kcp_endian.h"
#include "kcp_error.h"
#include "kcp_time.h"
#include "kcp_mtu.h"
#include "kcp_net_utils.h"
#include "kcp_log.h"

static int32_t  on_kcp_ack_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp);
static int32_t  on_kcp_push_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp);
static int32_t  on_kcp_fin_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp);
static int32_t  on_kcp_ping_timeout(kcp_connection_t *kcp_conn, uint64_t timestamp);

static void on_mtu_probe_completed(kcp_connection_t *kcp_conn, uint32_t mtu, int32_t code)
{
    KCP_LOGI("on_mtu_probe_completed: %u, mtu: %u, code: %d", kcp_conn->conv, mtu, code);
    if (code == NO_ERROR) {
        int32_t ip_header_size = kcp_conn->remote_host.sa.sa_family == AF_INET6 ? IPV6_HEADER_SIZE : IPV4_HEADER_SIZE;
        if (kcp_conn->mtu > mtu) {
            KCP_LOGW("MTU reduced from %d to %d", kcp_conn->mtu, mtu);
            kcp_conn->kcp_ctx->callback.on_error(kcp_conn->kcp_ctx, kcp_conn, MTU_REDUCTION);
            return;
        }

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
        KCP_LOGW("KCP_CMD_RST received, close connection");
        switch (kcp_connection->state) {
        case KCP_STATE_SYN_SENT: // client
            kcp_connection->state = KCP_STATE_DISCONNECTED;
            kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, CONNECTION_REFUSED);
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

        // NOTE 由 kcp_parse_packet 释放
        // kcp_connection_destroy(kcp_connection);
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
            if (kcp_connection->syn_timer_event) {
                event_free(kcp_connection->syn_timer_event);
                kcp_connection->syn_timer_event = NULL;
            }
            // 通知连接建立
            if (kcp_connection->kcp_ctx->callback.on_accepted) {
                uint64_t ts = kcp_time_monotonic_ms();
                kcp_connection->ts_flush = ts + kcp_connection->interval;
                kcp_connection->kcp_ctx->callback.on_accepted(kcp_connection->kcp_ctx, kcp_connection, NO_ERROR);
                kcp_connection->ping_ctx->keepalive_next_ts = ts + kcp_connection->ping_ctx->keepalive_interval;
                // kcp_mtu_probe(kcp_connection, DEFAULT_MTU_PROBE_TIMEOUT, 2);
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
            kcp_proto_header_t *pos = NULL;
            kcp_proto_header_t *next = NULL;
            list_for_each_entry_safe(pos, next, &kcp_connection->kcp_proto_header_list, node_list) {
                if (pos->cmd == KCP_CMD_FIN && pos->syn_fin_data.rand_sn == kcp_header->syn_fin_data.packet_sn) {
                    kcp_proto_header_t kcp_ack_header;
                    kcp_ack_header.conv = kcp_connection->conv;
                    kcp_ack_header.cmd = KCP_CMD_ACK;
                    kcp_ack_header.frg = 0;
                    kcp_ack_header.wnd = 0;
                    kcp_ack_header.ack_data.packet_ts = kcp_header->packet_data.ts;
                    kcp_ack_header.ack_data.ack_ts = kcp_time_monotonic_us();
                    kcp_ack_header.ack_data.sn = kcp_header->syn_fin_data.rand_sn;
                    kcp_ack_header.ack_data.una = 0;

                    char buffer[KCP_HEADER_SIZE] = {0};
                    kcp_proto_header_encode(&kcp_ack_header, buffer, KCP_HEADER_SIZE);
                    struct iovec data[1];
                    data[0].iov_base = buffer;
                    data[0].iov_len = KCP_HEADER_SIZE;

                    kcp_send_packet(kcp_connection, data, 1);
                    kcp_connection->state = KCP_STATE_DISCONNECTED;
                    if (kcp_connection->kcp_ctx->callback.on_closed) {
                        kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
                    }
                    break;
                }
            }

            // NOTE 由 kcp_parse_packet 释放
        }
        break;
    }
    case KCP_STATE_FIN_RECEIVED: {
        if (kcp_header->cmd == KCP_CMD_FIN) { // 对端未收到FIN包
            kcp_proto_header_t *kcp_fin_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
            list_init(&kcp_fin_header->node_list);
            kcp_fin_header->conv = kcp_connection->conv;
            kcp_fin_header->cmd = KCP_CMD_FIN;
            kcp_fin_header->frg = 0;
            kcp_fin_header->wnd = 0;
            kcp_fin_header->syn_fin_data.packet_ts = kcp_time_monotonic_us();
            kcp_fin_header->syn_fin_data.ts = kcp_fin_header->syn_fin_data.packet_ts;
            kcp_fin_header->syn_fin_data.packet_sn = kcp_header->syn_fin_data.rand_sn;
            kcp_fin_header->syn_fin_data.rand_sn = XXH32(&kcp_fin_header->syn_fin_data.ts, sizeof(kcp_fin_header->syn_fin_data.ts), 0);
            list_add_tail(&kcp_fin_header->node_list, &kcp_connection->kcp_proto_header_list);

            char buffer[KCP_HEADER_SIZE] = { 0 };
            kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);
            struct iovec data[1];
            data[0].iov_base = buffer;
            data[0].iov_len = KCP_HEADER_SIZE;

            kcp_send_packet(kcp_connection, data, 1);
        } else if (kcp_header->cmd == KCP_CMD_ACK) {
            kcp_proto_header_t *pos = NULL;
            kcp_proto_header_t *next = NULL;
            list_for_each_entry_safe(pos, next, &kcp_connection->kcp_proto_header_list, node_list) {
                if (pos->cmd == KCP_CMD_FIN && pos->syn_fin_data.rand_sn == kcp_header->ack_data.sn) {
                    kcp_connection->state = KCP_STATE_DISCONNECTED;
                    if (kcp_connection->kcp_ctx->callback.on_closed) {
                        kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
                    }
                    break;
                }
            }
            // NOTE 由 kcp_parse_packet 释放
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
        kcp_send_packet(kcp_connection, data, 1);

        kcp_connection->state = KCP_STATE_DISCONNECTED;

        // 回调用户
        switch (kcp_state) {
        case KCP_STATE_SYN_SENT: // client
            kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, CONNECTION_REFUSED);
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

static int kcp_wnd_unused(struct KcpConnection *kcp_connection)
{
    if (kcp_connection->nrcv_buf < kcp_connection->rcv_wnd) {
        return kcp_connection->rcv_wnd - kcp_connection->nrcv_buf;
    }

    return 0;
}

/**
 * @brief kcp 写超时回调
 * 
 * @param kcp_connection kcp connection 
 * @param timestamp 微秒时间戳 (timestamp > 0)
 * @return int32_t 成功返回NO_ERROR, 失败返回错误码
 */
static int32_t on_kcp_write_timeout(struct KcpConnection *kcp_connection, uint64_t timestamp)
{
    if (kcp_connection->state != KCP_STATE_CONNECTED) {
        return NO_ERROR;
    }

    if (kcp_connection->ts_flush > (timestamp / 1000)) {
        // 如果当前时间戳小于下次超时时间戳, 则不处理
        return NO_ERROR;
    }

    char *ptr = kcp_connection->buffer;
    char kcp_header_buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_t kcp_ack_header;
    kcp_ack_header.conv = kcp_connection->conv;
    kcp_ack_header.cmd = KCP_CMD_ACK;
    kcp_ack_header.frg = 0;
    kcp_ack_header.wnd = kcp_wnd_unused(kcp_connection);
    kcp_ack_header.ack_data.una = kcp_connection->rcv_nxt;
    {
        // 回复ACK
        kcp_ack_t *pos = NULL;
        kcp_ack_t *next = NULL;
        while (!list_empty(&kcp_connection->ack_item)) {
            pos = list_first_entry(&kcp_connection->ack_item, kcp_ack_t, node);
            kcp_ack_header.ack_data.packet_ts = pos->ts;
            kcp_ack_header.ack_data.ack_ts = timestamp;
            kcp_ack_header.ack_data.sn = pos->sn;

            kcp_proto_header_encode(&kcp_ack_header, kcp_header_buffer, KCP_HEADER_SIZE);
            memcpy(ptr, kcp_header_buffer, KCP_HEADER_SIZE);
            ptr += KCP_HEADER_SIZE;
            list_del_init(&pos->node);
            free(pos);

            if ((ptr - kcp_connection->buffer + KCP_HEADER_SIZE) > kcp_connection->mtu) {
                // 如果当前缓冲区大小超过了MTU, 则发送数据包
                struct iovec data[1];
                data[0].iov_base = kcp_connection->buffer;
                data[0].iov_len = ptr - kcp_connection->buffer;

                kcp_send_packet(kcp_connection, data, 1);
                ptr = kcp_connection->buffer;
            }
        }
    }

    if (kcp_connection->rmt_wnd == 0) {
        if (kcp_connection->probe_wait == 0) {
            kcp_connection->probe_wait = 1; // 开始探测
            kcp_connection->win_ts_probe = timestamp + KCP_PROBE_INIT;
        } else {
            if (timestamp >= kcp_connection->win_ts_probe) {
                if (kcp_connection->probe_wait < KCP_PROBE_INIT) {
                    kcp_connection->win_ts_probe = timestamp + KCP_PROBE_INIT;
                }
                kcp_connection->probe_wait += kcp_connection->probe_wait / 2;
                if (kcp_connection->probe_wait > KCP_PROBE_LIMIT) {
                    kcp_connection->probe_wait = KCP_PROBE_LIMIT; // 最大探测时间
                }
                kcp_connection->win_ts_probe = timestamp + kcp_connection->probe_wait;
                kcp_connection->probe |= KCP_ASK_SEND; // 设置探测标志
            }
        }
    } else {
        kcp_connection->win_ts_probe = 0;
        kcp_connection->probe_wait = 0;
    }

    kcp_proto_header_t kcp_window_header;
    memset(&kcp_window_header, 0, sizeof(kcp_proto_header_t));
    kcp_window_header.conv = kcp_connection->conv;
    kcp_window_header.packet_data.ts = timestamp;
    // window size ask
    if (kcp_connection->probe & KCP_ASK_SEND) {
        kcp_window_header.cmd = KCP_CMD_WASK;
        if ((ptr - kcp_connection->buffer + KCP_HEADER_SIZE) > kcp_connection->mtu) {
            struct iovec data[1];
            data[0].iov_base = kcp_connection->buffer;
            data[0].iov_len = ptr - kcp_connection->buffer;

            kcp_send_packet(kcp_connection, data, 1);
            ptr = kcp_connection->buffer;
        }
        kcp_proto_header_encode(&kcp_window_header, kcp_header_buffer, KCP_HEADER_SIZE);
        memcpy(ptr, kcp_header_buffer, KCP_HEADER_SIZE);
        ptr += KCP_HEADER_SIZE;
    }

    // windows size tell
    if (kcp_connection->probe & KCP_ASK_TELL) {
        kcp_window_header.cmd = KCP_CMD_WINS;
        kcp_window_header.wnd = kcp_wnd_unused(kcp_connection);
        if ((ptr - kcp_connection->buffer + KCP_HEADER_SIZE) > kcp_connection->mtu) {
            struct iovec data[1];
            data[0].iov_base = kcp_connection->buffer;
            data[0].iov_len = ptr - kcp_connection->buffer;

            kcp_send_packet(kcp_connection, data, 1);
            ptr = kcp_connection->buffer;
        }
        kcp_proto_header_encode(&kcp_window_header, kcp_header_buffer, KCP_HEADER_SIZE);
        memcpy(ptr, kcp_header_buffer, KCP_HEADER_SIZE);
        ptr += KCP_HEADER_SIZE;
    }

    // NOTE 此函数必须在 ping request 之前调用
    if (on_kcp_ping_timeout(kcp_connection, timestamp)) { // 资源已被释放
        return NO_ERROR;
    }
    // ping request
    if (kcp_connection->ping_ctx->keepalive_next_ts < timestamp) {
        KCP_LOGD("send ping, next ts: %llu", kcp_connection->ping_ctx->keepalive_next_ts, timestamp);
        kcp_proto_header_t ping_header;
        memset(&ping_header, 0, sizeof(kcp_proto_header_t));
        ping_header.conv = kcp_connection->conv;
        ping_header.cmd = KCP_CMD_PING;
        ping_header.ping_data.packet_ts = 0;
        ping_header.ping_data.ts = timestamp;
        ping_header.ping_data.sn = XXH64(&timestamp, sizeof(timestamp), 0);

        ping_session_t *ping_session = (ping_session_t *)malloc(sizeof(ping_session_t));
        list_init(&ping_session->node);
        ping_session->packet_ts = timestamp;
        ping_session->packet_sn = ping_header.ping_data.sn;
        list_add_tail(&ping_session->node, &kcp_connection->ping_ctx->ping_request_queue);

        if ((ptr - kcp_connection->buffer + KCP_HEADER_SIZE) > kcp_connection->mtu) {
            struct iovec data[1];
            data[0].iov_base = kcp_connection->buffer;
            data[0].iov_len = ptr - kcp_connection->buffer;

            kcp_send_packet(kcp_connection, data, 1);
            ptr = kcp_connection->buffer;
        }

        kcp_proto_header_encode(&ping_header, kcp_header_buffer, KCP_HEADER_SIZE);
        memcpy(ptr, kcp_header_buffer, KCP_HEADER_SIZE);
        ptr += KCP_HEADER_SIZE;

        kcp_connection->ping_ctx->keepalive_next_ts = timestamp + kcp_connection->ping_ctx->keepalive_interval;
    }

    // pong response
    if (kcp_connection->probe & KCP_PING_RECV) {
        kcp_proto_header_t pong_header;
        memset(&pong_header, 0, sizeof(kcp_proto_header_t));
        pong_header.conv = kcp_connection->conv;
        pong_header.cmd = KCP_CMD_PONG;
        pong_header.ping_data.packet_ts = kcp_connection->ping_ctx->keepalive_packet_ts;
        pong_header.ping_data.ts = timestamp;
        pong_header.ping_data.sn = kcp_connection->ping_ctx->keepalive_sn;

        if ((ptr - kcp_connection->buffer + KCP_HEADER_SIZE) > kcp_connection->mtu) {
            struct iovec data[1];
            data[0].iov_base = kcp_connection->buffer;
            data[0].iov_len = ptr - kcp_connection->buffer;

            kcp_send_packet(kcp_connection, data, 1);
            ptr = kcp_connection->buffer;
        }

        kcp_proto_header_encode(&pong_header, kcp_header_buffer, KCP_HEADER_SIZE);
        memcpy(ptr, kcp_header_buffer, KCP_HEADER_SIZE);
        ptr += KCP_HEADER_SIZE;
    }
    kcp_connection->probe = 0; // 清除探测标志

    // 发送数据包
    if (ptr > kcp_connection->buffer) {
        struct iovec data[1];
        data[0].iov_base = kcp_connection->buffer;
        data[0].iov_len = ptr - kcp_connection->buffer;

        kcp_send_packet(kcp_connection, data, 1);
        ptr = kcp_connection->buffer;
    }

    int32_t cwnd = MIN(kcp_connection->snd_wnd, kcp_connection->rmt_wnd);
    if (kcp_connection->nocwnd == 0) {
        cwnd = MIN(cwnd, kcp_connection->cwnd);
    }

    // 将 snd_queue 数据移动到 snd_buf
    while (((int64_t)kcp_connection->snd_nxt - ((int64_t)kcp_connection->snd_una + cwnd)) < 0) {
        if (list_empty(&kcp_connection->snd_queue)) {
            break; // 如果发送队列为空, 则退出
        }

        kcp_segment_t *segment = list_first_entry(&kcp_connection->snd_queue, kcp_segment_t, node_list);
        list_del_init(&segment->node_list);
        segment->conv = kcp_connection->conv;
        segment->cmd = KCP_CMD_PUSH;
        if (segment->frg > 0) {
            // NOTE frg == 0 时wnd表示分片个数
            segment->wnd = kcp_wnd_unused(kcp_connection);
        }
        segment->ts = timestamp;
        segment->sn = kcp_connection->snd_nxt++;
        segment->una = kcp_connection->rcv_nxt;
        segment->rto = kcp_connection->rx_rto;
        segment->resendts = timestamp;
        segment->fastack = 0;
        segment->xmit = 0;
        list_del_init(&segment->node_list);
        list_add_tail(&segment->node_list, &kcp_connection->snd_buf);
        --kcp_connection->nsnd_que;
        ++kcp_connection->nsnd_buf;
    }

    bool packet_lost = false;
    bool change_ssthresh = false;
    uint32_t resent = kcp_connection->fastresend > 0 ? kcp_connection->fastresend : UINT32_MAX;
    uint32_t rtomin = (kcp_connection->nodelay == 0) ? (kcp_connection->rx_rto >> 3) : 0;
    {
        bool need_flush = false;
        int32_t buffer_index = 0;
        char packet_cache[KCP_PACKET_COUNT + 1][ETHERNET_MTU];
        size_t packet_cache_size[KCP_PACKET_COUNT + 1] = {0};
        char *buffer_offset = packet_cache[buffer_index];
        kcp_segment_t *pos = NULL;
        if (!list_empty(&kcp_connection->snd_buf)) {
            list_for_each_entry(pos, &kcp_connection->snd_buf, node_list) {
                bool need_send = false;
                if (pos->xmit == 0) {
                    need_send = true;
                    pos->xmit++;
                    pos->rto = kcp_connection->rx_rto;
                    pos->resendts = timestamp + pos->rto + rtomin;
                } else if (timestamp >= pos->resendts) {
                    need_send = true; // 超时重传
                    pos->xmit++;
                    if (kcp_connection->nodelay == 0) {
                        pos->rto = MAX(pos->rto , kcp_connection->rx_rto);
                    } else {
                        int32_t step = (kcp_connection->nodelay < 2) ? pos->rto : kcp_connection->rx_rto;
                        pos->rto += step / 2;
                    }
                    pos->resendts = timestamp + pos->rto;
                    packet_lost = true;
                } else if (pos->fastack >= resent) {
                    if (pos->xmit <= kcp_connection->fastlimit || kcp_connection->fastlimit <= 0) {
                        need_send = true; // 快速重传
                        pos->xmit++;
                        pos->resendts = timestamp + pos->rto;
                        pos->fastack = 0;
                        change_ssthresh = true;
                    }
                }

                if (need_send) {
                    need_flush = true;
                    int32_t segment_size = 0;
                label_send:
                    if (buffer_index >= KCP_PACKET_COUNT) {
                        // 如果缓存区已满, 则发送数据包
                        struct iovec data[KCP_PACKET_COUNT];
                        for (int i = 0; i < KCP_PACKET_COUNT; i++) {
                            data[i].iov_base = packet_cache[i];
                            data[i].iov_len = packet_cache_size[i];
                        }

                        int32_t status = kcp_send_packet(kcp_connection, data, KCP_PACKET_COUNT);
                        if (status < 0) {
                            int32_t code = get_last_errno();
                            if (code == EAGAIN || code == EWOULDBLOCK) {
                                return OP_TRY_AGAIN;
                            } else {
                                return WRITE_ERROR;
                            }
                        }

                        buffer_index = 0;
                        buffer_offset = packet_cache[buffer_index];
                    }
                    segment_size = kcp_segment_encode(pos, buffer_offset, ETHERNET_MTU - packet_cache_size[buffer_index]);
                    if (segment_size == BUFFER_TOO_SMALL) {
                        ++buffer_index;
                        buffer_offset = packet_cache[buffer_index];
                        goto label_send; // 缓冲区不足, 切换到下一个缓存区
                    } else {
                        packet_cache_size[buffer_index] += segment_size;
                        buffer_offset += segment_size;
                    }
                }
            }
        }

        // 发送剩余数据包
        if (need_flush) {
            struct iovec data[KCP_PACKET_COUNT];
            for (int i = 0; i < (buffer_index + 1); i++) {
                data[i].iov_base = packet_cache[i];
                data[i].iov_len = packet_cache_size[i];
            }

            int32_t status = kcp_send_packet(kcp_connection, data, buffer_index + 1);
            if (status < 0) {
                int32_t code = get_last_errno();
                if (code == EAGAIN || code == EWOULDBLOCK) {
                    return OP_TRY_AGAIN;
                } else {
                    return WRITE_ERROR;
                }
            }
        }
    }

    if (change_ssthresh) {
        uint32_t inflight = kcp_connection->snd_nxt - kcp_connection->snd_una;
        kcp_connection->ssthresh = inflight / 2;
        if (kcp_connection->ssthresh < KCP_THRESH_MIN) {
            kcp_connection->ssthresh = KCP_THRESH_MIN;
        }
        kcp_connection->cwnd = kcp_connection->ssthresh + resent;
        kcp_connection->incr = kcp_connection->cwnd * kcp_connection->mss;
    }

    if (packet_lost) {
        kcp_connection->ssthresh = cwnd / 2;
        if (kcp_connection->ssthresh < KCP_THRESH_MIN) {
            kcp_connection->ssthresh = KCP_THRESH_MIN;
        }
        kcp_connection->cwnd = 1;
        kcp_connection->incr = kcp_connection->mss;
    }

    if (kcp_connection->cwnd < 1) {
        kcp_connection->cwnd = 1;
        kcp_connection->incr = kcp_connection->mss;
    }

    kcp_connection->ts_flush = timestamp / 1000 + kcp_connection->interval;
    return NO_ERROR;
}

/**
 * @brief kcp写事件回调
 * 
 * @param kcp_connection kcp connection 
 * @param timestamp 微秒时间戳, > 0 表示超时, == 0 表示正常写事件
 * @return int32_t 成功返回NO_ERROR, 失败返回错误码
 */
static int32_t on_kcp_write_event(struct KcpConnection *kcp_connection, uint64_t timestamp)
{
    assert(kcp_connection != NULL);
    if (kcp_connection == NULL) {
        return NO_ERROR;
    }

    if (timestamp > 0) {
        return on_kcp_write_timeout(kcp_connection, timestamp);
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
        int32_t status = kcp_send_packet(kcp_connection, data, 1);
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
        return on_kcp_write_timeout(kcp_connection, timestamp);
    }
    case KCP_STATE_FIN_SENT: // EAGAIN 重传
    case KCP_STATE_FIN_RECEIVED: {
        kcp_proto_header_t *kcp_fin_header = list_last_entry(&kcp_connection->node_list, kcp_proto_header_t, node_list);
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);

        struct iovec data[1];
        data->iov_base = buffer;
        data->iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, data, 1);
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
    kcp_conn->mtu = kcp_ctx->nic_mtu;
    kcp_conn->mss = kcp_conn->mtu - KCP_HEADER_SIZE;
    kcp_conn->mss_min = kcp_get_min_mtu(remote_host->sa.sa_family == AF_INET6) - KCP_HEADER_SIZE;

    kcp_conn->snd_una = 0;
    kcp_conn->snd_nxt = 0;
    kcp_conn->rcv_nxt = 0;
    kcp_conn->psn_nxt = 0;
    kcp_conn->ts_recent = 0;
    kcp_conn->ts_lastack = 0;
    kcp_conn->ssthresh = KCP_THRESH_INIT;
    kcp_conn->rx_rttval = 0;
    kcp_conn->rx_srtt = 0;
    kcp_conn->rx_rto = KCP_RTO_DEF * 1000;
    kcp_conn->rx_minrto = KCP_RTO_MIN * 1000;
    kcp_conn->snd_wnd = KCP_WND_SND;
    kcp_conn->rcv_wnd = KCP_WND_RCV;
    kcp_conn->rmt_wnd = KCP_WND_RCV;
    kcp_conn->cwnd = 0;
    kcp_conn->probe = 0;
    kcp_conn->current = 0;
    kcp_conn->ts_flush = 0;
    kcp_conn->nrcv_buf = 0;
    kcp_conn->nsnd_buf = 0;
    kcp_conn->nsnd_buf_unused = 0;
    kcp_conn->nrcv_buf_unused = 0;
    kcp_conn->nrcv_que = 0;
    kcp_conn->nsnd_que = 0;
    kcp_conn->nsnd_pkt_next = 0;
    kcp_conn->incr = 0;
    kcp_conn->win_ts_probe = 0;
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
    kcp_conn->mtu_probe_ctx->mtu_current = (remote_host->sa.sa_family == AF_INET6) ? ETHERNET_MTU_V6_MIN : ETHERNET_MTU_V4_MIN;
    kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
    kcp_conn->mtu_probe_ctx->on_probe_completed = on_mtu_probe_completed;
    kcp_conn->mtu_probe_ctx->timeout = DEFAULT_MTU_PROBE_TIMEOUT;

    kcp_conn->ping_ctx = (ping_ctx_t *)malloc(sizeof(ping_ctx_t));
    memset(kcp_conn->ping_ctx, 0, sizeof(ping_ctx_t));
    kcp_conn->ping_ctx->keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT;
    kcp_conn->ping_ctx->keepalive_interval = DEFAULT_KEEPALIVE_INTERVAL;
    kcp_conn->ping_ctx->keepalive_retries = DEFAULT_KEEPALIVE_RETRIES;
    list_init(&kcp_conn->ping_ctx->ping_request_queue);

    kcp_conn->read_cb = on_kcp_read_event;
    kcp_conn->write_cb = on_kcp_write_event;
    kcp_conn->read_event_cb = NULL;

    // statistics
    kcp_conn->ping_count = 0;
    kcp_conn->pong_count = 0;
    kcp_conn->tx_bytes = 0;
    kcp_conn->rtx_bytes = 0;
}

void kcp_connection_destroy(kcp_connection_t *kcp_conn)
{
    // 从红黑树中移除连接
    connection_set_erase_node(&kcp_conn->kcp_ctx->connection_set, kcp_conn);
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
            event_free(kcp_conn->mtu_probe_ctx->probe_timeout_event);
            kcp_conn->mtu_probe_ctx->probe_timeout_event = NULL;
        }

        free(kcp_conn->mtu_probe_ctx);
        kcp_conn->mtu_probe_ctx = NULL;
    }

    // 释放ping上下文
    if (kcp_conn->ping_ctx) {
        // 清理ping请求队列
        if (!list_empty(&kcp_conn->ping_ctx->ping_request_queue)) {
            ping_session_t *pos = NULL;
            ping_session_t *next = NULL;
            list_for_each_entry_safe(pos, next, &kcp_conn->ping_ctx->ping_request_queue, node) {
                list_del_init(&pos->node);
                free(pos);
            }
        }

        free(kcp_conn->ping_ctx);
        kcp_conn->ping_ctx = NULL;
    }

    // 释放超时事件
    kcp_conn->need_write_timer_event = false;
    if (kcp_conn->syn_timer_event) {
        event_free(kcp_conn->syn_timer_event);
        kcp_conn->syn_timer_event = NULL;
    }

    if (kcp_conn->fin_timer_event) {
        event_free(kcp_conn->fin_timer_event);
        kcp_conn->fin_timer_event = NULL;
    }

    // 清理SYN/FIN包
    if (!list_empty(&kcp_conn->kcp_proto_header_list)) {
        kcp_proto_header_t *pos = NULL;
        kcp_proto_header_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->kcp_proto_header_list, node_list) {
            list_del_init(&pos->node_list);

            if ((pos->cmd & KCP_CMD_OPT) && !list_empty(&pos->options)) {
                kcp_option_t *opt_pos = NULL;
                kcp_option_t *opt_next = NULL;
                list_for_each_entry_safe(opt_pos, opt_next, &pos->options, node) {
                    list_del_init(&opt_pos->node);
                    if (opt_pos->tag == KCP_OPTION_TAG_MTU) {
                        // do nothing
                    } else {
                        free(opt_pos->buf_value);
                    }
                    free(opt_pos);
                }
            }

            free(pos);
        }
    }

    // 清理ACK包
    if (!list_empty(&kcp_conn->ack_item)) {
        kcp_ack_t *pos = NULL;
        kcp_ack_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->ack_item, node) {
            list_del_init(&pos->node);
            free(pos);
        }
    }

    // 清理发送队列
    if (!list_empty(&kcp_conn->snd_queue)) {
        kcp_segment_t *pos = NULL;
        kcp_segment_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->snd_queue, node_list) {
            list_del_init(&pos->node_list);
            free(pos);
        }
    }

    // 清理发送缓冲区
    if (!list_empty(&kcp_conn->snd_buf)) {
        kcp_segment_t *pos = NULL;
        kcp_segment_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->snd_buf, node_list) {
            list_del_init(&pos->node_list);
            free(pos);
        }
    }

    // 清理发送缓冲区
    if (!list_empty(&kcp_conn->snd_buf_unused)) {
        kcp_segment_t *pos = NULL;
        kcp_segment_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->snd_buf_unused, node_list) {
            list_del_init(&pos->node_list);
            free(pos);
        }
    }

    // 清理接收队列
    if (!list_empty(&kcp_conn->rcv_queue)) {
        kcp_segment_t *pos = NULL;
        kcp_segment_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->rcv_queue, node_list) {
            list_del_init(&pos->node_list);
            free(pos);
        }
    }

    // 清理接收缓冲区
    if (!list_empty(&kcp_conn->rcv_buf)) {
        kcp_segment_t *pos = NULL;
        kcp_segment_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->rcv_buf, node_list) {
            list_del_init(&pos->node_list);
            free(pos);
        }
    }

    // 清理接收缓冲区
    if (!list_empty(&kcp_conn->rcv_buf_unused)) {
        kcp_segment_t *pos = NULL;
        kcp_segment_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->rcv_buf_unused, node_list) {
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
        KCP_LOGW("invalid conv: %u", kcp_header->conv);
        return INVALID_KCP_HEADER;
    }

    data_offset += 4;
    kcp_header->cmd = *(uint8_t *)(data_offset); // 命令
    data_offset += 1;
    kcp_header->frg = *(uint8_t *)(data_offset); // 分片
    data_offset += 1;
    kcp_header->wnd = le16toh(*(uint16_t *)(data_offset)); // 窗口大小
    data_offset += 2;

    uint8_t cmd = kcp_header->cmd ^ KCP_CMD_OPT;
    switch (cmd) {
    case KCP_CMD_ACK: {
        kcp_header->ack_data.packet_ts = le64toh(*(uint64_t *)(data_offset)); // 时间戳
        data_offset += 8;
        kcp_header->ack_data.ack_ts = le64toh(*(uint64_t *)(data_offset)); // ACK时间戳
        data_offset += 8;
        kcp_header->ack_data.sn = le32toh(*(uint32_t *)(data_offset)); // 序列号
        data_offset += 4;
        kcp_header->ack_data.una = le32toh(*(uint32_t *)(data_offset)); // 未确认序列号
        data_offset += 4;
        break;
    }
    case KCP_CMD_SYN:
    case KCP_CMD_FIN: {
        kcp_header->syn_fin_data.packet_ts = le64toh(*(uint64_t *)data_offset);
        data_offset += 8;
        kcp_header->syn_fin_data.ts = le64toh(*(uint64_t *)data_offset);
        data_offset += 8;
        kcp_header->syn_fin_data.packet_sn = le32toh(*(uint32_t *)data_offset);
        data_offset += 4;
        kcp_header->syn_fin_data.rand_sn = le32toh(*(uint32_t *)data_offset);
        data_offset += 4;
        break;
    }
    case KCP_CMD_PING:
    case KCP_CMD_PONG: {
        kcp_header->ping_data.packet_ts = le64toh(*(uint64_t *)data_offset); // 时间戳
        data_offset += 8;
        kcp_header->ping_data.ts = le64toh(*(uint64_t *)data_offset); // PING/PONG时间戳
        data_offset += 8;
        kcp_header->ping_data.sn = le64toh(*(uint64_t *)data_offset); // PONG时间戳
        data_offset += 8;
        break;
    }
    default: {
        kcp_header->packet_data.ts = le32toh(*(uint32_t *)(data_offset)); // 时间戳
        data_offset += 8;
        kcp_header->packet_data.sn = le32toh(*(uint32_t *)(data_offset)); // 序列号
        data_offset += 4;
        kcp_header->packet_data.psn = le32toh(*(uint32_t *)(data_offset)); // 包序列号
        data_offset += 4;
        kcp_header->packet_data.una = le32toh(*(uint32_t *)(data_offset)); // 未确认序列号
        data_offset += 4;
        kcp_header->packet_data.len = le32toh(*(uint32_t *)(data_offset)); // 数据长度
        data_offset += 4;
        kcp_header->packet_data.data = NULL;
        if (kcp_header->packet_data.len > 0) {
            kcp_header->packet_data.data = (char *)data_offset; // 数据
        }
        data_offset += kcp_header->packet_data.len;

        if (kcp_header->packet_data.len > (data_size - KCP_HEADER_SIZE)) {
            KCP_LOGE("invalid packet data length: %u, data_size: %zu", kcp_header->packet_data.len, data_size);
            return INVALID_KCP_HEADER;
        }
        break;
    }
    }

    if (kcp_header->cmd & KCP_CMD_OPT) {
        // 解析选项
        while (data_offset < (*data + data_size)) {
            if ((data_size - (data_offset - *data)) < 2) {
                KCP_LOGE("invalid option data size: %zu", data_size - (data_offset - *data));
                return INVALID_KCP_HEADER;
            }

            uint8_t tag = *(uint8_t *)data_offset; // 选项标签
            data_offset += 1;
            uint8_t length = *(uint8_t *)data_offset; // 选项长度
            data_offset += 1;

            if (length > (data_size - (data_offset - *data))) {
                KCP_LOGE("invalid option length: %u, data_size: %zu", length, data_size - (data_offset - *data));
                return INVALID_KCP_HEADER;
            }

            kcp_option_t *option = (kcp_option_t *)malloc(sizeof(kcp_option_t));
            list_init(&option->node);
            option->tag = tag;
            option->length = length;

            switch (tag) {
            case KCP_OPTION_TAG_MTU:
                option->u64_value = le32toh(*(uint32_t *)data_offset);
                KCP_LOGI("kcp option tag: %u, length: %u, value: %lu", option->tag, option->length, option->u64_value);
                break;
            default:
                KCP_LOGE("unknown option tag: %u", tag);
                return INVALID_KCP_HEADER;
            }

            data_offset += length;
            list_add_tail(&option->node, &kcp_header->options);
        }
    }

    *data = data_offset;
    return NO_ERROR;
}

int32_t kcp_proto_header_encode(const kcp_proto_header_t *kcp_header, char *buffer, size_t buffer_size)
{
    if (buffer_size < KCP_HEADER_SIZE) {
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

    uint8_t cmd = kcp_header->cmd ^ KCP_CMD_OPT;
    uint32_t lengeth = 0;
    if (cmd == KCP_CMD_ACK) {
        *(uint64_t *)buffer_offset = htole64(kcp_header->ack_data.packet_ts);
        buffer_offset += sizeof(uint64_t);
        *(uint64_t *)buffer_offset = htole64(kcp_header->ack_data.ack_ts);
        buffer_offset += sizeof(uint64_t);
        *(uint32_t *)buffer_offset = htole32(kcp_header->ack_data.sn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->ack_data.una);
        buffer_offset += 4;
    } else if (cmd == KCP_CMD_SYN || cmd == KCP_CMD_FIN) {
        *(uint64_t *)buffer_offset = htole64(kcp_header->syn_fin_data.packet_ts);
        buffer_offset += 8;
        *(uint64_t *)buffer_offset = htole64(kcp_header->syn_fin_data.ts);
        buffer_offset += 8;
        *(uint32_t *)buffer_offset = htole32(kcp_header->syn_fin_data.packet_sn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->syn_fin_data.rand_sn);
        buffer_offset += 4;
    } else if (cmd == KCP_CMD_PING || cmd == KCP_CMD_PONG) {
        *(uint64_t *)buffer_offset = htole64(kcp_header->ping_data.packet_ts);
        buffer_offset += 8;
        *(uint64_t *)buffer_offset = htole64(kcp_header->ping_data.ts);
        buffer_offset += 8;
        *(uint32_t *)buffer_offset = htole64(kcp_header->ping_data.sn);
        buffer_offset += 8;
    } else {
        if (buffer_size < (KCP_HEADER_SIZE + kcp_header->packet_data.len)) {
            KCP_LOGE("buffer size is too small: %zu, need: %zu", buffer_size, (KCP_HEADER_SIZE + kcp_header->packet_data.len));
            return BUFFER_TOO_SMALL;
        }

        *(uint64_t *)buffer_offset = htole64(kcp_header->packet_data.ts);
        buffer_offset += 8;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.sn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.psn);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.una);
        buffer_offset += 4;
        *(uint32_t *)buffer_offset = htole32(kcp_header->packet_data.len);
        buffer_offset += 4;
        if (kcp_header->packet_data.len > 0) {
            memcpy(buffer_offset, kcp_header->packet_data.data, kcp_header->packet_data.len);
        }

        buffer_offset += kcp_header->packet_data.len;
        lengeth = kcp_header->packet_data.len;
    }

    if ((kcp_header->cmd & KCP_CMD_OPT) && !list_empty(&kcp_header->options)) {
        kcp_option_t *pos = NULL;

        list_for_each_entry(pos, &kcp_header->options, node) {
            if (buffer_size < (KCP_HEADER_SIZE + lengeth + pos->length)) {
                KCP_LOGE("buffer size is too small: %zu, need: %zu", buffer_size, (KCP_HEADER_SIZE + lengeth + pos->length));
                return BUFFER_TOO_SMALL;
            }

            *buffer_offset = pos->tag;
            buffer_offset += 1;
            *buffer_offset = pos->length;
            buffer_offset += 1;
            switch (pos->tag) {
            case KCP_OPTION_TAG_MTU:
                *(uint32_t *)buffer_offset = htole32((uint32_t)pos->u64_value);
                buffer_offset += 4;
                KCP_LOGI("kcp option tag: %u, length: %u, value: %lu", pos->tag, pos->length, pos->u64_value);
                break;
            default:
                return INVALID_PARAM;
            }

            // 2字节tag和length, 加上实际数据长度
            lengeth += (2 + pos->length);
        }
    }

    return (KCP_HEADER_SIZE + lengeth);
}

int32_t kcp_segment_encode(const kcp_segment_t *segment, char *buffer, size_t buffer_size)
{
    if (buffer_size < (KCP_HEADER_SIZE + segment->len)) {
        return BUFFER_TOO_SMALL;
    }
    char *buffer_offset = buffer;

    *(uint32_t *)buffer_offset = htole32(segment->conv);
    buffer_offset += 4;
    *(uint8_t *)buffer_offset = segment->cmd;
    buffer_offset += 1;
    *(uint8_t *)buffer_offset = segment->frg;
    buffer_offset += 1;
    *(uint16_t *)buffer_offset = htole16(segment->wnd);
    buffer_offset += 2;

    *(uint64_t *)buffer_offset = htole64(segment->ts);
    buffer_offset += 8;
    *(uint32_t *)buffer_offset = htole32(segment->sn);
    buffer_offset += 4;
    *(uint32_t *)buffer_offset = htole32(segment->psn);
    buffer_offset += 4;
    *(uint32_t *)buffer_offset = htole32(segment->una);
    buffer_offset += 4;
    *(uint32_t *)buffer_offset = htole32(segment->len);
    buffer_offset += 4;

    if (segment->data && segment->len > 0) {
        memcpy(buffer_offset, segment->data, segment->len);
        buffer_offset += segment->len;
    }

    return (KCP_HEADER_SIZE + segment->len);
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
    int32_t prev_una = kcp_conn->snd_una;
    switch (kcp_header->cmd) {
    case KCP_CMD_ACK:
        kcp_conn->rmt_wnd = kcp_header->wnd;
        return on_kcp_ack_pcaket(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_PUSH:
        if (kcp_header->frg > 0) {
            kcp_conn->rmt_wnd = kcp_header->wnd;
        }
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
        kcp_conn->ping_count++;
        kcp_conn->ping_ctx->keepalive_next_ts = timestamp + kcp_conn->ping_ctx->keepalive_interval;
        break;
    case KCP_CMD_PONG: { // 计算RTT
        ping_session_t *pos = NULL;
        ping_session_t *next = NULL;
        list_for_each_entry_safe(pos, next, &kcp_conn->ping_ctx->ping_request_queue, node) {
            if (pos->packet_sn == kcp_header->ping_data.sn) {
                kcp_conn->pong_count++;
                uint64_t rtt = (timestamp - kcp_conn->ping_ctx->keepalive_packet_ts) -
                            (kcp_header->ping_data.ts - kcp_header->ping_data.packet_ts);
                kcp_conn->ping_ctx->keepalive_rtt = rtt;
                kcp_conn->ping_ctx->keepalive_sn = 0;
                kcp_conn->ping_ctx->keepalive_packet_ts = 0;
                kcp_conn->ping_ctx->keepalive_next_ts = timestamp + kcp_conn->ping_ctx->keepalive_interval;
                kcp_conn->ping_ctx->keepalive_xretries = 0;
                list_del_init(&pos->node);
                free(pos);
            } else if (pos->packet_ts + 2 * kcp_conn->ping_ctx->keepalive_timeout < timestamp) {
                // 超时删除
                list_del_init(&pos->node);
                free(pos);
            }
        }

        break;
    }
    case KCP_CMD_MTU_PROBE: // NOTE MTU探测包
        return kcp_mtu_probe_received(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_MTU_ACK: // NOTE MTU探测响应包
        return kcp_mtu_ack_received(kcp_conn, kcp_header, timestamp);
    case KCP_CMD_FIN:
        return on_kcp_fin_pcaket(kcp_conn, kcp_header, timestamp);
    default:
        break;
    }

    if (kcp_conn->snd_una > prev_una && kcp_conn->cwnd < kcp_conn->rmt_wnd) {
        uint32_t mss = kcp_conn->mss;
        if (kcp_conn->cwnd < kcp_conn->ssthresh) {
            kcp_conn->cwnd++;
            kcp_conn->incr += mss;
        } else {
            if (kcp_conn->incr < mss) {
                kcp_conn->incr = mss;
            }
            kcp_conn->incr += (mss * mss) / kcp_conn->incr + (mss / 16);
            if ((kcp_conn->cwnd + 1) * mss <= kcp_conn->incr) {
                kcp_conn->cwnd = (kcp_conn->incr + mss - 1) / ((mss > 0)? mss : 1);
            }
        }
        if (kcp_conn->cwnd > kcp_conn->rmt_wnd) {
            kcp_conn->cwnd = kcp_conn->rmt_wnd;
            kcp_conn->incr = kcp_conn->rmt_wnd * mss;
        }
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
                kcp_send_packet_raw(kcp_ctx->sock, addr, data, 1);
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

                    kcp_option_t *option = NULL;
                    uint32_t mtu = kcp_connection->kcp_ctx->nic_mtu;
                    list_for_each_entry(option, &syn_packet->options, node) {
                        if (option->tag == KCP_OPTION_TAG_MTU) {
                            mtu = (uint32_t)option->u64_value;
                            break;
                        }
                    }

                    char buffer[KCP_HEADER_SIZE] = { 0 };
                    kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

                    struct iovec data[1];
                    data[0].iov_base = buffer;
                    data[0].iov_len = KCP_HEADER_SIZE;
                    if (kcp_send_packet(kcp_connection, data, 1) < 0) {
                        // NOTE 此处不会触发 EAGAIN
                        int32_t code = get_last_errno();
                        KCP_LOGE("kcp send packet error. [%d, %s]", code, errno_string(code));
                        kcp_connection->state = KCP_STATE_DISCONNECTED;
                        kcp_ctx->callback.on_error(kcp_connection->kcp_ctx, kcp_connection, WRITE_ERROR);
                    } else {
                        // NOTE 对端未收到ACK, 会重发SYN
                        if (kcp_connection->syn_timer_event) {
                            event_free(kcp_connection->syn_timer_event);
                            kcp_connection->syn_timer_event = NULL;
                        }

                        if (kcp_connection->state == KCP_STATE_SYN_SENT) {
                            kcp_connection->state = KCP_STATE_CONNECTED;
                            status = true;
                            uint64_t ts = kcp_time_monotonic_ms();
                            kcp_connection->ts_flush = ts + kcp_connection->interval;
                            kcp_connection->need_write_timer_event = true;
                            kcp_connection->mtu = mtu;
                            kcp_connection->mss = mtu - KCP_HEADER_SIZE;
                            kcp_ctx->callback.on_connected(kcp_connection, NO_ERROR);
                            kcp_connection->ping_ctx->keepalive_next_ts = ts + kcp_connection->ping_ctx->keepalive_interval;
                            // kcp_mtu_probe(kcp_connection, DEFAULT_MTU_PROBE_TIMEOUT, 2);
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
        list_del_init(&pos->node_list); \
        list_add_tail(&pos->node_list, &kcp_conn->snd_buf_unused); \
        ++kcp_conn->nsnd_buf_unused; \
        --kcp_conn->nsnd_buf; \
    } while (false)

static int32_t on_kcp_ack_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    if (kcp_conn->state == KCP_STATE_DISCONNECTED) {
        return NO_ERROR;
    }

    // update ping ts
    kcp_conn->ping_ctx->keepalive_next_ts = timestamp + kcp_conn->ping_ctx->keepalive_interval;

    if (kcp_header->ack_data.sn < kcp_conn->snd_una) {
        // 如果ack的序号小于snd_una, 则忽略此ack
        return NO_ERROR;
    }

    if (kcp_header->ack_data.sn > kcp_conn->snd_nxt) {
        // 如果ack的序号大于snd_nxt, 则说明此ack是无效的
        return NO_ERROR;
    }

    uint32_t max_ack = 0;
    kcp_segment_t *pos = NULL;
    kcp_segment_t *next = NULL;
    list_for_each_entry_safe(pos, next, &kcp_conn->snd_buf, node_list) {
        if (pos->sn < kcp_header->ack_data.sn) {
            // 当前包的序号小于ack的序号, 表示当前包被跳过
            pos->fastack++;
        } else if (pos->sn == kcp_header->ack_data.sn) {
            max_ack = pos->sn;
            int32_t timestamp_tmp = timestamp;
            int32_t ts_tmp = pos->ts;
            int32_t ack_ts_tmp = kcp_header->ack_data.ack_ts;
            int32_t packet_ts_tmp = kcp_header->ack_data.packet_ts;

            // NOTE 发送重传时无法确认ACK是哪次重传包的ACK, 故计算出的RTT和RTO会不准确, 一般情况会偏高
            if (pos->xmit == 1) { // 如果未发生重传, 则计算RTT和RTO
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
                kcp_conn->rx_rto = CLAMP(rto, kcp_conn->rx_minrto, KCP_RTO_MAX * 1000);
                KCP_LOGI("RTT: %u, RTO: %u", kcp_conn->rx_srtt, kcp_conn->rx_rto);
            }

            kcp_conn->tx_bytes += pos->len; // 累加发送的字节数
            if (pos->xmit > 1) { // 本包是重传包
                kcp_conn->rtx_bytes += pos->len; // 累加重传的字节数
            }

            HANDLE_SND_BUF(kcp_conn);
            break;
        } else {
            // 序号是顺序排列的, 当前包的序号大于ack的序号, 则说明此ack是已被确认的
            break;
        }
    }

    // 解析una
    if (!list_empty(&kcp_conn->snd_buf)) {
        pos = NULL;
        next = NULL;

        list_for_each_entry_safe(pos, next, &kcp_conn->snd_buf, node_list) {
            if (pos->sn < kcp_header->ack_data.una) {
                HANDLE_SND_BUF(kcp_conn);
            } else {
                break;
            }
        }
    }

    // 收缩snd_buf_unused空间
    int32_t nsnd_buf_diff = (int32_t)kcp_conn->nsnd_buf_unused - (int32_t)kcp_conn->snd_wnd;
    for (int32_t i = 0; i < nsnd_buf_diff; ++i) {
        if (list_empty(&kcp_conn->snd_buf_unused)) {
            break;
        }

        pos = list_first_entry(&kcp_conn->snd_buf_unused, kcp_segment_t, node_list);
        list_del_init(&pos->node_list);
        free(pos);
    }

    if (list_empty(&kcp_conn->snd_buf)) {
        kcp_conn->snd_una = kcp_conn->snd_nxt; // 如果snd_buf为空, 则snd_una等于snd_nxt
    } else {
        kcp_segment_t *first = list_first_entry(&kcp_conn->snd_buf, kcp_segment_t, node_list);
        kcp_conn->snd_una = first->sn; // snd_una为snd_buf的第一个包的序号
    }

    return NO_ERROR;
}

int32_t on_kcp_push_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    // update ping ts
    kcp_conn->ping_ctx->keepalive_next_ts = timestamp + kcp_conn->ping_ctx->keepalive_interval;

    if (kcp_header->packet_data.sn >= (kcp_conn->rcv_nxt + kcp_conn->rcv_wnd)) {
        // NOTE 如果收到PUSH包的序号超过接收窗口大小时, 发送可接收包大小, 并丢弃此包
        kcp_conn->probe |= KCP_ASK_TELL;
        return NO_ERROR;
    }

    // FIXME 解决序号溢出导致的回环问题
    if (kcp_header->packet_data.sn < kcp_conn->rcv_nxt) { // 此包已被接收过
        KCP_LOGW("packet is received: %u, %u", kcp_header->packet_data.sn, kcp_conn->rcv_nxt);
        return NO_ERROR;
    }

    // push ack
    kcp_ack_t *ack_item = (kcp_ack_t *)malloc(sizeof(kcp_ack_t));
    if (ack_item == NULL) {
        return NO_MEMORY;
    }
    list_init(&ack_item->node);
    kcp_segment_t *kcp_segment = kcp_segment_recv_get(kcp_conn);
    if (kcp_segment == NULL) {
        free(ack_item);
        return NO_MEMORY;
    }
    list_init(&kcp_segment->node_list);

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
    list_for_each_entry_safe(pos, next, &kcp_conn->rcv_buf, node_list) {
        if (pos->psn == kcp_segment->psn && pos->frg == kcp_segment->frg) { // 相同包
            repeat = true;
            break;
        }

        if (kcp_segment->sn > pos->sn) {
            break;
        }
    }

    if (repeat) {
        kcp_segment_recv_put(kcp_conn, kcp_segment);
        list_del_init(&ack_item->node);
        free(ack_item);
        return NO_ERROR;
    }

    kcp_segment_t *last = NULL;
    list_for_each_entry_safe(pos, next, &kcp_conn->rcv_buf, node_list) {
        if (pos->psn == kcp_segment->psn) {
            last = pos;
            break;
        }
    }

    if (last) { // 属于同一包
        pos = last;
        list_for_each_entry_safe_from(pos, next, &kcp_conn->rcv_buf, node_list) {
            if (pos->psn != kcp_segment->psn) { // 同一个包的最后一个节点
                break;
            }
            if (pos->frg > kcp_segment->frg) { // 同一包的不同分片
                break;
            }

            last = pos;
        }

        list_add(&kcp_segment->node_list, &last->node_list);
    } else { // 新包
        list_add_tail(&kcp_segment->node_list, &kcp_conn->rcv_buf);
    }

    ++kcp_conn->nrcv_buf;
    if (kcp_segment->sn == kcp_conn->rcv_nxt) {
        kcp_conn->rcv_nxt++;
    }

    // 解析rcv_buf, 组成完整数据包
    kcp_segment_t *first = NULL;
    uint16_t packet_count = 0;
    list_for_each_entry_safe(first, next, &kcp_conn->rcv_buf, node_list) {
        if (first->frg == 0) {
            packet_count = (uint16_t)first->wnd; // NOTE 起始packet, 表示packet个数
            pos = first;
            uint16_t count = 0;
            kcp_segment_t *temp_next = NULL;
            list_for_each_entry_safe_from(pos, temp_next, &kcp_conn->rcv_buf, node_list) {
                if (first->psn != pos->psn) { // 下一个包
                    break;
                }
                ++count;
            }

            if (packet_count == count) { // 完整的一包
                pos = first;
                int32_t size = 0;
                list_for_each_entry_safe_from(pos, temp_next, &kcp_conn->rcv_buf, node_list) {
                    if (first->psn != pos->psn) { // 下一个包
                        break;
                    }
                    size += pos->len;
                    list_del_init(&pos->node_list);
                    list_add_tail(&pos->node_list, &kcp_conn->rcv_queue);
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
        list_init(&kcp_fin_header->node_list);
        kcp_fin_header->conv = kcp_conn->conv;
        kcp_fin_header->cmd = KCP_CMD_FIN;
        kcp_fin_header->frg = 0;
        kcp_fin_header->wnd = 0;
        kcp_fin_header->syn_fin_data.packet_ts = kcp_time_monotonic_us();
        kcp_fin_header->syn_fin_data.ts = kcp_fin_header->syn_fin_data.packet_ts;
        kcp_fin_header->syn_fin_data.packet_sn = packet_sn;
        kcp_fin_header->syn_fin_data.rand_sn = XXH32(&kcp_fin_header->syn_fin_data.ts, sizeof(kcp_fin_header->syn_fin_data.ts), 0);
        list_add_tail(&kcp_fin_header->node_list, &kcp_conn->kcp_proto_header_list);

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        kcp_send_packet(kcp_conn, data, 1);

        evtimer_del(kcp_conn->fin_timer_event);
        uint32_t timeout_ms = kcp_conn->receive_timeout;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_conn->fin_timer_event, &tv);
        return;
    }

    if (kcp_conn->kcp_ctx->callback.on_closed) {
        kcp_conn->state = KCP_STATE_DISCONNECTED;
        kcp_conn->kcp_ctx->callback.on_closed(kcp_conn, TIMED_OUT);
    }
    kcp_connection_destroy(kcp_conn);
}

int32_t on_kcp_fin_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header, uint64_t timestamp)
{
    // 1、响应FIN包, 修改状态
    kcp_proto_header_t *kcp_fin_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
    list_init(&kcp_fin_header->node_list);
    kcp_fin_header->conv = kcp_conn->conv;
    kcp_fin_header->cmd = KCP_CMD_FIN;
    kcp_fin_header->frg = 0;
    kcp_fin_header->wnd = 0;
    kcp_fin_header->syn_fin_data.packet_ts = timestamp;
    kcp_fin_header->syn_fin_data.ts = timestamp;
    kcp_fin_header->syn_fin_data.packet_sn = kcp_header->syn_fin_data.rand_sn;
    kcp_fin_header->syn_fin_data.rand_sn = XXH32(&timestamp, sizeof(timestamp), 0);
    list_add_tail(&kcp_fin_header->node_list, &kcp_conn->kcp_proto_header_list);

    char buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_encode(kcp_fin_header, buffer, KCP_HEADER_SIZE);
    struct iovec data[1];
    data[0].iov_base = buffer;
    data[0].iov_len = KCP_HEADER_SIZE;

    // 发送失败或丢包靠超时重传
    kcp_send_packet(kcp_conn, data, 1);
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

int32_t on_kcp_ping_timeout(kcp_connection_t *kcp_conn, uint64_t timestamp)
{
    if (list_empty(&kcp_conn->ping_ctx->ping_request_queue)) {
        return 0;
    }

    ping_session_t *ping_session = list_last_entry(&kcp_conn->ping_ctx->ping_request_queue, ping_session_t, node);
    if (timestamp >= (kcp_conn->ping_ctx->keepalive_timeout + ping_session->packet_ts)) {
        kcp_conn->ping_ctx->keepalive_next_ts = timestamp - 1;
        list_del_init(&ping_session->node);
        free(ping_session);
        kcp_conn->ping_ctx->keepalive_xretries++;

        if (kcp_conn->ping_ctx->keepalive_xretries >= kcp_conn->ping_ctx->keepalive_retries) {
            kcp_conn->state = KCP_STATE_DISCONNECTED;
            kcp_conn->kcp_ctx->callback.on_error(kcp_conn->kcp_ctx, kcp_conn, KEEPALIVE_ERROR);
            return 1;
        }
    }

    return 0;
}

kcp_segment_t *kcp_segment_send_get(kcp_connection_t *kcp_conn)
{
    kcp_segment_t *kcp_segment = NULL;
    if (!list_empty(&kcp_conn->snd_buf_unused)) {
        kcp_segment = list_first_entry(&kcp_conn->snd_buf_unused, kcp_segment_t, node_list);
        list_del_init(&kcp_segment->node_list);
        --kcp_conn->nsnd_buf_unused;
    } else {
        kcp_segment = (kcp_segment_t *)malloc(sizeof(kcp_segment_t) + ETHERNET_MTU);
        if (kcp_segment != NULL) {
            list_init(&kcp_segment->node_list);
        }
    }

    return kcp_segment;
}

void kcp_segment_send_put(kcp_connection_t *kcp_conn, kcp_segment_t *segment)
{
    if (kcp_conn->nsnd_buf_unused < kcp_conn->snd_wnd) {
        list_add_tail(&segment->node_list, &kcp_conn->snd_buf_unused);
        ++kcp_conn->nsnd_buf_unused;
    } else {
        free(segment);
    }
}

kcp_segment_t *kcp_segment_recv_get(kcp_connection_t *kcp_conn)
{
    kcp_segment_t *kcp_segment = NULL;
    if (!list_empty(&kcp_conn->rcv_buf_unused)) {
        kcp_segment = list_first_entry(&kcp_conn->rcv_buf_unused, kcp_segment_t, node_list);
        list_del_init(&kcp_segment->node_list);
        --kcp_conn->nrcv_buf_unused;
    } else {
        kcp_segment = (kcp_segment_t *)malloc(sizeof(kcp_segment_t) + ETHERNET_MTU);
        if (kcp_segment != NULL) {
            list_init(&kcp_segment->node_list);
        }
    }

    return kcp_segment;
}

void kcp_segment_recv_put(kcp_connection_t *kcp_conn, kcp_segment_t *segment)
{
    if (kcp_conn->nrcv_buf_unused < kcp_conn->rcv_wnd) {
        list_add_tail(&segment->node_list, &kcp_conn->rcv_buf_unused);
        ++kcp_conn->nrcv_buf_unused;
    } else {
        free(segment);
    }
}