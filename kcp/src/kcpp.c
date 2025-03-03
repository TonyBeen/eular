#include "kcpp.h"

#include <string.h>
#include <assert.h>

#include <event2/event.h>

#include "kcp_error.h"
#include "kcp_protocol.h"
#include "kcp_net_utils.h"

struct KcpContext *kcp_create(struct event_base *base, void *user)
{
    if (base == NULL) {
        return NULL;
    }

    struct KcpContext *ctx = (struct KcpContext *)malloc(sizeof(struct KcpContext));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->sock = INVALID_SOCKET;
    memset(&ctx->local_addr, 0, sizeof(sockaddr_t));
    ctx->callback = (kcp_function_callback_t) {
        .on_accepted = NULL,
        .on_connected = NULL,
        .on_closed = NULL
    };
    connection_set_init(&ctx->connection_set);
    ctx->event_loop = base;
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