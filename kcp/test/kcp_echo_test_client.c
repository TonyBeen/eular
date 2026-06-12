#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <event2/event.h>
#include <getopt.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kcp_error.h>
#include <kcp_log.h>
#include <kcpp.h>

#ifndef OS_LINUX
#error "This test client requires a Linux environment."
#endif

typedef struct EchoClientConfig {
    const char *server_host;
    int local_port;
    int server_port;
    int bad_candidate_port;
    int message_count;
    int duration_ms;
    int message_len;
    int timeout_ms;
    const char *nic;
} echo_client_config_t;

typedef struct EchoClientState {
    struct event_base *base;
    struct event *timeout_event;
    struct KcpContext *ctx;
    struct KcpConnection *conn;
    echo_client_config_t cfg;
    int tx_count;
    int rx_count;
    int closed_code;
    int exit_code;
    uint64_t start_ms;
    uint64_t end_ms;
    char *tx_buffer;
    char *rx_buffer;
} echo_client_state_t;

static echo_client_state_t g_state;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void stop_with_error(int exit_code, const char *message, int detail)
{
    if (message != NULL) {
        if (detail != 0) {
            fprintf(stderr, "%s: %d\n", message, detail);
        } else {
            fprintf(stderr, "%s\n", message);
        }
    }

    g_state.exit_code = exit_code;
    if (g_state.conn != NULL) {
        kcp_close(g_state.conn);
    }
    if (g_state.base != NULL) {
        event_base_loopbreak(g_state.base);
    }
}

static void fill_payload(char *buffer, size_t len, int seq)
{
    int prefix = snprintf(buffer, len, "seq=%d|", seq);
    if (prefix < 0) {
        prefix = 0;
    }
    if ((size_t)prefix > len) {
        prefix = (int)len;
    }
    for (size_t i = (size_t)prefix; i < len; ++i) {
        buffer[i] = (char)('a' + ((seq + (int)i) % 26));
    }
}

static int send_next_message(void)
{
    if (g_state.cfg.message_count > 0 && g_state.tx_count >= g_state.cfg.message_count) {
        return NO_ERROR;
    }

    if (g_state.cfg.duration_ms > 0 && monotonic_ms() >= g_state.end_ms) {
        return NO_ERROR;
    }

    fill_payload(g_state.tx_buffer, (size_t)g_state.cfg.message_len, g_state.tx_count);
    int32_t status = kcp_send(g_state.conn, g_state.tx_buffer, (size_t)g_state.cfg.message_len);
    if (status != NO_ERROR) {
        fprintf(stderr, "kcp_send failed: %d\n", status);
        return status;
    }

    printf("TX seq=%d len=%d\n", g_state.tx_count, g_state.cfg.message_len);
    g_state.tx_count++;
    return NO_ERROR;
}

static void on_timeout(evutil_socket_t fd, short event, void *arg)
{
    (void)fd;
    (void)event;
    (void)arg;
    stop_with_error(124, "echo test timed out", 0);
}

static void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    (void)kcp_ctx;
    fprintf(stderr, "KCP error code=%d conn=%p\n", code, (void *)kcp_connection);
    stop_with_error(2, "KCP error", code);
}

static void on_kcp_closed(struct KcpConnection *kcp_connection, int32_t code)
{
    (void)kcp_connection;
    g_state.closed_code = code;
    if (code == NO_ERROR && g_state.exit_code == 0 && g_state.rx_count == g_state.tx_count) {
        printf("RESULT ok tx=%d rx=%d len=%d duration_ms=%llu\n",
            g_state.tx_count,
            g_state.rx_count,
            g_state.cfg.message_len,
            (unsigned long long)(monotonic_ms() - g_state.start_ms));
        event_base_loopbreak(g_state.base);
        return;
    }

    if (g_state.exit_code == 0) {
        fprintf(stderr, "unexpected close code=%d tx=%d rx=%d expected_count=%d duration_ms=%d\n",
            code, g_state.tx_count, g_state.rx_count, g_state.cfg.message_count, g_state.cfg.duration_ms);
        g_state.exit_code = 3;
    }
    event_base_loopbreak(g_state.base);
}

