#include "kcpp.h"

#include <string.h>
#include <assert.h>

#include <event2/event.h>
#include <event2/buffer.h>

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

    kcp_proto_header_t kcp_header;
    if (NO_ERROR != kcp_proto_parse(&kcp_header, buffer, buffer_size)) {
        return;
    }

    kcp_connection_t *kcp_connection = connection_set_search(&kcp_ctx->connection_set, kcp_header.conv);
    if (kcp_connection == NULL) {
        if (kcp_header.cmd == KCP_CMD_SYN) {
            kcp_syn_node_t *syn_node = (kcp_syn_node_t *)malloc(sizeof(kcp_syn_node_t));
            if (syn_node == NULL) {
                kcp_ctx->callback.on_error(kcp_ctx, NO_MEMORY);
                return;
            }
            syn_node->conv = kcp_header.conv;
            memcpy(&syn_node->remote_host, addr, sizeof(sockaddr_t));
            syn_node->sn = kcp_header.sn;
            list_add_tail(&kcp_ctx->syn_queue, syn_node);
            kcp_ctx->callback.on_syn_received(kcp_ctx, addr);
        }
        return;
    }

    if (kcp_connection->read_cb != NULL) {
        kcp_connection->read_cb(kcp_connection, &kcp_header);
    }
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
            int32_t status = pos->write_cb(pos);
            if (status == NO_ERROR) {
                list_del_init(&pos->node_list);
            } else if (status == OP_TRY_AGAIN) { // 缓存区已满
                break;
            } else {
                if (kcp_ctx->callback.on_error) {
                    kcp_ctx->callback.on_error(kcp_ctx, status);
                }
                break;
            }
        }
    }

    if (list_empty(&kcp_ctx->conn_write_event_queue)) {
        event_del(kcp_ctx->write_event);
    }
}

struct KcpContext *kcp_create(struct event_base *base, on_kcp_error_t cb, void *user)
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
    ctx->backlog = 128;
    list_init(&ctx->syn_queue);
    connection_set_init(&ctx->connection_set);
    ctx->event_loop = base;
    ctx->read_event = event_new(base, -1, EV_READ | EV_PERSIST, kcp_read_cb, ctx);
    ctx->write_event = event_new(base, -1, EV_WRITE | EV_PERSIST, kcp_write_cb, ctx);
    ctx->read_buffer = (char *)malloc(ETHERNET_MTU);
    ctx->read_buffer_size = ETHERNET_MTU;
    ctx->user_data = user;
    return ctx;
}

void kcp_destroy(struct KcpContext *kcp_ctx)
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

int32_t kcp_configure(kcp_connection_t *kcp_connection, em_config_key_t flags, const kcp_config_t *config)
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
    {
        kcp_connection->receive_timeout = *(uint32_t *)data;
        break;
    }
    case IOCTL_MTU_PROBE_TIMEOUT:
    {
        kcp_connection->mtu_probe_ctx->timeout = *(uint32_t *)data;
        break;
    }
    case IOCTL_KEEPALIVE_TIMEOUT:
    {
        kcp_connection->keepalive_timeout = *(uint32_t *)data;
        break;
    }
    case IOCTL_KEEPALIVE_INTERVAL:
    {
        kcp_connection->keepalive_interval = *(uint32_t *)data;
        break;
    }
    case IOCTL_SYN_RETRIES:
    {
        kcp_connection->syn_retries = *(uint32_t *)data;
        break;
    }
    case IOCTL_FIN_RETRIES:
    {
        kcp_connection->fin_retries = *(uint32_t *)data;
        break;
    }
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

int32_t kcp_listen(struct KcpContext *kcp_ctx, int32_t backlog, on_kcp_syn_received_t cb)
{
    if (kcp_ctx == NULL || cb == NULL) {
        return INVALID_PARAM;
    }

    kcp_ctx->backlog = backlog;
    kcp_ctx->callback.on_syn_received = cb;

    event_add(kcp_ctx->read_event, NULL);
    return NO_ERROR;
}

void kcp_set_accept_cb(struct KcpContext *kcp_ctx, on_kcp_accepted_t cb)
{
    if (kcp_ctx != NULL) {
        kcp_ctx->callback.on_accepted = cb;
    }
}

