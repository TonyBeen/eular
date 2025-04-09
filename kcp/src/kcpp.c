#include "kcpp.h"

#include <string.h>
#include <assert.h>

#include <event2/event.h>
#include <event2/buffer.h>

#include <xxhash.h>

#include "kcp_inc.h"
#include "kcp_error.h"
#include "kcp_endian.h"
#include "kcp_protocol.h"
#include "kcp_net_utils.h"
#include "connection_set.h"
#include "kcp_mtu.h"
#include "kcp_time.h"
#include "kcp_log.h"

static void kcp_parse_packet(struct KcpContext *kcp_ctx, const char *buffer, size_t buffer_size, const sockaddr_t *addr)
{
    if (buffer_size < KCP_HEADER_SIZE) {
        return;
    }

    const char *buffer_offset = buffer;
    size_t buffer_remain = buffer_size;
    do {
        kcp_proto_header_t kcp_header;
        if (NO_ERROR != kcp_proto_parse(&kcp_header, &buffer_offset, buffer_remain)) {
            KCP_LOGW("kcp parse packet error");
            break;
        }
        buffer_remain = buffer + buffer_size - buffer_offset;

        kcp_connection_t *kcp_connection = connection_set_search(&kcp_ctx->connection_set, kcp_header.conv);
        if (kcp_header.cmd == KCP_CMD_SYN) {
            kcp_syn_node_t *syn_node = (kcp_syn_node_t *)malloc(sizeof(kcp_syn_node_t));
            if (syn_node == NULL) {
                kcp_ctx->callback.on_error(kcp_ctx, NO_MEMORY);
                return;
            }
            list_init(&syn_node->node);

            syn_node->conv = kcp_header.conv;
            memcpy(&syn_node->remote_host, addr, sizeof(sockaddr_t));
            syn_node->packet_sn = kcp_header.syn_data.packet_sn;
            syn_node->rand_sn = kcp_header.syn_data.rand_sn;
            syn_node->packet_ts = kcp_header.syn_data.packet_ts;
            syn_node->syn_ts = kcp_header.syn_data.syn_ts;
            list_add_tail(&kcp_ctx->syn_queue, syn_node);

            on_kcp_syn_received(kcp_ctx, syn_node);
        } else if (kcp_connection == NULL) { // 发送rst
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
            kcp_send_packet_raw(kcp_ctx->sock, addr, &data, sizeof(data));
            continue;
        }

        kcp_connection->read_cb(kcp_connection, &kcp_header, addr);
        if (kcp_connection->state == KCP_STATE_DISCONNECTED) {
            kcp_connection_destroy(kcp_connection);
            break;
        }
    } while (buffer_offset < (buffer + buffer_size)); // ACK 包可能会有多个
}