static void on_kcp_read_event(struct KcpConnection *kcp_connection, int32_t size)
{
    if (size != g_state.cfg.message_len) {
        stop_with_error(4, "unexpected echo size", size);
        return;
    }

    int32_t bytes_read = kcp_recv(kcp_connection, g_state.rx_buffer, (size_t)size);
    if (bytes_read != g_state.cfg.message_len) {
        stop_with_error(5, "kcp_recv failed", bytes_read);
        return;
    }

    fill_payload(g_state.tx_buffer, (size_t)g_state.cfg.message_len, g_state.rx_count);
    if (memcmp(g_state.tx_buffer, g_state.rx_buffer, (size_t)g_state.cfg.message_len) != 0) {
        stop_with_error(6, "echo payload mismatch", g_state.rx_count);
        return;
    }

    printf("RX seq=%d len=%d\n", g_state.rx_count, bytes_read);
    g_state.rx_count++;

    if ((g_state.cfg.message_count > 0 && g_state.rx_count >= g_state.cfg.message_count) ||
        (g_state.cfg.duration_ms > 0 && monotonic_ms() >= g_state.end_ms)) {
        kcp_close(kcp_connection);
        return;
    }

    if (send_next_message() != NO_ERROR) {
        stop_with_error(7, "failed to send next message", 0);
    }
}

static void on_kcp_connected(struct KcpConnection *kcp_connection, int32_t code)
{
    if (code != NO_ERROR) {
        stop_with_error(8, "kcp_connect failed", code);
        return;
    }

    g_state.conn = kcp_connection;
    kcp_set_read_event_cb(kcp_connection, on_kcp_read_event);
    g_state.start_ms = monotonic_ms();
    if (g_state.cfg.duration_ms > 0) {
        g_state.end_ms = g_state.start_ms + (uint64_t)g_state.cfg.duration_ms;
    }

    kcp_config_t config = KCP_CONFIG_FAST;
    kcp_configure(kcp_connection, CONFIG_KEY_ALL, &config);

    uint32_t timeout = (uint32_t)g_state.cfg.timeout_ms;
    kcp_ioctl(kcp_connection, IOCTL_RECEIVE_TIMEOUT, &timeout);

    if (send_next_message() != NO_ERROR) {
        stop_with_error(9, "initial send failed", 0);
    }
}

static int resolve_ipv4(const char *host, int port, sockaddr_t *addr)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int ret = getaddrinfo(host, NULL, &hints, &result);
    if (ret != 0) {
        return -1;
    }

    struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
    memset(addr, 0, sizeof(*addr));
    addr->sin.sin_family = AF_INET;
    addr->sin.sin_addr = ipv4->sin_addr;
    addr->sin.sin_port = htons((uint16_t)port);
    freeaddrinfo(result);
    return 0;
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [-s host] [-p server_port] [-l local_port] [-b bad_candidate_port] [-c count] [-d duration_ms] [-m message_len] [-t timeout_ms] [-n nic]\n",
        argv0);
}