static void kcp_accept_timeout(int fd, short ev, void *arg)
{
    kcp_connection_t *kcp_connection = (kcp_connection_t *)arg;
    kcp_context_t *kcp_ctx = kcp_connection->kcp_ctx;
    if (kcp_connection->syn_retries--) {
        kcp_proto_header_t kcp_header;
        kcp_header.conv = kcp_connection->conv;
        kcp_header.cmd = KCP_CMD_SYN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.ts = (uint32_t)time(NULL);
        kcp_header.sn = kcp_header.ts;
        kcp_connection->syn_fin_sn = kcp_header.sn;
        kcp_header.una = 0;
        kcp_header.len = 0;
        kcp_header.data = NULL;

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_connection, buffer, KCP_HEADER_SIZE);
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
        kcp_ctx->callback.on_accepted(kcp_ctx, kcp_connection, TIMED_OUT);
    }
}

int32_t kcp_accept(struct KcpContext *kcp_ctx, sockaddr_t *addr, uint32_t timeout_ms)
{
    if (kcp_ctx == NULL) {
        return INVALID_PARAM;
    }

    kcp_connection_t *kcp_connection = (kcp_connection_t *)malloc(sizeof(kcp_connection_t));
    if (kcp_connection == NULL) {
        return NO_MEMORY;
    }
    memset(kcp_connection, 0, sizeof(kcp_connection_t));

    if (list_empty(&kcp_ctx->syn_queue)) {
        free(kcp_connection);
        return NO_PENDING_CONNECTION;
    }

    int32_t status = NO_ERROR;
    kcp_syn_node_t *syn_packet = list_first_entry(&kcp_ctx->syn_queue, kcp_syn_node_t, node);
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

        kcp_proto_header_t kcp_header;
        kcp_header.conv = conv;
        kcp_header.cmd = KCP_CMD_SYN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.ts = time(NULL);
        kcp_header.sn = kcp_header.ts;
        kcp_header.una = 0;
        kcp_header.len = 0;
        kcp_header.data = NULL;
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

        list_del_init(&syn_packet->node);
        free(syn_packet);
        syn_packet = NULL;

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

    // 发送FIN
    kcp_proto_header_t kcp_header;
    kcp_header.conv = KCP_CONV_FLAG;
    kcp_header.cmd = KCP_CMD_FIN;
    kcp_header.frg = 0;
    kcp_header.wnd = 0;
    kcp_header.ts = time(NULL);
    kcp_header.sn = kcp_header.ts;
    kcp_header.una = 0;
    kcp_header.len = 0;
    kcp_header.data = NULL;
    char buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

    struct iovec data[1];
    data->iov_base = buffer;
    data->iov_len = KCP_HEADER_SIZE;
    kcp_send_packet(kcp_connection, &data, 1);
    kcp_connection_destroy(kcp_connection);

    // 删除SYN节点
    if (syn_packet) {
        list_del_init(&syn_packet->node);
        free(syn_packet);
    }
    return status;
}

static bool on_kcp_syn_received(struct KcpContext *kcp_ctx, const sockaddr_t *addr)
{
    if (kcp_ctx == NULL || addr == NULL) {
        return false;
    }

    kcp_connection_t *kcp_connection = connection_first(&kcp_ctx->connection_set);
    if (kcp_connection == NULL) {
        return false;
    }

    bool status = false;
    kcp_syn_node_t *syn_packet = list_first_entry(&kcp_ctx->syn_queue, kcp_syn_node_t, node);
    if (syn_packet->sn == kcp_connection->syn_fin_sn) {
        kcp_proto_header_t kcp_header;
        kcp_header.conv = syn_packet->conv;
        kcp_header.cmd = KCP_CMD_ACK;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.ts = time(NULL);
        kcp_header.sn = 0;
        kcp_header.una = 0;
        kcp_header.len = 0;
        kcp_header.data = NULL;

        char buffer[KCP_HEADER_SIZE] = { 0 };
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;

        if (kcp_send_packet(kcp_connection, &data, sizeof(data)) < 0) {
            // NOTE 此处不会触发 EAGAIN
            int32_t code = get_last_errno();
            KCP_LOGE("kcp send packet error. [%d, %s]", code, errno_string(code));
        } else {
            evtimer_del(kcp_connection->syn_timer_event);
            evtimer_free(kcp_connection->syn_timer_event);
            kcp_connection->syn_timer_event = NULL;
            kcp_connection->state = KCP_STATE_CONNECTED;
            status = true;
            kcp_connection->kcp_ctx->callback.on_connected(kcp_connection, NO_ERROR);
            kcp_connection->conv = syn_packet->conv;
        }
    }

    // 检验序号不通过时依靠超时处理异常场景, 客户端不接受SYN
    list_del(&syn_packet->node);
    free(syn_packet);
    return status;
}