static void kcp_read_cb(int fd, short ev, void *arg)
{
    assert(arg != NULL);
    struct KcpContext *kcp_ctx = (struct KcpContext *)arg;
    if (kcp_ctx == NULL) {
        return;
    }

#if defined(OS_LINUX)
    // process icmp error
    while (true) {
        sockaddr_t remote_addr;
        socklen_t addr_len = sizeof(sockaddr_t);

        struct cmsghdr *cmsg = NULL;
        char cmsgbuf[4096] = {0};
        struct msghdr msg;
        struct iovec iov;
        iov.iov_base = kcp_ctx->read_buffer;
        iov.iov_len = kcp_ctx->read_buffer_size;
    
        msg.msg_name = &remote_addr;
        msg.msg_namelen = addr_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        msg.msg_flags = 0;

        ssize_t nreads = recvmsg(kcp_ctx->sock, &msg, MSG_ERRQUEUE | MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nreads < 0) {
            int32_t code = get_last_errno();
            if (code == EAGAIN || code == EWOULDBLOCK) {
                break;
            } else {
                kcp_ctx->callback.on_error(kcp_ctx, READ_ERROR);
                break;
            }
        }

        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_type != IP_RECVERR) {
                continue;
            }
            if (cmsg->cmsg_level != SOL_IP) {
                continue;
            }
            struct sock_extended_err *err = (struct sock_extended_err *)CMSG_DATA(cmsg);
            if (err == NULL) {
                continue;
            }
            if (err->ee_origin != SO_EE_ORIGIN_ICMP) {
                continue;
            }

            if (err->ee_type == ICMP_DEST_UNREACH) {
                switch (err->ee_code) {
                case ICMP_FRAG_NEEDED:
                    kcp_process_icmp_fragmentation(kcp_ctx, kcp_ctx->read_buffer, nreads, &remote_addr, err->ee_info);
                    break;
                case ICMP_NET_UNREACH:
                case ICMP_HOST_UNREACH:
                case ICMP_PROT_UNREACH:
                case ICMP_PORT_UNREACH:
                    kcp_process_icmp_unreach(kcp_ctx, kcp_ctx->read_buffer, nreads, &remote_addr);
                    break;
                default:
                    break;
                }
            } else { // 其他未知错误
                KCP_LOGE("ICMP Error. type: %u, code: %u", err->ee_type, err->ee_code);
                kcp_process_icmp_error(kcp_ctx, kcp_ctx->read_buffer_size, nreads, &remote_addr);
            }
        }
    }

    while (true) {
        sockaddr_t remote_addr;
        socklen_t addr_len = sizeof(sockaddr_t);
        struct msghdr msg;
        struct cmsghdr *cmsg = NULL;
        char cmsgbuf[1024] = {0};
        struct iovec iov;
        iov.iov_base = kcp_ctx->read_buffer;
        iov.iov_len = kcp_ctx->read_buffer_size;

        msg.msg_name = &remote_addr;
        msg.msg_namelen = addr_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        msg.msg_flags = 0;
        ssize_t nreads = recvmsg(kcp_ctx->sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nreads < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                return;
            }
        }

        kcp_parse_packet(kcp_ctx, kcp_ctx->read_buffer, nreads, &remote_addr);
    }
#else
    sockaddr_t remote_addr;
    socklen_t addr_len = sizeof(sockaddr_t);
    ssize_t nreads = recvfrom(kcp_ctx->sock, kcp_ctx->read_buffer, kcp_ctx->read_buffer_size, 0, (struct sockaddr *)&remote_addr, &addr_len);
    if (nreads == SOCKET_ERROR) {
        return;
    }
#endif
}

static void kcp_write_cb(int fd, short ev, void *arg)
{
    assert(arg != NULL);
    struct KcpContext *kcp_ctx = (struct KcpContext *)arg;
    if (kcp_ctx == NULL) {
        return;
    }

    kcp_connection_t *pos = NULL;
    kcp_connection_t *next = NULL;
    list_for_each_entry_safe(pos, next, &kcp_ctx->conn_write_event_queue, node_list) {
        if (pos->write_cb) {
            int32_t status = pos->write_cb(pos, 0);
            if (status == NO_ERROR) {
                list_del_init(&pos->node_list);
            } else if (status == OP_TRY_AGAIN) { // 缓存区已满
                break;
            } else {
                kcp_ctx->callback.on_error(kcp_ctx, status);
                break;
            }
        } else {
            list_del_init(&pos->node_list);
        }
    }

    if (list_empty(&kcp_ctx->conn_write_event_queue)) {
        event_del(kcp_ctx->write_event);
    }
}

static void kcp_write_timeout(int fd, short ev, void *arg)
{
    kcp_context_t *kcp_ctx = (kcp_context_t *)arg;
    // 遍历所有连接, 超时未发送数据则触发写事件
    uint64_t current_time_ms = kcp_time_monotonic_ms();
    kcp_connection_t *pos = NULL;
    for (pos = connection_first(&kcp_ctx->connection_set); pos != NULL; pos = connection_next(pos)) {
        if (pos->need_write_timer_event) {
            pos->write_cb(pos, current_time_ms);
        }
    }
}

