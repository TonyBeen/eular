#include "kcpp.h"

#include <string.h>
#include <assert.h>

#include <event2/event.h>
#include <event2/buffer.h>

#include "kcp_error.h"
#include "kcp_endian.h"
#include "kcp_protocol.h"
#include "kcp_net_utils.h"
#include "connection_set.h"
#include "kcp_mtu.h"
#include "kcp_inc.h"

static void kcp_parse_packet(struct KcpContext *kcp_ctx, const char *buffer, size_t buffer_size, const sockaddr_t *addr)
{
    if (buffer_size < KCP_HEADER_SIZE) {
        return;
    }

    kcp_proto_header_t kcp_header;
    if (NO_ERROR != kcp_proto_parse(&kcp_header, buffer, buffer_size)) {
        return;
    }

    connection_set_node_t *kcp_conn_node = connection_set_search(&kcp_ctx->connection_set, kcp_header.conv);
    if (kcp_conn_node == NULL) {
        // TODO 处理 conv == KCP_CONV_FLAG 情况

        return;
    }
    kcp_connection_t *kcp_connection = kcp_conn_node->sock;
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

        struct iovec iov;
        iov.iov_base = kcp_ctx->read_buffer;
        iov.iov_len = kcp_ctx->read_buffer_size;

        struct msghdr msg;
        msg.msg_name = &remote_addr;
        msg.msg_namelen = addr_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        msg.msg_flags = 0;

        ssize_t nreads = recvmsg(kcp_ctx->sock, &msg, MSG_ERRQUEUE | MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nreads < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                // TODO 处理错误
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
            if (err->ee_type == ICMP_DEST_UNREACH && err->ee_code == ICMP_FRAG_NEEDED) {
                kcp_process_icmp_fragmentation(kcp_ctx, kcp_ctx->read_buffer, nreads, &remote_addr, err->ee_info);
            } else {
                kcp_process_icmp_error(kcp_ctx, kcp_ctx->read_buffer, nreads, &remote_addr);
            }
        }
    }

    while (true) {
        sockaddr_t remote_addr;
        socklen_t addr_len = sizeof(sockaddr_t);
        struct msghdr msg;
        struct iovec iov;
        struct cmsghdr *cmsg = NULL;
        char cmsgbuf[1024] = {0};
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

}

struct KcpContext *kcp_create(struct event_base *base, void *user)
{
    if (base == NULL) {
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
        .on_closed = NULL
    };
    ctx->backlog = 128;
    list_init(&ctx->syn_queue);
    connection_set_init(&ctx->connection_set);
    ctx->event_loop = base;
    ctx->read_event = event_new(base, -1, EV_READ | EV_PERSIST, kcp_read_cb, ctx);
    ctx->write_event = NULL;
    ctx->read_buffer = (char *)malloc(LOCALHOST_MTU);
    ctx->read_buffer_size = LOCALHOST_MTU;
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

int32_t kcp_configure(struct KcpConnection *kcp_connection, config_key_t flags, kcp_config_t *config)
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

int32_t kcp_accept(struct KcpContext *kcp_ctx, sockaddr_t *addr)
{
    if (kcp_ctx == NULL) {
        return INVALID_PARAM;
    }

    struct KcpConnection *kcp_connection = (struct KcpConnection *)malloc(sizeof(struct KcpConnection));
    if (kcp_connection == NULL) {
        return NO_MEMORY;
    }

    if (list_empty(&kcp_ctx->syn_queue)) {
        return NO_PENDING_CONNECTION;
    }

    kcp_syn_node_t* syn_packet = list_first_entry(&kcp_ctx->syn_queue, kcp_syn_node_t, node);
    if (syn_packet == NULL) {
        return NO_PENDING_CONNECTION;
    }
    // TODO 发送SYN给对端并等待对端ACK响应并设置超时时间

    return NULL;
}

// TODO 实现 syn_received 回调函数, 用于connect连接回调
static bool on_kcp_syn_received(struct KcpContext *kcp_ctx, const sockaddr_t *addr)
{
    if (kcp_ctx == NULL || addr == NULL) {
        return false;
    }

    // struct KcpConnection *kcp_connection = kcp_ctx->;
    // if (kcp_connection == NULL) {
    //     return false;
    // }
    // kcp_connection->state = KCP_STATE_SYN_RECEIVED;


    // TODO 发送SYN_ACK给对端

    return true;
}

static void kcp_connect_timeout(int fd, short ev, void *arg)
{
    kcp_connection_t *kcp_connection = (kcp_connection_t *)arg;
    assert(kcp_connection != NULL);
    if (kcp_connection == NULL) {
        return;
    }

    // TODO 超时处理
    // 1. 重发SYN
    // 2. 超时关闭连接
    // 3. 重试次数超过限制
    // 4. 重试间隔时间
    // 5. 重试次数

    evtimer_del(kcp_connection->syn_timer_event);
    evtimer_free(kcp_connection->syn_timer_event);
    kcp_connection->syn_timer_event = NULL;
    free(kcp_connection);
    kcp_connection = NULL;
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
    kcp_connection->state = KCP_STATE_SYN_SENT;
    kcp_ctx->callback.on_connected = cb;
    kcp_ctx->callback.on_syn_received = on_kcp_syn_received;

    kcp_connection->syn_timer_event = evtimer_new(kcp_ctx->event_loop, kcp_connect_timeout, kcp_connection);
    if (kcp_connection->syn_timer_event == NULL) {
        return NO_MEMORY;
    }
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    evtimer_add(kcp_connection->syn_timer_event, &tv);
    return NO_ERROR;
}