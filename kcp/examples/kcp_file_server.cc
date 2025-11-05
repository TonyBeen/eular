/*************************************************************************
    > File Name: kcp_file_server.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年10月30日 星期四 17时36分53秒
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <utils/endian.hpp>
#include "xxhash.h"

#include "file_info.h"

struct event_base *g_event_base = NULL;

struct KcpFileContext {
    int32_t file_offset = 0;
    uint32_t file_size = 0;
    uint32_t file_hash = 0;
    std::map<int32_t, std::string>  file_info_map; // file offset <-> file content
    std::string file_name;
    std::ofstream file_stream;
    XXH32_state_t* xxhash_state;
};

std::string gen_random_string(size_t length)
{
    static const std::string charset =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "_";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, static_cast<int>(charset.size() - 1));

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result.push_back(charset[dist(gen)]);
    }

    return result;
}

void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    (void)kcp_ctx; // Unused parameter

    switch (code) {
    case MTU_REDUCTION:
        if (kcp_connection) {
            printf("KCP connection %p: MTU reduction\n", kcp_connection);
            kcp_close(kcp_connection);
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
    case TOO_MANY_RETRANS:
        if (kcp_connection) {
            printf("KCP connection %p: Too many retransmissions\n", kcp_connection);
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
    char *buffer_offset = buffer;
    int32_t bytes_read = kcp_recv(kcp_connection, buffer, size);
    KcpFileContext *file_ctx = (KcpFileContext *)kcp_connection_user_data(kcp_connection);
    if (bytes_read > 0) {
        printf("Received %d bytes\n", bytes_read);
        uint16_t file_transfer_type = be16toh(*(uint16_t *)buffer_offset);
        switch (file_transfer_type) {
        case kFileTransferTypeInfo: {
            file_info_t *file_info = decode_file_info(buffer_offset, bytes_read - sizeof(uint16_t));
            if (!file_info) {
                printf("Error decoding file info\n");
                break;
            }
            printf("Received file info: %.*s, %u, %u\n", file_info->file_name_size, file_info->file_name, file_info->file_size, file_info->file_hash);

            // 写入缓存中的其他文件内容
            auto it = file_ctx->file_info_map.begin();
            while (it != file_ctx->file_info_map.end() && it->first == file_ctx->file_offset) {
                XXH32_update(file_ctx->xxhash_state, it->second.data(), it->second.size());
                file_ctx->file_stream.write(it->second.data(), it->second.size());
                file_ctx->file_offset += it->second.size();
                it = file_ctx->file_info_map.erase(it);
            }

            // 文件已发送完毕
            if ((uint32_t)file_ctx->file_offset == file_info->file_size) {
                // 校验xxhash
                uint32_t hash = XXH32_digest(file_ctx->xxhash_state);
                if (hash != file_info->file_hash) {
                    printf("Hash check failed: %u != %u\n", hash, file_info->file_hash);
                    break;
                }
                // 关闭文件流
                file_ctx->file_stream.close();
                // 释放xxhash状态
                XXH32_freeState(file_ctx->xxhash_state);
                // 重命名文件
                std::string new_file_name = std::string(file_info->file_name, file_info->file_name_size);
                if (rename(file_ctx->file_name.c_str(), new_file_name.c_str()) != 0) {
                    printf("Rename file failed: %s\n", strerror(errno));
                }
            }
            delete file_ctx;
            kcp_ioctl(kcp_connection, IOCTL_USER_DATA, NULL);
            break;
        }
        case kFileTransferTypeContent: {
            file_content_t *file_content = decode_file_content(buffer_offset, bytes_read - sizeof(uint16_t));
            if (!file_content) {
                printf("Error decoding file content\n");
                break;
            }
            printf("Received file content: %d, %d\n", file_content->size, file_content->offset);
            if (file_content->offset == file_ctx->file_offset) {
                // 写入文件
                if (!file_ctx->file_stream.is_open()) {
                    // 随机生成名字
                    file_ctx->file_name = gen_random_string(16) + ".tmp";
                    // 初始化xxhash状态
                    file_ctx->xxhash_state = XXH32_createState();
                    XXH32_reset(file_ctx->xxhash_state, 0);
                    file_ctx->file_stream.open(file_ctx->file_name, std::ios::binary | std::ios::out);
                }
                // 更新xxhash状态
                XXH32_update(file_ctx->xxhash_state, file_content->content, file_content->size);
                file_ctx->file_stream.write((char *)file_content->content, file_content->size);
                file_ctx->file_offset += file_content->size;

                // 写入缓存中的其他文件内容
                auto it = file_ctx->file_info_map.begin();
                while (it != file_ctx->file_info_map.end() && it->first == file_ctx->file_offset) {
                    XXH32_update(file_ctx->xxhash_state, it->second.data(), it->second.size());
                    file_ctx->file_stream.write(it->second.data(), it->second.size());
                    file_ctx->file_offset += it->second.size();
                    it = file_ctx->file_info_map.erase(it);
                }
            } else {
                // 缓存文件内容
                file_ctx->file_info_map[file_content->offset] = std::string((char *)file_content->content, file_content->size);
            }
            break;
        }
        default:
            printf("Unknown file transfer type: %d\n", file_transfer_type);
            break;
        }
    } else if (bytes_read < 0) {
        fprintf(stderr, "Error reading from KCP connection: %d\n", bytes_read);
    }
    free(buffer);
}

void on_kcp_accepted(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    (void)kcp_ctx; // Unused parameter

    if (code == NO_ERROR) {
        printf("KCP connection accepted\n");
        kcp_set_read_event_cb(kcp_connection, on_kcp_read_event);
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
        KcpFileContext *file_ctx = new KcpFileContext();
        kcp_ioctl(kcp_connection, IOCTL_USER_DATA, file_ctx);
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