struct KcpContext *kcp_context_create(struct event_base *base, on_kcp_error_t cb, void *user)
{
    if (base == NULL || cb == NULL) {
        return NULL;
    }

    struct KcpContext *ctx = (struct KcpContext *)malloc(sizeof(struct KcpContext));
    if (ctx == NULL) {
        return NULL;
    }
    if (!bitmap_create(&ctx->conv_bitmap, KCP_BITMAP_SIZE)) {
        free(ctx);
        return NULL;
    }

    ctx->sock = INVALID_SOCKET;
    memset(&ctx->local_addr, 0, sizeof(sockaddr_t));
    ctx->callback = (kcp_function_callback_t) {
        .on_accepted = NULL,
        .on_connected = NULL,
        .on_closed = NULL,
        .on_error = cb,
    };

    list_init(&ctx->syn_queue);
    connection_set_init(&ctx->connection_set);
    ctx->event_loop = base;
    ctx->read_event = event_new(base, -1, EV_READ | EV_PERSIST, kcp_read_cb, ctx);
    ctx->write_event = event_new(base, -1, EV_WRITE | EV_PERSIST, kcp_write_cb, ctx);
    ctx->write_timer_event = evtimer_new(base, kcp_write_timeout, ctx);
    ctx->read_buffer = (char *)malloc(ETHERNET_MTU);
    ctx->read_buffer_size = ETHERNET_MTU;
    ctx->user_data = user;
    return ctx;
}

void kcp_context_destroy(struct KcpContext *kcp_ctx)
{
    if (kcp_ctx == NULL) {
        return;
    }

    // TODO 释放资源

#if defined(OS_LINUX)
    close(kcp_ctx->sock);
#else
    closesocket(kcp_ctx->sock);
#endif

    free(kcp_ctx);
}

int32_t kcp_configure(struct KcpConnection *kcp_connection, em_config_key_t flags, const kcp_config_t *config)
{
    if (kcp_connection == NULL || config == NULL) {
        return INVALID_PARAM;
    }

    if (flags & CONFIG_KEY_NODELAY) {
        kcp_connection->nodelay = config->nodelay ? 1 : 0;
        if (kcp_connection->nodelay) {
            kcp_connection->rx_minrto = IKCP_RTO_NDL;
        } else {
            kcp_connection->rx_minrto = IKCP_RTO_MIN;
        }
    }

    if (flags & CONFIG_KEY_INTERVAL) {
        if (config->interval < KCP_INTERVAL_MIN || config->interval > KCP_INTERVAL_MAX) {
            return INVALID_PARAM;
        }

        kcp_connection->interval = config->interval;
    }

    if (flags & CONFIG_KEY_RESEND) {
        if (config->resend > KCP_FASTACK_LIMIT) {
            return INVALID_PARAM;
        }

        kcp_connection->fastresend = config->resend;
    }

    if (flags & CONFIG_KEY_NC) {
        kcp_connection->nocwnd = config->nc ? 1 : 0;
    }

    return NO_ERROR;
}

static int32_t create_socket(struct KcpContext *kcp_ctx, const sockaddr_t *addr)
{
    if (addr->sa.sa_family == AF_INET) {
        kcp_ctx->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    } else if (addr->sa.sa_family == AF_INET6) {
        kcp_ctx->sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    } else {
        return UNKNOWN_PROTO;
    }

    if (kcp_ctx->sock == INVALID_SOCKET) {
        return CREATE_SOCKET_ERROR;
    }

    return NO_ERROR;
}