static void kcp_connect_timeout(int fd, short ev, void *arg)
{
    kcp_connection_t *kcp_connection = (kcp_connection_t *)arg;
    if (kcp_connection->syn_retries--) {
        kcp_proto_header_t kcp_header;
        kcp_header.conv = KCP_CONV_FLAG;
        kcp_header.cmd = KCP_CMD_SYN;
        kcp_header.frg = 0;
        kcp_header.wnd = 0;
        kcp_header.ts = (uint32_t)time(NULL);
        kcp_header.sn = kcp_header.ts;
        kcp_connection->syn_fin_sn = kcp_header.sn;
        kcp_header.una = 0;
        kcp_header.len = 0;
        kcp_header.data = NULL;

        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_connection, buffer, KCP_HEADER_SIZE);
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

    kcp_connection_t *kcp_connection = (kcp_connection_t *)malloc(sizeof(kcp_connection_t));
    if (kcp_connection == NULL) {
        return NO_MEMORY;
    }
    kcp_connection_init(kcp_connection, addr, kcp_ctx);
    if (!connection_set_insert(&kcp_ctx->connection_set, kcp_connection)) {
        return UNKNOWN_ERROR;
    }

    kcp_proto_header_t kcp_header;
    kcp_header.conv = KCP_CONV_FLAG;
    kcp_header.cmd = KCP_CMD_SYN;
    kcp_header.frg = 0;
    kcp_header.wnd = 0;
    kcp_header.ts = (uint32_t)time(NULL);
    kcp_header.sn = kcp_header.ts;
    kcp_connection->syn_fin_sn = kcp_header.sn;
    kcp_header.una = 0;
    kcp_header.len = 0;
    kcp_header.data = NULL;

    char buffer[KCP_HEADER_SIZE] = {0};
    kcp_proto_header_encode(&kcp_connection, buffer, KCP_HEADER_SIZE);
    struct iovec data[1];
    data[0].iov_base = buffer;
    data[0].iov_len = KCP_HEADER_SIZE;
    int32_t status = kcp_send_packet(kcp_connection, &data, sizeof(data));
    if (status <= 0) {
        return status;
    }

    kcp_connection->state = KCP_STATE_SYN_SENT;
    kcp_ctx->callback.on_connected = cb;
    kcp_ctx->callback.on_syn_received = on_kcp_syn_received;
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
        kcp_header.ts = (uint32_t)time(NULL);
        kcp_header.sn = kcp_header.ts;
        kcp_connection->syn_fin_sn = kcp_header.sn;
        kcp_header.una = 0;
        kcp_header.len = 0;
        kcp_header.data = NULL;
        char buffer[KCP_HEADER_SIZE] = {0};
        kcp_proto_header_encode(&kcp_header, buffer, KCP_HEADER_SIZE);

        struct iovec data[1];
        data[0].iov_base = buffer;
        data[0].iov_len = KCP_HEADER_SIZE;
        int32_t status = kcp_send_packet(kcp_connection, &data, 1);
        if (status <= 0) {
            // Linux EAGAIN, Windows EWOULDBLOCK (WSAEWOULDBLOCK)
            if (get_last_errno() != EAGAIN || get_last_errno() != EWOULDBLOCK) {
                kcp_add_write_event(kcp_connection);
            } else {
                kcp_connection->kcp_ctx->callback.on_closed(kcp_connection, status);
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
        kcp_header.ts = (uint32_t)time(NULL);
        kcp_header.sn = kcp_header.ts;
        kcp_connection->syn_fin_sn = kcp_header.sn;
        kcp_header.una = 0;
        kcp_header.len = 0;
        kcp_header.data = NULL;
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
    case KCP_STATE_FIN_SENT:
        // 已处于挥手流程, 直接返回
        return;
    case KCP_STATE_FIN_RECEIVED:
        // TODO 被动断连, 收到后立即发送ACK
        break;
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
    kcp_header.ts = (uint32_t)time(NULL);
    kcp_header.sn = 0;
    kcp_header.una = 0;
    kcp_header.len = 0;
    kcp_header.data = NULL;
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
