/*************************************************************************
    > File Name: kcp_server.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年04月28日 星期一 16时07分52秒
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct event_base *g_event_base = NULL;

void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    switch (code) {
    case MTU_REDUCTION:
        if (kcp_connection) {
            printf("KCP connection %p: MTU reduction\n", kcp_connection);
            kcp_close(kcp_connection, 1000);
        }
        break;
    case UDP_UNREACH:
    case ICMP_ERROR:
        if (kcp_connection) {
            printf("KCP connection %p: UDP unreachable or ICMP error\n", kcp_connection);
            kcp_shutdown(kcp_connection);
        }
        break;
    case KEEPALIVE_ERROR:
        if (kcp_connection) {
            printf("KCP connection %p: Keepalive error\n", kcp_connection);
            kcp_shutdown(kcp_connection);
        }
        break;
    case READ_ERROR: // system error
    case WRITE_ERROR:
    case NO_MEMORY:
        event_base_loopbreak(g_event_base);
        break;
    default:
        break;
    }
}

bool on_kcp_connect(struct KcpContext *kcp_ctx, const sockaddr_t *addr)
{
    int32_t status = kcp_accept(kcp_ctx, 1000);
    printf("New KCP connection from %s:%d, status = %d\n", inet_ntoa(addr->sin.sin_addr), ntohs(addr->sin.sin_port), status);
    return NO_ERROR == status; // Accept the connection
}

void on_kcp_closed(struct KcpConnection *kcp_connection, int32_t code)
{
    char address[SOCKADDR_STRING_LEN] = {0};
    const char *addr_str = kcp_connection_remote_address(kcp_connection, address, sizeof(address));
    if (code == NO_ERROR) {
        printf("KCP connection closed gracefully. %s\n", addr_str);
    } else {
        printf("KCP connection closed with error code: %d\n", code);
    }
}

void on_kcp_read_event(struct KcpConnection *kcp_connection, int32_t size)
{
    char *buffer = (char *)malloc(size);
    int32_t bytes_read = kcp_recv(kcp_connection, buffer, size);
    if (bytes_read > 0) {
        printf("Received %d bytes: %.*s\n", bytes_read, bytes_read, buffer);
        // Echo back the data
        kcp_send(kcp_connection, buffer, bytes_read);
    } else if (bytes_read < 0) {
        fprintf(stderr, "Error reading from KCP connection: %d\n", bytes_read);
    }
}

void on_kcp_accepted(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    if (code == NO_ERROR) {
        printf("KCP connection accepted\n");
        set_kcp_read_event_cb(kcp_connection, on_kcp_read_event);
        struct KcpConfig config = KCP_CONFIG_FAST;
        kcp_configure(kcp_connection, CONFIG_KEY_ALL, &config);

        uint32_t timeout = 1000;
        kcp_ioctl(kcp_connection, IOCTL_RECEIVE_TIMEOUT, &timeout);
        timeout = 5000; // 5s
        kcp_ioctl(kcp_connection, IOCTL_MTU_PROBE_TIMEOUT, &timeout);
        timeout = 2000; // 2s
        kcp_ioctl(kcp_connection, IOCTL_KEEPALIVE_TIMEOUT, &timeout);
        timeout = 10000; // 10s
        kcp_ioctl(kcp_connection, IOCTL_KEEPALIVE_INTERVAL, &timeout);
        uint32_t retries = 3;
        kcp_ioctl(kcp_connection, IOCTL_KEEPALIVE_RETRIES, &retries);
    } else {
        fprintf(stderr, "Failed to accept KCP connection: %d\n", code);
    }
}

int main(int argc, char **argv)
{
    kcp_log_level(LOG_LEVEL_DEBUG);

    int32_t command = 0;
    const char *nic = NULL;
    while ((command = getopt(argc, argv, "n:h")) != -1) {
        switch (command) {
            case 'n':
                nic = optarg;
                break;
            case 'h':
                fprintf(stderr, "Usage: %s [-n nic]\n", argv[0]);
                return 0;
            case '?':
                fprintf(stderr, "Unknown option: %d\n", optopt);
                return -1;
            default:
                fprintf(stderr, "Usage: %s [-n nic] %d\n", argv[0], command);
                return -1;
        }
    }


    g_event_base = event_base_new();
    struct event_base *base = g_event_base;
    struct KcpContext *ctx = NULL;
    ctx = kcp_context_create(base, on_kcp_error, NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create KCP context\n");
        return -1;
    }

    sockaddr_t local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin.sin_family = AF_INET;
    local_addr.sin.sin_addr.s_addr = inet_addr("0.0.0.0");
    local_addr.sin.sin_port = htons(54321);

    int32_t statuc = kcp_bind(ctx, &local_addr, nic);
    if (statuc != NO_ERROR) {
        fprintf(stderr, "Failed to bind KCP context: %d\n", statuc);
        kcp_context_destroy(ctx);
        return -1;
    }

    statuc = kcp_listen(ctx, on_kcp_connect);
    if (statuc != NO_ERROR) {
        perror("Failed to bind KCP context");
        kcp_context_destroy(ctx);
        return -1;
    }

    kcp_set_accept_cb(ctx, on_kcp_accepted);
    kcp_set_close_cb(ctx, on_kcp_closed);

    printf("KCP server is running on %s:%d\n", inet_ntoa(local_addr.sin.sin_addr), ntohs(local_addr.sin.sin_port));
    // Start the event loop
    if (event_base_dispatch(base) < 0) {
        fprintf(stderr, "Failed to start event loop\n");
        kcp_context_destroy(ctx);
        return -1;
    }

    // Cleanup
    kcp_context_destroy(ctx);
    event_base_free(base);
    printf("KCP server stopped\n");
    return 0;
}