int32_t kcp_ioctl(struct KcpConnection *kcp_connection, em_ioctl_t flags, void *data)
{
    if (kcp_connection == NULL || data == NULL) {
        return INVALID_PARAM;
    }

    switch (flags) {
    case IOCTL_RECEIVE_TIMEOUT:
        kcp_connection->receive_timeout = *(uint32_t *)data;
        break;
    case IOCTL_MTU_PROBE_TIMEOUT:
        kcp_connection->mtu_probe_ctx->timeout = *(uint32_t *)data;
        break;
    case IOCTL_KEEPALIVE_TIMEOUT:
        kcp_connection->keepalive_timeout = *(uint32_t *)data;
        break;
    case IOCTL_KEEPALIVE_INTERVAL:
        kcp_connection->keepalive_interval = *(uint32_t *)data;
        break;
    case IOCTL_SYN_RETRIES:
        kcp_connection->syn_retries = *(uint32_t *)data;
        break;
    case IOCTL_FIN_RETRIES:
        kcp_connection->fin_retries = *(uint32_t *)data;
        break;
    case IOCTL_WINDOW_SIZE:
        kcp_connection->rcv_wnd = MAX(IKCP_WND_RCV, *(uint32_t *)data);
        kcp_connection->snd_wnd = *(uint32_t *)data;
        break;
    default:
        return INVALID_PARAM;
    }

    return NO_ERROR;
}

int32_t kcp_bind(struct KcpContext *kcp_ctx, const sockaddr_t *addr, const char *nic)
{
    if (kcp_ctx == NULL || addr == NULL) {
        return INVALID_PARAM;
    }

    if (kcp_ctx->sock != INVALID_SOCKET) {
        return SOCKET_INUSE;
    }

    int32_t status = create_socket(kcp_ctx, addr);
    if (status != NO_ERROR) {
        return status;
    }

    status = set_socket_sendbuf(kcp_ctx->sock, 128 * 1024); // 128KB
    if (status != NO_ERROR) {
        goto _error;
    }
    status = set_socket_recvbuf(kcp_ctx->sock, 128 * 1024); // 128KB
    if (status != NO_ERROR) {
        goto _error;
    }
    status = set_socket_nonblock(kcp_ctx->sock);
    if (status != NO_ERROR) {
        goto _error;
    }
    // status = set_socket_reuseaddr(kcp_ctx->sock);
    // if (status != NO_ERROR) {
    //     goto _error;
    // }
    // status = set_socket_reuseport(kcp_ctx->sock);
    // if (status != NO_ERROR) {
    //     goto _error;
    // }
    status = set_socket_dont_fragment(kcp_ctx->sock);
    if (status != NO_ERROR) {
        goto _error;
    }
    status = set_socket_recverr(kcp_ctx->sock, addr); // 开启接收ICMP错误报文
    if (status != NO_ERROR) {
        goto _error;
    }
    if (nic != NULL) {
        status = set_socket_bind_nic(kcp_ctx->sock, nic);
        if (status != NO_ERROR) {
            goto _error;
        }
    }

    if (bind(kcp_ctx->sock, (const struct sockaddr *)addr, sizeof(sockaddr_t)) == SOCKET_ERROR) {
        status = BIND_ERROR;
        goto _error;
    }

    return NO_ERROR;

_error:
    close(kcp_ctx->sock);
    kcp_ctx->sock = INVALID_SOCKET;
    return status;
}

int32_t kcp_listen(struct KcpContext *kcp_ctx, on_kcp_connect_t cb)
{
    if (kcp_ctx == NULL || cb == NULL) {
        return INVALID_PARAM;
    }

    kcp_ctx->callback.on_connect = cb;

    event_add(kcp_ctx->read_event, NULL);
    struct timeval tv = {0, 1000}; // 1ms
    evtimer_add(kcp_ctx->write_timer_event, &tv);
    return NO_ERROR;
}

void kcp_set_accept_cb(struct KcpContext *kcp_ctx, on_kcp_accepted_t cb)
{
    if (kcp_ctx != NULL) {
        kcp_ctx->callback.on_accepted = cb;
    }
}

/**
 * @brief 发送SYN给客户端, 在指定时间内未收到ACK时重发
 * 
 * @param fd 文件描述符
 * @param ev 事件
 * @param arg 用户数据
 */
