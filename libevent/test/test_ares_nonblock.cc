/*************************************************************************
    > File Name: test_ares.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月01日 星期五 14时23分03秒
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <iostream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/event.h>
#include <ares.h>
#include <netdb.h>

void socket_state_callback(void *data, ares_socket_t sock, int readable, int writable)
{
    printf("----------socket = %d-----readable = %d-----writable = %d---\n", sock, readable, writable);

    // struct ares_event *ev = (struct ares_event *)data;

    // if (readable) {
    //     // 注册可读事件
    //     event_assign(&ev->read_event, sock, EV_READ | EV_PERSIST, (void (*)(int, short, void *))ares_process, ev->channel);
    //     event_add(&ev->read_event, NULL);
    // } else {
    //     // 取消可读事件
    //     event_del(&ev->read_event);
    // }

    // if (writable) {
    //     // 注册可写事件
    //     event_assign(&ev->write_event, sock, EV_WRITE | EV_PERSIST, (void (*)(int, short, void *))ares_process, ev->channel);
    //     event_add(&ev->write_event, NULL);
    // } else {
    //     // 取消可写事件
    //     event_del(&ev->write_event);
    // }
}

void dns_callback(void *arg, int status, int timeouts, struct hostent *host)
{
    if (status == ARES_SUCCESS) {
        std::cout << "Resolved IP: " << inet_ntoa(*(struct in_addr *)host->h_addr) << std::endl;
    } else {
        std::cerr << "DNS query failed: " << ares_strerror(status) << std::endl;
    }
}

void dns_callback(void *arg, int status, int timeouts, struct ares_addrinfo *res)
{
    if (status == ARES_SUCCESS) {
        printf("getaddrinfo(%s) callback, status: %d, timeouts:%d\n", res->name, status, timeouts);
        struct ares_addrinfo_node* iter = res->nodes;
        while (iter) {
            char addr[INET6_ADDRSTRLEN];
            if (iter->ai_family == AF_INET) {
                struct sockaddr_in* sa = (struct sockaddr_in*)iter->ai_addr;
                ares_inet_ntop(AF_INET, &(sa->sin_addr), addr, INET_ADDRSTRLEN);
            }
            else if (iter->ai_family == AF_INET6) {
                struct sockaddr_in6* sa = (struct sockaddr_in6*)iter->ai_addr;
                ares_inet_ntop(AF_INET6, &(sa->sin6_addr), addr, INET6_ADDRSTRLEN);
            }
            printf("\tResolved IP address: %s, ttl = %d\n", addr, iter->ai_ttl);
            iter = iter->ai_next;
        }

        // 多层嵌套CNAME时需要多个ares_addrinfo_cname节点
        // 如: my.4399.com -> my.4399api.net -> my.4399.com.lxdns.com -> 118.123.235.2
        struct ares_addrinfo_cname *cname_iter = res->cnames;
        while (cname_iter) {
            printf("\tttl: %d, alias = %s, name = %s\n", cname_iter->ttl, cname_iter->alias, cname_iter->name);
            cname_iter = cname_iter->next;
        }

        ares_freeaddrinfo(res);
    } else {
        printf("DNS resolution failed: %d, %s, %p\n", status, ares_strerror(status), res);
    }
}

void ares_event_callback(int fd, short events, void *arg) {
    ares_channel channel = (ares_channel)arg;
    ares_process_fd(channel, (events & EV_READ) ? fd : ARES_SOCKET_BAD, (events & EV_WRITE) ? fd : ARES_SOCKET_BAD);
}

int main() {
    ares_channel channel = nullptr;
    ares_options options;
    options.timeout = 1000;
    options.flags = ARES_FLAG_NORECURSE;
    options.sock_state_cb = socket_state_callback;
    // ARES_OPT_SOCK_STATE_CB

    int status = ares_library_init(ARES_LIB_INIT_ALL);
    if (status != ARES_SUCCESS) {
        fprintf(stderr, "Failed to initialize c-ares: %s\n", ares_strerror(status));
        return 0;
    }

    // ares_init(&channel); // deprecated
    status = ares_init_options(&channel, &options, ARES_OPT_TIMEOUT | ARES_OPT_SOCK_STATE_CB);
    if (status != ARES_SUCCESS) {
        fprintf(stderr, "Failed to create DNS channel: %s\n", ares_strerror(status));
        ares_library_cleanup();
        return 0;
    }

    // 设置 DNS 服务器
    ares_set_servers_csv(channel, "114.114.114.114");  // 多个地址中间用 , 隔开

    // 进行 DNS 查询
    struct ares_addrinfo_hints hint;
    memset(&hint, 0x00, sizeof(hint));
    hint.ai_family = AF_UNSPEC;
    ares_getaddrinfo(channel, "my.4399.com", NULL, &hint, dns_callback, NULL);
    // ares_getaddrinfo(channel, "my.4399.com.lxdns.com", NULL, &hint, dns_callback, NULL);
    // ares_getaddrinfo(channel, "imga4.4399.com", NULL, &hint, dns_callback, NULL);
    // ares_gethostbyname(channel, "www.baidu.com", AF_INET, dns_callback, NULL); // deprecated

    struct event_base *base = event_base_new();
    struct event* ev_array[ARES_GETSOCK_MAXNUM] = {nullptr};

    ares_socket_t socks[ARES_GETSOCK_MAXNUM] = {0};
    int bitmask = ares_getsock(channel, socks, ARES_GETSOCK_MAXNUM); // deprecated
    printf("mask = %X\n", bitmask);

    int socknum = 0;
    for (int i = 0; i < ARES_GETSOCK_MAXNUM; i++) 
    {
        int16_t flag = EV_PERSIST;
        if (ARES_GETSOCK_READABLE(bitmask, i)) {
            flag |= EV_READ;
        }

        if (ARES_GETSOCK_WRITABLE(bitmask, i)) {
            flag |= EV_WRITE;
        }

        printf("sock = %d, flag = %X\n", socks[i], flag);

        if (flag != EV_PERSIST) {
            // 创建 libevent 的事件
            ev_array[i] = event_new(base, socks[i], flag, ares_event_callback, channel);
            event_add(ev_array[i], NULL);
            socknum++;
        } else {
            break;
        }
    }

    // 运行 libevent 事件循环
    event_base_dispatch(base);

    // 清理
    for (auto it : ev_array) {
        if (it != nullptr) {
            event_free(it);
        }
    }

    ares_destroy(channel);
    event_base_free(base);

    return 0;
}
