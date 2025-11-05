/*************************************************************************
    > File Name: kcp_sendfile.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年10月30日 星期四 17时37分02秒
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <fstream>
#include <random>

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

#include <utils/endian.hpp>
#include "xxhash.h"

#include "file_info.h"

struct event_base*  g_ev_base = NULL;
struct event*       g_timer_event = NULL;

#define PACKET_COUNT    (5)

struct KcpFileContext {
    int32_t         file_offset = 0;
    uint32_t        file_size = 0;
    uint32_t        file_hash = 0;
    std::string     file_name;
    std::ifstream   file_stream;
    XXH32_state_t*  xxhash_state;
};

std::string g_file_name;

void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    if (kcp_connection) {
        printf("KCP error on connection %p: %d\n", kcp_connection, code);
    } else {
        printf("KCP error on context %p: %d\n", kcp_ctx, code);
    }
    kcp_close(kcp_connection);

    event_base_loopbreak(g_ev_base);
}

void on_kcp_closed(struct KcpConnection *kcp_connection, int32_t code)
{
    char address[SOCKADDR_STRING_LEN] = {0};
    const char *addr_str = kcp_connection_remote_address(kcp_connection, address, sizeof(address));
    if (code == NO_ERROR) {
        printf("KCP connection closed gracefully. %s\n", addr_str);
    } else {
        printf("KCP connection closed with error code: %d, %s\n", code, addr_str);
    }

    printf("mtu = %d\n", kcp_connection_get_mtu(kcp_connection));

    kcp_statistic_t stat;
    kcp_connection_get_statistic(kcp_connection, &stat);
    printf("Statistics: ping: %u, pong: %u, tx: %lu, rtx: %lu, rrate: %.3f, "
           "srtt: %d us, rttvar: %d us, rto: %d us\n",
           stat.ping_count, stat.pong_count, stat.tx_bytes, stat.rtx_bytes, stat.rtx_bytes / (double)stat.tx_bytes,
           stat.srtt, stat.rttvar, stat.rto);

    event_base_loopbreak(g_ev_base);
}

void on_kcp_read_event(struct KcpConnection *kcp_connection, int32_t size)
{
    (void)size;
    uint8_t type  = 0;
    int32_t bytes_read = kcp_recv(kcp_connection, &type, sizeof(type));
    if (bytes_read > 0) {
        printf("on_kcp_read_event: %d\n", type);
        if (type == kFileTransferTypeOk) {
            printf("File transfer confirmed\n");
        }
    } else if (bytes_read < 0) {
        fprintf(stderr, "Error reading from KCP connection: %d\n", bytes_read);
    }

    kcp_close(kcp_connection);
}

void on_kcp_write_event(struct KcpConnection *kcp_connection, int32_t size)
{
    printf("on_kcp_write_event: %d\n", size);
    if (size <= 0) {
        return;
    }

    int32_t mtu = kcp_connection_get_mtu(kcp_connection);
    size = std::min(KCP_PACKET_COUNT, size);
    int32_t packet_size = (mtu - KCP_HEADER_SIZE) * size;
    packet_size = std::min(packet_size, KCP_MAX_PACKET_SIZE) - sizeof(file_content_t);

    KcpFileContext *file_ctx = (KcpFileContext *)kcp_connection_user_data(kcp_connection);
    if (file_ctx == NULL) {
        return;
    }

    if (!file_ctx->file_stream.is_open()) {
        fprintf(stderr, "Error: file_ctx->file_stream is not open\n");
        return;
    }

    // 文件结尾
    if (file_ctx->file_stream.eof()) {
        file_info_t *file_info = (file_info_t *)malloc(sizeof(file_info_t) + file_ctx->file_name.size());
        file_info->type = kFileTransferTypeInfo;
        file_info->file_size = file_ctx->file_size;
        file_ctx->file_hash = XXH32_digest(file_ctx->xxhash_state);
        XXH32_freeState(file_ctx->xxhash_state);
        file_info->file_hash = file_ctx->file_hash;
        file_info->file_name_size = file_ctx->file_name.size();
        memcpy(file_info->file_name, file_ctx->file_name.c_str(), file_ctx->file_name.size());
        packet_size = encode_file_info(file_info);
        int32_t status = kcp_send(kcp_connection, file_info, packet_size);
        if (status != NO_ERROR) {
            file_ctx->file_stream.close();
            free(file_info);
            printf("Error sending file info on KCP connection: %d\n", status);
            kcp_close(kcp_connection);
            return;
        }
        printf("Successfully sent file info: %s, size = %u, hash = %u\n", file_ctx->file_name.c_str(), file_ctx->file_size, file_ctx->file_hash);

        file_ctx->file_stream.close();
        free(file_info);
        delete file_ctx;
        kcp_ioctl(kcp_connection, IOCTL_USER_DATA, NULL);
        return;
    }

    file_content_t *file_content = (file_content_t *)malloc(sizeof(file_content_t) + packet_size);
    file_ctx->file_stream.read((char *)file_content->content, packet_size);
    int32_t bytes_read = file_ctx->file_stream.gcount();
    if (bytes_read <= 0) {
        fprintf(stderr, "Error: file_ctx->file_stream read failed\n");
        kcp_close(kcp_connection);
        return;
    }

    // 计算hash
    XXH32_update(file_ctx->xxhash_state, file_content->content, bytes_read);
    file_content->type = kFileTransferTypeContent;
    file_content->offset = file_ctx->file_offset;
    file_content->size = bytes_read;
    file_ctx->file_offset += bytes_read;
    packet_size = encode_file_content(file_content);
    int32_t status = kcp_send(kcp_connection, file_content, packet_size);
    if (status != NO_ERROR) {
        printf("Error sending file content on KCP connection: %d\n", status);
        kcp_close(kcp_connection);
        return;
    }

    free(file_content);
}

void on_kcp_connected(struct KcpConnection *kcp_connection, int32_t code)
{
    if (code != NO_ERROR) {
        fprintf(stderr, "Failed to connect KCP connection: %d\n", code);
        event_base_loopbreak(g_ev_base);
        return;
    }
    printf("KCP connection established: %p\n", kcp_connection);
    kcp_set_read_event_cb(kcp_connection, on_kcp_read_event);
    kcp_set_write_event_cb(kcp_connection, on_kcp_write_event);

    struct KcpConfig config = KCP_CONFIG_FAST;
    kcp_configure(kcp_connection, CONFIG_KEY_ALL, &config);

    uint32_t timeout = 1000;
    kcp_ioctl(kcp_connection, IOCTL_RECEIVE_TIMEOUT, &timeout);
    timeout = 5000; // 5s
    kcp_ioctl(kcp_connection, IOCTL_MTU_PROBE_TIMEOUT, &timeout);
    timeout = 2000; // 2s
    kcp_ioctl(kcp_connection, IOCTL_KEEPALIVE_TIMEOUT, &timeout);
    timeout = 5000; // 5s
    kcp_ioctl(kcp_connection, IOCTL_KEEPALIVE_INTERVAL, &timeout);
    uint32_t retries = 3;
    kcp_ioctl(kcp_connection, IOCTL_KEEPALIVE_RETRIES, &retries);

    KcpFileContext* file_ctx = new KcpFileContext();
    file_ctx->file_name = std::move(g_file_name);
    file_ctx->file_stream.open(file_ctx->file_name, std::ios::binary);
    if (!file_ctx->file_stream.is_open()) {
        fprintf(stderr, "Failed to open file: %s\n", file_ctx->file_name.c_str());
        event_base_loopbreak(g_ev_base);
        return;
    }
    printf("Successfully opened the file: %s\n", file_ctx->file_name.c_str());
    file_ctx->file_size = (uint32_t)file_ctx->file_stream.seekg(0, std::ios::end).tellg();
    file_ctx->file_stream.seekg(0, std::ios::beg);
    file_ctx->xxhash_state = XXH32_createState();
    XXH32_reset(file_ctx->xxhash_state, 0);

    kcp_ioctl(kcp_connection, IOCTL_USER_DATA, file_ctx);
}

void print_help(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [-s command] [-n nic] [-f file_name]\n", prog_name);
}

int main(int argc, char **argv)
{
    printf("Libevent Version: %s\n", event_get_version());
    kcp_log_level(LOG_LEVEL_DEBUG);

    int32_t command = 0;
    const char *remote_host = "127.0.0.1";
    const char *nic = NULL;
    while ((command = getopt(argc, argv, "s:n:f:h")) != -1) {
        switch (command) {
            case 's':
                remote_host = optarg;
                break;
            case 'n':
                nic = optarg;
                break;
            case 'f':
                g_file_name = optarg;
                break;
            case 'h':
                print_help(argv[0]);
                return 0;
            case '?':
                print_help(argv[0]);
                return -1;
            default:
                print_help(argv[0]);
                return -1;
        }
    }

    if (g_file_name.empty()) {
        print_help(argv[0]);
        return -1;
    }
    // 检验文件是否存在
    if (!std::ifstream(g_file_name).good()) {
        fprintf(stderr, "File not found: %s\n", g_file_name.c_str());
        return -1;
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
    char ipstr[INET6_ADDRSTRLEN];
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        void *addr;

        // 根据地址类型获取 IP 字符串
        if (rp->ai_family == AF_INET) { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
            remote_addr.sin.sin_addr.s_addr = ipv4->sin_addr.s_addr;
            addr = &(ipv4->sin_addr);

            // 将二进制地址转换为可读字符串
            inet_ntop(rp->ai_family, addr, ipstr, sizeof(ipstr));
            remote_host = ipstr;
            printf("IP: %s\n", ipstr);
            break;
        }
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
    local_addr.sin.sin_port = htons(0);

    int32_t status = kcp_bind(ctx, &local_addr, nic);
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

    printf("KCP client exiting...\n");
    kcp_context_destroy(ctx);
    event_base_free(g_ev_base);
    return 0;
}