static void kcp_accept_timeout(int fd, short ev, void *arg)
{
    UNUSED_PARAM(fd);
    UNUSED_PARAM(ev);

    kcp_connection_t *kcp_connection = (kcp_connection_t *)arg;
    kcp_context_t *kcp_ctx = kcp_connection->kcp_ctx;
    if (kcp_connection->syn_retries--) {
        kcp_proto_header_t *kcp_header_last = list_last_entry(&kcp_connection->kcp_proto_header_list, kcp_proto_header_t, node_list);
        kcp_proto_header_t *kcp_syn_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
        if (kcp_syn_header == NULL) {
            uint32_t timeout_ms = kcp_connection->receive_timeout;
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            evtimer_add(kcp_connection->syn_timer_event, &tv);
            return;
        }
        memcpy(kcp_syn_header, kcp_header_last, sizeof(kcp_proto_header_t));
        list_init(&kcp_syn_header->node_list);
        list_add_tail(&kcp_connection->kcp_proto_header_list, kcp_syn_header);

        // TODO 分析因为网络问题导致的超时重发需不需要更新packet_ts
        kcp_syn_header->syn_data.packet_ts = kcp_time_monotonic_us();
        kcp_syn_header->syn_data.syn_ts = kcp_syn_header->syn_data.packet_ts;
        kcp_syn_header->syn_data.rand_sn++;

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_syn_header, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, sizeof(data));
        if (status <= 0) {
            // Linux EAGAIN, Windows EWOULDBLOCK (WSAEWOULDBLOCK)
            if (get_last_errno() != EAGAIN || get_last_errno() != EWOULDBLOCK) {
                kcp_add_write_event(kcp_connection);
            } else {
                kcp_ctx->callback.on_accepted(kcp_ctx, kcp_connection, status);
                return;
            }
        }

        uint32_t timeout_ms = kcp_connection->receive_timeout;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_connection->syn_timer_event, &tv);
        return;
    }

    if (kcp_ctx->callback.on_accepted) {
        bitmap_set(&kcp_ctx->conv_bitmap, kcp_connection->conv & ~KCP_CONV_FLAG, false);
        evtimer_free(kcp_connection->syn_timer_event);
        kcp_connection->syn_timer_event = NULL;
        kcp_connection->state = KCP_STATE_DISCONNECTED;
        kcp_ctx->callback.on_accepted(kcp_ctx, kcp_connection, TIMED_OUT);
    }
}

