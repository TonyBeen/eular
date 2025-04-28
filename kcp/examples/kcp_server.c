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

void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{

}

bool on_kcp_connect(struct KcpContext *kcp_ctx, const sockaddr_t *addr)
{
    printf("New KCP connection from %s:%d\n", inet_ntoa(addr->sin.sin_addr), ntohs(addr->sin.sin_port));
    return NO_ERROR == kcp_accept(kcp_ctx, 1000); // Accept the connection
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
    char buffer[1024];
    int32_t bytes_read = kcp_read(kcp_connection, buffer, sizeof(buffer));
    if (bytes_read > 0) {
        printf("Received %d bytes: %.*s\n", bytes_read, bytes_read, buffer);
        // Echo back the data
        kcp_write(kcp_connection, buffer, bytes_read);
    } else if (bytes_read < 0) {
        fprintf(stderr, "Error reading from KCP connection: %d\n", bytes_read);
    }
}

void on_kcp_accepted(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    if (code == NO_ERROR) {
        printf("KCP connection accepted\n");
        set_kcp_read_event_cb(kcp_connection, on_kcp_read_event);
    } else {
        fprintf(stderr, "Failed to accept KCP connection: %d\n", code);
    }
}

int main(int argc, char **argv)
{
    struct event_base *base = event_base_new();
    struct KcpContext *ctx = NULL;
    ctx = kcp_context_create(base, on_kcp_error, NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create KCP context\n");
        return -1;
    }

    sockaddr_t local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin.sin_family = AF_INET;
    local_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin.sin_port = htons(54321);

    int32_t statuc = kcp_bind(ctx, &local_addr, "eno1");
    if (statuc != NO_ERROR) {
        fprintf(stderr, "Failed to bind KCP context: %d\n", statuc);
        kcp_context_destroy(ctx);
        return -1;
    }

    statuc = kcp_listen(ctx, on_kcp_connect);
    if (statuc != NO_ERROR) {
        fprintf(stderr, "Failed to listen on KCP context: %d\n", statuc);
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
