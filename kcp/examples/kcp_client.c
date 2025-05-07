/*************************************************************************
    > File Name: kcp_client.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年04月28日 星期一 16时07分58秒
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <event2/event.h>

#include <kcpp.h>
#include <kcp_error.h>
#include <kcp_log.h>

#ifndef OS_LINUX
#error "This example requires a Linux environment."
#endif

#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

struct event_base*  g_ev_base = NULL;
struct event*       g_timer_event = NULL;

void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    if (kcp_connection) {
        printf("KCP error on connection %p: %d\n", kcp_connection, code);
    } else {
        printf("KCP error on context %p: %d\n", kcp_ctx, code);
    }

    fprintf(stderr, "Unhandled KCP error code: %d\n", code);
}

void on_kcp_closed(struct KcpConnection *kcp_connection, int32_t code)
{
    if (code == NO_ERROR) {
        printf("KCP connection closed gracefully\n");
    } else {
        printf("KCP connection closed with error code: %d\n", code);
    }
}

void on_kcp_read_event(struct KcpConnection *kcp_connection, int32_t size)
{
    char *buffer = (char *)malloc(size);
    int32_t bytes_read = kcp_recv(kcp_connection, buffer, size);
    if (bytes_read > 0) {
        printf("----> RX: %.*s\n", bytes_read, buffer);
    } else if (bytes_read < 0) {
        fprintf(stderr, "Error reading from KCP connection: %d\n", bytes_read);
        kcp_close(kcp_connection, 1000);
    }
}

void kcp_timer(int fd, short ev, void *user)
{
    struct KcpConnection *kcp_connection = (struct KcpConnection *)user;

    // 定时发送数据
    const char *message = "Hello, KCP!";
    int32_t status = kcp_send(kcp_connection, message, strlen(message));
    if (status != NO_ERROR) {
        fprintf(stderr, "Error sending data on KCP connection: %d\n", status);
        kcp_close(kcp_connection, 1000);
        return;
    }

    printf("TX: %s\n", message);

    struct timeval timer_interval = {1, 0}; // 1s
    evtimer_add(g_timer_event, &timer_interval);
}

void on_kcp_connected(struct KcpConnection *kcp_connection, int32_t code)
{
    if (code != NO_ERROR) {
        fprintf(stderr, "Failed to connect KCP connection: %d\n", code);
        return;
    }
    printf("KCP connection established: %p\n", kcp_connection);
    set_kcp_read_event_cb(kcp_connection, on_kcp_read_event);

    // const char *message = "Hello, KCP!";
    // int32_t status = kcp_send(kcp_connection, message, strlen(message));
    // if (status != NO_ERROR) {
    //     fprintf(stderr, "Error sending data on KCP connection: %d\n", status);
    //     kcp_close(kcp_connection, 1000);
    //     return;
    // }

    // printf("TX: %s\n", message);

    // 创建定时器, 定时发送
    g_timer_event = evtimer_new(g_ev_base, kcp_timer, kcp_connection);
    struct timeval timer_interval = {1, 0}; // 1s
    evtimer_add(g_timer_event, &timer_interval);
}

int main(int argc, char **argv)
{
    kcp_log_level(LOG_LEVEL_DEBUG);

    int32_t command = 0;
    const char *remote_host = "127.0.0.1";
    while ((command = getopt(argc, argv, "s:h")) != -1) {
        switch (command) {
            case 's':
                remote_host = optarg;
                break;
            case 'h':
                fprintf(stderr, "Usage: %s [-s command]\n", argv[0]);
                return 0;
            case '?':
                fprintf(stderr, "Unknown option: %d\n", optopt);
                return -1;
            default:
                fprintf(stderr, "Usage: %s [-s command] %d\n", argv[0], command);
                return -1;
        }
    }

    sockaddr_t remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin.sin_family = AF_INET;
    remote_addr.sin.sin_port = htons(54321);

    // 解析域名
    struct addrinfo hints, *result, *rp;
    int ret;

    // 设置 hints 结构体，指定查询条件
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;     // 允许 IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // 流式套接字（TCP）
    hints.ai_flags = AI_ALL;         // 返回所有可用地址

    // 调用 getaddrinfo 解析域名
    if ((ret = getaddrinfo(remote_host, NULL, &hints, &result)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }

    // 遍历所有返回的地址信息
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        char ipstr[INET6_ADDRSTRLEN];
        void *addr;

        // 根据地址类型获取 IP 字符串
        if (rp->ai_family == AF_INET) { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
            remote_addr.sin.sin_addr.s_addr = ipv4->sin_addr.s_addr;
            addr = &(ipv4->sin_addr);
        }

        // 将二进制地址转换为可读字符串
        inet_ntop(rp->ai_family, addr, ipstr, sizeof(ipstr));
        printf("IP: %s\n", ipstr);
    }

    g_ev_base = event_base_new();
    struct KcpContext *ctx = NULL;
    ctx = kcp_context_create(g_ev_base, on_kcp_error, NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create KCP context\n");
        return -1;
    }

    sockaddr_t local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin.sin_family = AF_INET;
    local_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin.sin_port = htons(65432);

    int32_t status = kcp_bind(ctx, &local_addr, NULL);
    if (NO_ERROR != status) {
        fprintf(stderr, "Failed to bind KCP context. %d\n", status);
        kcp_context_destroy(ctx);
        return -1;
    }

    if (NO_ERROR != kcp_connect(ctx, &remote_addr, 1000, on_kcp_connected)) {
        fprintf(stderr, "Failed to connect KCP context\n");
        kcp_context_destroy(ctx);
        return -1;
    }

    kcp_set_close_cb(ctx, on_kcp_closed);

    printf("KCP client started, connecting to %s:%d\n", remote_host, ntohs(remote_addr.sin.sin_port));
    if (event_base_dispatch(g_ev_base) < 0) {
        fprintf(stderr, "Failed to start event loop\n");
        kcp_context_destroy(ctx);
        return -1;
    }

    kcp_context_destroy(ctx);
    event_base_free(g_ev_base);
    return 0;
}