int32_t kcp_accept(struct KcpContext *kcp_ctx, sockaddr_t *addr, uint32_t timeout_ms)
{
    if (kcp_ctx == NULL) {
        return INVALID_PARAM;
    }

    if (list_empty(&kcp_ctx->syn_queue)) {
        return NO_PENDING_CONNECTION;
    }
    int32_t status = NO_ERROR;
    kcp_syn_node_t *syn_packet = list_first_entry(&kcp_ctx->syn_queue, kcp_syn_node_t, node);

    kcp_connection_t *kcp_connection = (kcp_connection_t *)malloc(sizeof(kcp_connection_t));
    if (kcp_connection == NULL) {
        return NO_MEMORY;
    }
    memset(kcp_connection, 0, sizeof(kcp_connection_t));
    kcp_connection_init(kcp_connection, &syn_packet->remote_host, kcp_ctx);

    do {
        if (bitmap_count(&kcp_ctx->conv_bitmap) == kcp_ctx->conv_bitmap.size) {
            status = NO_MORE_CONV;
            break;
        }

        uint32_t conv = 0;
        for (int32_t i = 1; i < kcp_ctx->conv_bitmap.size; ++i) {
            if (!bitmap_get(&kcp_ctx->conv_bitmap, i)) {
                conv = KCP_CONV_FLAG | i;
                bitmap_set(&kcp_ctx->conv_bitmap, i, true);
                break;
            }
        }
        if (conv == 0) {
            KCP_LOGE("bitmap count = %d, no more conversation", bitmap_count(&kcp_ctx->conv_bitmap));
            abort();
        }

        kcp_connection->conv = conv;
        kcp_connection->syn_timer_event = evtimer_new(kcp_ctx->event_loop, kcp_accept_timeout, kcp_connection);
        if (kcp_connection->syn_timer_event == NULL) {
            status = NO_MEMORY;
            break;
        }
        kcp_connection->state = KCP_STATE_SYN_RECEIVED;
        connection_set_insert(&kcp_ctx->connection_set, kcp_connection);
        kcp_connection->receive_timeout = timeout_ms;

        kcp_proto_header_t *kcp_syn_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
        if (kcp_syn_header == NULL) {
            status = NO_MEMORY;
            break;
        }
        list_init(&kcp_syn_header->node_list);
        kcp_proto_header_t kcp_header;
        kcp_header.conv = conv;
        kcp_header.cmd = KCP_CMD_SYN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.syn_data.packet_ts = syn_packet->syn_ts;
        kcp_header.syn_data.syn_ts = kcp_time_monotonic_us();
        kcp_header.syn_data.packet_sn = syn_packet->rand_sn; // client发送的序列
        kcp_header.syn_data.rand_sn = XXH32(&kcp_header.syn_data.syn_ts, sizeof(kcp_header.syn_data.syn_ts), 0); // server响应的序列
        memcpy(kcp_syn_header, &kcp_header, sizeof(kcp_proto_header_t));
        list_add_tail(&kcp_connection->kcp_proto_header_list, kcp_syn_header);

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

        list_del_init(&syn_packet->node);
        kcp_connection->syn_node = syn_packet;

        struct iovec data[1];
        data->iov_base = buffer;
        data->iov_len = KCP_HEADER_SIZE;
        status = kcp_send_packet(kcp_connection, &data, 1);

        // 添加SYN超时事件
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_connection->syn_timer_event, &tv);

        if (status != 1) {
            int32_t code = get_last_errno();
            if (code == EAGAIN || code == EWOULDBLOCK) {
                return kcp_add_write_event(kcp_connection);
            }

            status = WRITE_ERROR;
            break;
        }

        return status;
    } while (false);

    // NOTE kcp_accept调用时机在on_connect, kcp_accept返回失败时on_connect返回false, 由on_kcp_syn_received发送RST
    kcp_connection_destroy(kcp_connection);

    // 在on_kcp_syn_received清理节点
    return status;
}

static void kcp_connect_timeout(int fd, short ev, void *arg)
{
    kcp_connection_t *kcp_connection = (kcp_connection_t *)arg;
    if (kcp_connection->syn_retries--) {
        kcp_proto_header_t *kcp_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
        list_init(&kcp_header->node_list);
        kcp_header->conv = KCP_CONV_FLAG;
        kcp_header->cmd = KCP_CMD_SYN;
        kcp_header->frg = 0;
        kcp_header->wnd = 0;
        kcp_header->syn_data.packet_ts = 0;
        kcp_header->syn_data.syn_ts = kcp_time_monotonic_us();
        kcp_header->syn_data.packet_sn = 0;
        kcp_header->syn_data.rand_sn = XXH32(&kcp_header->syn_data.syn_ts, sizeof(kcp_header->syn_data.syn_ts), 0);
        list_add_tail(&kcp_connection->kcp_proto_header_list, kcp_header);

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(kcp_header, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, sizeof(data));
        if (status <= 0) {
            evtimer_free(kcp_connection->syn_timer_event);
            kcp_connection->syn_timer_event = NULL;
            kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, status);
            return;
        }

        uint32_t timeout_ms = kcp_connection->receive_timeout;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_connection->syn_timer_event, &tv);
        return;
    }

    evtimer_free(kcp_connection->syn_timer_event);
    kcp_connection->syn_timer_event = NULL;
    kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, TIMED_OUT);
}