int main(int argc, char **argv)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg.server_host = "127.0.0.1";
    g_state.cfg.local_port = 0;
    g_state.cfg.server_port = 54321;
    g_state.cfg.bad_candidate_port = 0;
    g_state.cfg.message_count = 10;
    g_state.cfg.duration_ms = 0;
    g_state.cfg.message_len = 64;
    g_state.cfg.timeout_ms = 10000;
    g_state.cfg.nic = NULL;
    g_state.exit_code = 0;
    g_state.closed_code = UNKNOWN_ERROR;

    int opt = 0;
    while ((opt = getopt(argc, argv, "s:p:l:b:c:d:m:t:n:h")) != -1) {
        switch (opt) {
        case 's':
            g_state.cfg.server_host = optarg;
            break;
        case 'p':
            g_state.cfg.server_port = atoi(optarg);
            break;
        case 'l':
            g_state.cfg.local_port = atoi(optarg);
            break;
        case 'b':
            g_state.cfg.bad_candidate_port = atoi(optarg);
            break;
        case 'c':
            g_state.cfg.message_count = atoi(optarg);
            break;
        case 'd':
            g_state.cfg.duration_ms = atoi(optarg);
            break;
        case 'm':
            g_state.cfg.message_len = atoi(optarg);
            break;
        case 't':
            g_state.cfg.timeout_ms = atoi(optarg);
            break;
        case 'n':
            g_state.cfg.nic = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (g_state.cfg.server_port <= 0 || g_state.cfg.server_port > 65535 ||
        g_state.cfg.local_port < 0 || g_state.cfg.local_port > 65535 ||
        g_state.cfg.bad_candidate_port < 0 || g_state.cfg.bad_candidate_port > 65535 ||
        g_state.cfg.message_count < 0 || g_state.cfg.duration_ms < 0 ||
        (g_state.cfg.message_count == 0 && g_state.cfg.duration_ms == 0) ||
        g_state.cfg.timeout_ms <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    g_state.tx_buffer = (char *)malloc((size_t)g_state.cfg.message_len);
    g_state.rx_buffer = (char *)malloc((size_t)g_state.cfg.message_len);
    if (g_state.tx_buffer == NULL || g_state.rx_buffer == NULL) {
        fprintf(stderr, "failed to allocate test buffers\n");
        return 1;
    }

    kcp_log_level(LOG_LEVEL_WARN);
    g_state.base = event_base_new();
    if (g_state.base == NULL) {
        fprintf(stderr, "event_base_new failed\n");
        g_state.exit_code = 1;
        goto cleanup;
    }

    g_state.timeout_event = evtimer_new(g_state.base, on_timeout, NULL);
    if (g_state.timeout_event == NULL) {
        fprintf(stderr, "evtimer_new failed\n");
        g_state.exit_code = 1;
        goto cleanup;
    }

    struct timeval tv = {
        g_state.cfg.timeout_ms / 1000,
        (g_state.cfg.timeout_ms % 1000) * 1000
    };
    evtimer_add(g_state.timeout_event, &tv);

    g_state.ctx = kcp_context_create(g_state.base, on_kcp_error, NULL);
    if (g_state.ctx == NULL) {
        fprintf(stderr, "kcp_context_create failed\n");
        g_state.exit_code = 1;
        goto cleanup;
    }

    sockaddr_t local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin.sin_family = AF_INET;
    local_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin.sin_port = htons((uint16_t)g_state.cfg.local_port);
    if (kcp_bind(g_state.ctx, &local_addr, g_state.cfg.nic) != NO_ERROR) {
        fprintf(stderr, "kcp_bind failed\n");
        g_state.exit_code = 1;
        goto cleanup;
    }

    sockaddr_t remote_addr;
    if (resolve_ipv4(g_state.cfg.server_host, g_state.cfg.server_port, &remote_addr) != 0) {
        fprintf(stderr, "resolve_ipv4 failed for %s\n", g_state.cfg.server_host);
        g_state.exit_code = 1;
        goto cleanup;
    }

    kcp_set_close_cb(g_state.ctx, on_kcp_closed);
    int32_t connect_status = NO_ERROR;
    if (g_state.cfg.bad_candidate_port > 0) {
        kcp_p2p_candidate_t candidates[2];
        memset(candidates, 0, sizeof(candidates));
        candidates[0].addr = remote_addr;
        candidates[0].addr.sin.sin_port = htons((uint16_t)g_state.cfg.bad_candidate_port);
        candidates[0].type = KCP_P2P_CANDIDATE_UNKNOWN;
        candidates[0].priority = 1;
        candidates[1].addr = remote_addr;
        candidates[1].type = KCP_P2P_CANDIDATE_UNKNOWN;
        candidates[1].priority = 2;
        connect_status = kcp_connect_candidates(
            g_state.ctx, candidates, 2, (uint32_t)g_state.cfg.timeout_ms, on_kcp_connected);
    } else {
        connect_status = kcp_connect(g_state.ctx, &remote_addr, (uint32_t)g_state.cfg.timeout_ms, on_kcp_connected);
    }
    if (connect_status != NO_ERROR) {
        fprintf(stderr, "kcp_connect call failed\n");
        g_state.exit_code = 1;
        goto cleanup;
    }

    if (event_base_dispatch(g_state.base) < 0) {
        fprintf(stderr, "event_base_dispatch failed\n");
        g_state.exit_code = 1;
    }

cleanup:
    if (g_state.ctx != NULL) {
        kcp_context_destroy(g_state.ctx);
    }
    if (g_state.timeout_event != NULL) {
        event_free(g_state.timeout_event);
    }
    if (g_state.base != NULL) {
        event_base_free(g_state.base);
    }
    free(g_state.tx_buffer);
    free(g_state.rx_buffer);
    return g_state.exit_code;
}