int32_t kcp_connect(struct KcpContext *kcp_ctx, const sockaddr_t *addr, uint32_t timeout_ms, on_kcp_connected_t cb)
{
    if (kcp_ctx == NULL || addr == NULL || cb == NULL || timeout_ms == 0) {
        return INVALID_PARAM;
    }

    if (kcp_ctx->sock == INVALID_SOCKET) {
        return SOCKET_CLOSED;
    }

    // 客户端只能有一个连接
    kcp_connection_t *kcp_connection = connection_first(&kcp_ctx->connection_set);
    if (kcp_connection != NULL) {
        if (kcp_connection->state == KCP_STATE_CONNECTED) {
            return NO_ERROR;
        } else if (kcp_connection->state == KCP_STATE_SYN_SENT) {
            return IN_PROGRESS;
        } else {
            return INVALID_STATE;
        }
    }

    struct timeval tv = {0, 1000}; // 1ms
    evtimer_add(kcp_ctx->write_timer_event, &tv);

    kcp_connection = (kcp_connection_t *)malloc(sizeof(kcp_connection_t));
    if (kcp_connection == NULL) {
        return NO_MEMORY;
    }
    kcp_connection_init(kcp_connection, addr, kcp_ctx);
    if (!connection_set_insert(&kcp_ctx->connection_set, kcp_connection)) {
        return UNKNOWN_ERROR;
    }

    kcp_proto_header_t *kcp_header = (kcp_proto_header_t *)malloc(sizeof(kcp_proto_header_t));
    list_init(&kcp_header->node_list);
    kcp_header->conv = KCP_CONV_FLAG;
    kcp_header->cmd = KCP_CMD_SYN;
    kcp_header->frg = 0;
    kcp_header->wnd = 0;
    kcp_header->syn_data.packet_ts = 0;
    kcp_header->syn_data.syn_ts = kcp_time_monotonic_us();
    kcp_header->syn_data.packet_sn = 0;
    kcp_header->syn_data.rand_sn = XXH32(&kcp_header->syn_data.syn_ts, sizeof(kcp_header->syn_data.syn_ts), 0);
    list_add_tail(&kcp_connection->kcp_proto_header_list, kcp_header);

    char buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_encode(kcp_header, buffer, KCP_HEADER_SIZE);
    struct iovec data[1];
    data[0].iov_base = buffer;
    data[0].iov_len = KCP_HEADER_SIZE;
    int32_t status = kcp_send_packet(kcp_connection, &data, sizeof(data));
    if (status <= 0) {
        return status;
    }

    kcp_connection->state = KCP_STATE_SYN_SENT;
    kcp_ctx->callback.on_connected = cb;
    kcp_ctx->callback.on_connect = NULL;
    kcp_connection->syn_timer_event = evtimer_new(kcp_ctx->event_loop, kcp_connect_timeout, kcp_connection);
    if (kcp_connection->syn_timer_event == NULL) {
        return NO_MEMORY;
    }

    // 添加超时事件, 读事件
    kcp_connection->receive_timeout = timeout_ms;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    evtimer_add(kcp_connection->syn_timer_event, &tv);
    event_add(kcp_ctx->read_event, NULL);
    return NO_ERROR;
}

void kcp_set_close_cb(struct KcpContext *kcp_ctx, on_kcp_closed_t cb)
{
    if (kcp_ctx) {
        kcp_ctx->callback.on_closed = cb;
    }
}

static void kcp_close_timeout(int fd, short ev, void *arg)
{
    kcp_connection_t *kcp_connection = (kcp_connection_t *)arg;
    if (kcp_connection->fin_retries--) {
        kcp_proto_header_t kcp_header;
        kcp_header.conv = kcp_connection->conv;
        kcp_header.cmd = KCP_CMD_FIN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.packet_data.ts = (uint32_t)time(NULL);
        kcp_header.packet_data.sn = kcp_header.packet_data.ts;
        kcp_connection->syn_fin_sn = kcp_header.packet_data.sn;
        kcp_header.packet_data.una = 0;
        kcp_header.packet_data.len = 0;
        kcp_header.packet_data.data = NULL;
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, 1);
        if (status != 1) {
            // Linux EAGAIN, Windows EWOULDBLOCK (WSAEWOULDBLOCK)
            int32_t code = get_last_errno();
            if (code == EAGAIN || code == EWOULDBLOCK) {
                kcp_add_write_event(kcp_connection);
            } else {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, WRITE_ERROR);
                return;
            }
        }

        uint32_t timeout_ms = kcp_connection->receive_timeout;
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_connection->fin_timer_event, &tv);
        return;
    }
}

void kcp_close(struct KcpConnection *kcp_connection, uint32_t timeout_ms)
{
    if (kcp_connection == NULL) {
        return;
    }

    timeout_ms = CLAMP(timeout_ms, 100, 10000);
    kcp_connection->receive_timeout = timeout_ms;

    kcp_context_t *kcp_ctx = kcp_connection->kcp_ctx;
    int32_t status = NO_ERROR;
    switch (kcp_connection->state) {
    case KCP_STATE_DISCONNECTED:
        // 一般情况下是被动断连
        break;
    case KCP_STATE_SYN_SENT:
    case KCP_STATE_SYN_RECEIVED:
        kcp_shutdown(kcp_connection);
        return;
    case KCP_STATE_CONNECTED:
    {
        kcp_proto_header_t kcp_header;
        kcp_header.conv = kcp_connection->conv;
        kcp_header.cmd = KCP_CMD_FIN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.packet_data.ts = (uint32_t)time(NULL);
        kcp_header.packet_data.sn = kcp_header.packet_data.ts;
        kcp_connection->syn_fin_sn = kcp_header.packet_data.sn;
        kcp_header.packet_data.una = 0;
        kcp_header.packet_data.len = 0;
        kcp_header.packet_data.data = NULL;
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);
        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        kcp_send_packet(kcp_connection, data, 1);
        kcp_connection->state = KCP_STATE_FIN_SENT;

        if (kcp_connection->fin_timer_event == NULL) {
            kcp_connection->fin_timer_event = evtimer_new(kcp_connection->kcp_ctx->event_loop, kcp_close_timeout, kcp_connection);
        }
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        evtimer_add(kcp_connection->fin_timer_event, &tv);
        return;
    }
    case KCP_STATE_FIN_SENT: // NOTE 已处于挥手流程, 直接返回
    case KCP_STATE_FIN_RECEIVED: // NOTE 被动断连, 已发送FIN, 等待对端ACK
        return;
    default:
        break;
    }

    if (kcp_ctx->callback.on_closed) {
        kcp_ctx->callback.on_closed(kcp_connection, status);
    }

    kcp_connection_destroy(kcp_connection);
}

void kcp_shutdown(struct KcpConnection *kcp_connection)
{
    // 发送RST
    kcp_context_t *kcp_ctx = kcp_connection->kcp_ctx;

    kcp_proto_header_t kcp_header;
    kcp_header.conv = kcp_connection->conv;
    kcp_header.cmd = KCP_CMD_RST;
    kcp_header.frg = 0;
    kcp_header.wnd = 0;
    kcp_header.packet_data.ts = (uint32_t)time(NULL);
    kcp_header.packet_data.sn = 0;
    kcp_header.packet_data.una = 0;
    kcp_header.packet_data.len = 0;
    kcp_header.packet_data.data = NULL;
    char buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

    struct iovec data[1];
    data[0].iov_base = buffer;
    data[0].iov_len = KCP_HEADER_SIZE;
    kcp_send_packet(kcp_connection, data, 1);
    kcp_connection->state = KCP_STATE_DISCONNECTED;

    if (kcp_ctx->callback.on_closed) {
        kcp_ctx->callback.on_closed(kcp_connection, NO_ERROR);
    }

    kcp_connection_destroy(kcp_connection);
}
