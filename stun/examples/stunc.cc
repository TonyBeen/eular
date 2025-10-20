/*************************************************************************
    > File Name: stunc.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月19日 星期四 16时27分01秒
 ************************************************************************/

#include <stdio.h>

#include <utils/CLI11.hpp>

#include <stun.h>
#include <stun_types.h>
#include <socket_address.h>

#include <event2/event.h>
#include <event2/dns.h>

#ifdef __linux__
#include <unistd.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#endif

#define STUNC_LOGD(...)             \
    do {                            \
        if (g_config.verbose) {     \
            printf(__VA_ARGS__);    \
            printf("\n");           \
        }                           \
    } while (0)

/**
 * Test I：仅发送Binding Request
 * Test II：发送带有修改IP和Port标志的CHANGE-REQUEST属性的请求
 * Test III：发送仅修改Port标志的CHANGE-REQUEST请求
 */

struct NetworkInterface {
    std::string name;
    std::string ip_address;
    bool is_up;
    bool can_send_data;

    NetworkInterface(const std::string& n, const std::string& ip, bool up)
        : name(n), ip_address(ip), is_up(up), can_send_data(false) {}
};

struct StunClientConfig {
    // config
    std::string     interface;
    std::string     server_host;
    std::string     server_ip;
    uint16_t        server_port;
    std::string     username;
    std::string     password;
    bool            use_ipv6;
    bool            verbose;
    int32_t         timeout;
    int32_t         max_retries;

    bool            open_public;
    int32_t         nat_type;
    int32_t         state;
    int32_t         sock;
    int32_t         retries;

    std::vector<NetworkInterface>   interfaces;
    eular::stun::StunMsgBuilder::SP msg_builder;
    eular::stun::SocketAddress      external_address; // 公网地址
    eular::stun::SocketAddress      changed_address; // binging response中CHANGED-ADDRESS属性

    std::string     connectivity_testing_service_host;
    std::string     connectivity_testing_service_ip;
    uint16_t        connectivity_testing_service_port = 80;

    // event handling
    struct event_base*  base;
    struct event*       timeout_event;
    struct event*       read_event;

    StunClientConfig() :
        server_ip("stun.voipstunt.com"), // stun.l.google.com
        server_port(3478), // 19302
        username(""),
        password(""),
        use_ipv6(false),
        verbose(false),
        timeout(1),
        max_retries(2),
        open_public(false),
        nat_type(0)
    {
    }
};

StunClientConfig g_config;

enum StunState {
    BINDING_REQUEST_SENT,           // 发送了Binding Request
    CHANGE_REQUEST_FOR_FIREWALL,    // 检测到开放的公网IP，发送了带有CHANGE-REQUEST属性的Binding Request以检测防火墙
    CHANGE_REQUEST_FOR_CONE,        // Nat类型检测，发送了带有CHANGE-REQUEST属性的Binding Request以检测锥形NAT
    BINDING_REQUEST_FOR_SYMMETRIC,  // Nat类型检测，发送Binding Request以检测对称NAT
    CHANGE_PORT_FOR_RESTRICTED,     // Nat类型检测，发送了带有CHANGE-REQUEST(change port)属性的Binding Request以检测受限锥形NAT
    COMPLETED,
    FAILED
};

enum NatType {
    Unknown,                // 未知类型
    OpenPublic,             // 开放的公网IP
    OpenPublicWithFirewall, // 有防火墙的开放公网IP
    FullCone,               // 全锥形NAT
    IPRestrictedCone,       // IP限制锥形NAT
    PortRestrictedCone,     // 端口限制锥形NAT
    Symmetric,              // 对称NAT
};

std::array<uint8_t, STUN_TRX_ID_SIZE> GenerateTransactionTd()
{
    std::array<uint8_t, STUN_TRX_ID_SIZE> trx_id;
    for (size_t i = 0; i < STUN_TRX_ID_SIZE; ++i) {
        trx_id[i] = rand() % 256; // 随机生成0-255之间的字节
    }

    return trx_id;
}

void sendto_peer(StunClientConfig *config, bool changed_addr = false)
{
    struct sockaddr_storage peer_addr;
    memset(&peer_addr, 0, sizeof(struct sockaddr_storage));
    if (config->use_ipv6) {
        struct sockaddr_in6 *server_addr6 = (struct sockaddr_in6 *)&peer_addr;

        server_addr6->sin6_family = AF_INET6;
        server_addr6->sin6_port = htons(config->server_port);
        inet_pton(AF_INET6, config->server_ip.c_str(), &server_addr6->sin6_addr);
        if (changed_addr) {
            memcpy(server_addr6, config->changed_address.getSockAddr(), config->changed_address.getSockAddrLength());
        }
    } else {
        struct sockaddr_in *server_addr = (struct sockaddr_in *)&peer_addr;

        server_addr->sin_family = AF_INET;
        server_addr->sin_port = htons(config->server_port);
        inet_pton(AF_INET, config->server_ip.c_str(), &server_addr->sin_addr);
        if (changed_addr) {
            memcpy(server_addr, config->changed_address.getSockAddr(), config->changed_address.getSockAddrLength());
        }
    }
    std::vector<uint8_t> stun_msg = config->msg_builder->message();

    if (sendto(config->sock, stun_msg.data(), stun_msg.size(), 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0) {
        perror("sendto");
        exit(EXIT_FAILURE);
        return;
    }

    if (config->retries >= 0) {
        evtimer_del(config->timeout_event);
        struct timeval timeout = {config->timeout, 0};
        event_add(config->timeout_event, &timeout);
    }
}

void read_event_callback(evutil_socket_t sock, short ev, void *arg)
{
    (void)ev;

    StunClientConfig *config = static_cast<StunClientConfig *>(arg);
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    uint8_t buffer[2048];
    ssize_t bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0,
                                      (struct sockaddr *)&peer_addr, &peer_addr_len);
    if (bytes_received < 0) {
        perror("recvfrom");
        exit(EXIT_FAILURE);
    }

    eular::stun::SocketAddress peer_address((struct sockaddr *)&peer_addr);
    STUNC_LOGD("Received STUN message from %s:%d\n", peer_address.getIp().c_str(), peer_address.getPort());
    eular::stun::StunMsgParser parser;
    if (!parser.parse(buffer, bytes_received)) {
        fprintf(stderr, "Failed to parse STUN message.\n");
        return;
    }
    event_del(config->timeout_event);
    STUNC_LOGD("Received STUN message of type: 0x%02x\n", parser.msgType());

    if (parser.msgType() == ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_RESPONSE)) {
        if (config->state == StunState::BINDING_REQUEST_SENT) {
            // 处理 Binding-Response
            STUNC_LOGD("Received STUN Binding Response from %s:%d\n", config->server_ip.c_str(), config->server_port);

            const eular::any *changedAddr = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_CHANGED_ADDRESS));
            if (changedAddr) {
                const eular::stun::SocketAddress *changed_address = eular::any_cast<eular::stun::SocketAddress>(changedAddr);
                config->changed_address = *changed_address;
                STUNC_LOGD("Changed Address: %s:%d\n", changed_address->getIp().c_str(), changed_address->getPort());
            } else {
                STUNC_LOGD("Received STUN Binding Response without Changed Address attribute.\n");
            }

            const eular::stun::SocketAddress *mapped_address = nullptr;
            const eular::any *xorMappedAddr = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_XOR_MAPPED_ADDRESS));
            if (xorMappedAddr) {
                mapped_address = eular::any_cast<eular::stun::SocketAddress>(xorMappedAddr);
                STUNC_LOGD("XOR-Mapped Address: %s:%d\n", mapped_address->getIp().c_str(), mapped_address->getPort());
            } else {
                STUNC_LOGD("Received STUN Binding Response without XOR-Mapped Address attribute.\n");
            }

            const eular::any *mappedAddr = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_MAPPED_ADDRESS));
            if (mappedAddr) {
                mapped_address = eular::any_cast<eular::stun::SocketAddress>(mappedAddr);
                STUNC_LOGD("Mapped Address: %s:%d\n", mapped_address->getIp().c_str(), mapped_address->getPort());
            } else {
                STUNC_LOGD("Received STUN Binding Response without Mapped Address attribute.\n");
            }

            if (mapped_address) {
                // 获取本地地址
                config->external_address = *mapped_address;
                for (const auto &iface : config->interfaces) {
                    if (iface.ip_address == mapped_address->getIp()) {
                        config->open_public = true;
                        config->state = StunState::COMPLETED;
                        config->nat_type = NatType::OpenPublic;
                        printf("NAT Type: Open Public\n");
                        event_base_loopbreak(config->base);
                        event_free(config->read_event);
                        event_free(config->timeout_event);
                        close(config->sock);
                        return;
                    }
                }

                config->msg_builder->setMsgType(ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST));
                config->msg_builder->setTransactionId(parser.transactionId());
                config->msg_builder->addAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_CHANGE_REQUEST), static_cast<uint32_t>(CHANGE_IP | CHANGE_PORT));
                config->open_public = false;
                STUNC_LOGD("Sending STUN Binding Request with CHANGE-REQUEST (change ip and port) to test for Full Cone...\n");
                config->state = StunState::CHANGE_REQUEST_FOR_CONE;
                config->retries = config->max_retries;
                sendto_peer(config);
            } else {
                config->state = StunState::FAILED;
                event_base_loopbreak(config->base);
                event_free(config->read_event);
                event_free(config->timeout_event);
                close(config->sock);
                return;
            }
        } else if (config->state == StunState::BINDING_REQUEST_FOR_SYMMETRIC) {
            const eular::any *mappedAddr = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_MAPPED_ADDRESS));
            if (mappedAddr) {
                const eular::stun::SocketAddress *mapped_address = eular::any_cast<eular::stun::SocketAddress>(mappedAddr);
                STUNC_LOGD("Other server response: mapped address: %s:%d\n", mapped_address->getIp().c_str(), mapped_address->getPort());
                if (mapped_address->getIp() == config->external_address.getIp() &&
                    mapped_address->getPort() == config->external_address.getPort()) {
                    // 锥形NAT, 发送Test III
                    config->msg_builder->setMsgType(ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST));
                    config->msg_builder->setTransactionId(parser.transactionId());
                    config->msg_builder->addAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_CHANGE_REQUEST), static_cast<uint32_t>(CHANGE_PORT));
                    printf("Sending STUN Binding Request with CHANGE-REQUEST (change port) to test for Restricted Cone NAT...\n");
                    config->state = StunState::CHANGE_PORT_FOR_RESTRICTED;
                    config->retries = config->max_retries;
                    sendto_peer(config);
                } else {
                    // 端口改变，说明是对称NAT
                    printf("NAT Type: Symmetric NAT\n");
                    config->nat_type = NatType::Symmetric;
                    config->state = StunState::COMPLETED;
                    event_base_loopbreak(config->base);
                    event_free(config->read_event);
                    event_free(config->timeout_event);
                    close(config->sock);
                    return;
                }
            } else {
                printf("Received STUN Binding Response without Mapped Address attribute.\n");
                exit(EXIT_FAILURE);
            }
        } else if (config->state == StunState::CHANGE_REQUEST_FOR_FIREWALL) {
            // 处理防火墙测试的响应
            printf("NAT Type: Open Internet\n");
            config->nat_type = NatType::OpenPublic;
            config->state = StunState::COMPLETED;
            event_base_loopbreak(config->base);
            event_free(config->read_event);
            event_free(config->timeout_event);
            close(config->sock);
            return;
        } else if (config->state == StunState::CHANGE_REQUEST_FOR_CONE) {
            if (peer_address == config->changed_address) {
                // 处理锥形NAT测试的响应
                printf("NAT Type: Full Cone NAT\n");
                config->nat_type = NatType::FullCone;
                config->state = StunState::COMPLETED;
                event_base_loopbreak(config->base);
                event_free(config->read_event);
                event_free(config->timeout_event);
                close(config->sock);
            }
        } else if (config->state == StunState::CHANGE_PORT_FOR_RESTRICTED) {
            // 处理受限锥形NAT测试的响应
            printf("NAT Type: IP Restricted Cone NAT\n");
            config->nat_type = NatType::IPRestrictedCone;
            config->state = StunState::COMPLETED;
            event_base_loopbreak(config->base);
            event_free(config->read_event);
            event_free(config->timeout_event);
            close(config->sock);
            return;
        } else {
            printf("Unexpected state in STUN Binding Response handling: %d\n", config->state);
            exit(EXIT_FAILURE);
        }
        // TODO: 处理响应中的属性
    } else if (parser.msgType() == ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_ERROR_RESPONSE)) {
        // 处理错误响应
        const eular::any *errorMsg = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_ERROR_CODE));
        if (errorMsg) {
            const eular::stun::StunAttrErrorCode *error_code = eular::any_cast<eular::stun::StunAttrErrorCode>(errorMsg);
            STUNC_LOGD("STUN Binding Error Response: %s\n", error_code->error_reason.c_str());
        } else {
            STUNC_LOGD("Received STUN Binding Error Response without error code attribute.\n");
        }
        exit(EXIT_FAILURE);
    } else {
        fprintf(stderr, "Received unexpected STUN message type: %u\n", parser.msgType());
        exit(EXIT_FAILURE);
    }
}

void stun_nat_detect(StunClientConfig *config)
{
    // 1、创建UDP socket
    int sockfd = socket(config->use_ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return;
    }
    config->sock = sockfd;
    STUNC_LOGD("Created socket %d for STUN client.\n", sockfd);

    evutil_make_socket_nonblocking(sockfd);
    // evutil_make_listen_socket_reuseable_port(sockfd);
    // struct sockaddr_in local_addr;
    // memset(&local_addr, 0, sizeof(local_addr));
    // local_addr.sin_family = config->use_ipv6 ? AF_INET6 : AF_INET;
    if (!config->interface.empty()) {
#if defined(SO_BINDTODEVICE)
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, config->interface.c_str(), IFNAMSIZ);
        if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
            perror("setsockopt SO_BINDTODEVICE");
            close(sockfd);
            return;
        }
#endif
    }

    // 2、发送Test I
    config->msg_builder = std::make_shared<eular::stun::StunMsgBuilder>();
    config->msg_builder->setMsgType(ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST));
    uint8_t tsx_id[STUN_TRX_ID_SIZE] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    config->msg_builder->setTransactionId(tsx_id);
    STUNC_LOGD("Sending STUN Binding Request to %s:%d\n", config->server_ip.c_str(), config->server_port);

    config->read_event = event_new(config->base, sockfd, EV_READ | EV_PERSIST, read_event_callback, config);
    event_add(config->read_event, nullptr);
    config->timeout_event = evtimer_new(config->base, [] (evutil_socket_t sock, short ev, void *arg) {
        (void)sock;
        (void)ev;

        StunClientConfig *config = static_cast<StunClientConfig *>(arg);
        STUNC_LOGD("STUN request timed out.\n");

        if (config->retries--) {
            sendto_peer(config);
            return;
        }

        if (config->state == StunState::CHANGE_REQUEST_FOR_FIREWALL) {
            printf("NAT Type: Open Internet (with Firewall)\n");
            config->nat_type = NatType::OpenPublicWithFirewall;
            config->state = StunState::COMPLETED;
        } else if (config->state == StunState::CHANGE_REQUEST_FOR_CONE) {
            config->msg_builder->setMsgType(ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST));
            config->msg_builder->setTransactionId(GenerateTransactionTd());
            STUNC_LOGD("Sending STUN Binding Request with CHANGE-REQUEST (change port) to test for Symmetric...\n");

            // 发送Test I到CHANGED-ADDRESS
            config->retries = config->max_retries;
            config->state = BINDING_REQUEST_FOR_SYMMETRIC;
            sendto_peer(config, true);
            return;
        } else if (config->state == StunState::CHANGE_PORT_FOR_RESTRICTED) {
            printf("NAT Type: Port Restricted Cone NAT\n");
            config->nat_type = NatType::PortRestrictedCone;
            config->state = StunState::COMPLETED;
        } else {
            printf("NAT Type: UDP Blocked\n");
            config->state = StunState::FAILED;
        }

        event_base_loopbreak(config->base);
        event_free(config->read_event);
        event_free(config->timeout_event);
        close(config->sock);
    }, config);

    sendto_peer(config);
    config->state = StunState::BINDING_REQUEST_SENT;
    STUNC_LOGD("Waiting for STUN Binding Response...\n");
}

bool TestConnectivity(const std::string& interface_name, const std::string& source_ip = "")
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket error");
        return false;
    }

    // 设置socket选项，允许绑定到特定接口
    if (!source_ip.empty()) {
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = inet_addr(source_ip.c_str());
        local_addr.sin_port = 0;

        if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            perror("bind error");
            close(sock);
            return false;
        }
    }

#if defined(SO_BINDTODEVICE)
    // 绑定网卡
    if (!interface_name.empty()) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
        if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
            perror("bind to device error");
            close(sock);
            return false;
        }
    }
#endif

    // 设置连接超时
    struct timeval timeout = {1, 0};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sock);
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_SNDTIMEO");
        close(sock);
        return false;
    }

    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_addr.s_addr = inet_addr(g_config.connectivity_testing_service_ip.c_str());
    remote_addr.sin_port = htons(g_config.connectivity_testing_service_port);

    // 连接到服务端
    if (connect(sock, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
        perror("connect error");
        close(sock);
        return false;
    }

    close(sock);
    return true;
}

std::vector<NetworkInterface> GetNetworkInterfaces() {
    std::vector<NetworkInterface> interfaces;
    
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return interfaces;
    }

    // 第一次遍历：收集所有接口的基本信息
    std::vector<std::string> interface_names;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL) continue;

        std::string name(ifa->ifa_name);

        // 检查是否已处理过此接口
        bool found = false;
        for (const auto& existing : interface_names) {
            if (existing == name) {
                found = true;
                break;
            }
        }

        if (!found) {
            interface_names.push_back(name);
        }
    }

    // 为每个接口获取详细信息
    for (const auto& name : interface_names) {
        // 获取接口标志
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) continue;

        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

        bool is_up = false;
        std::string ip_address = "";

        // 获取接口标志
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            is_up = (ifr.ifr_flags & IFF_UP) != 0;
        }

        // 获取IPv4地址
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_name && name == ifa->ifa_name && ifa->ifa_addr) {
                if (ifa->ifa_addr->sa_family == AF_INET) { // IPv4
                    struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);
                    ip_address = std::string(ip_str);
                    break;
                }
            }
        }

        close(sock);

        // 创建网络接口对象
        NetworkInterface iface(name, ip_address, is_up);

        // 如果接口是UP状态且有IP地址，测试是否可以发送数据
        if (is_up && !ip_address.empty() && ip_address != "127.0.0.1") {
            iface.can_send_data = TestConnectivity(name, ip_address);
        }

        interfaces.push_back(iface);
    }

    freeifaddrs(ifaddr);
    return interfaces;
}

int main(int argc, char **argv)
{
    // Parse command line arguments
    CLI::App app{"STUN Client"};
    app.add_option("-i,--interface", g_config.interface, "Network interface to bind")->default_val("");
    app.add_option("-s,--server", g_config.server_host, "STUN server IP address")->default_val("stun.voipstunt.com");
    app.add_option("-p,--port", g_config.server_port, "STUN server port")->default_val(3478);
    app.add_option("-U,--username", g_config.username, "STUN username")->default_val("");
    app.add_option("-P,--password", g_config.password, "STUN password")->default_val("");
    app.add_flag("-6,--ipv6", g_config.use_ipv6, "Use IPv6")->default_val(false);
    app.add_flag("-v,--verbose", g_config.verbose, "Enable verbose output")->default_val(false);
    app.add_option("-t,--timeout", g_config.timeout, "Timeout in seconds")->default_val(1);
    app.add_option("-r,--retries", g_config.max_retries, "Maximum number of retries")->default_val(2);
    app.add_option("-c,--connectivity-service", g_config.connectivity_testing_service_host, "Connectivity testing service host")->default_val("www.baidu.com");

    CLI11_PARSE(app, argc, argv);
    g_config.retries = g_config.max_retries;

    struct addrinfo *result;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (0 != getaddrinfo(g_config.connectivity_testing_service_host.c_str(), NULL, NULL, &result)) {
        perror("getaddrinfo error");
        return 1;
    }

    // 遍历所有返回的地址信息
    char ipstr[INET6_ADDRSTRLEN];
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        void *addr;

        if (rp->ai_family == AF_INET) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
            addr = &(ipv4->sin_addr);

            inet_ntop(rp->ai_family, addr, ipstr, sizeof(ipstr));
            g_config.connectivity_testing_service_ip = ipstr;
            STUNC_LOGD("Connectivity testing service IP: '%s'\n", g_config.connectivity_testing_service_ip.c_str());
            break;
        }
    }
    freeaddrinfo(result);

    auto interfaces = GetNetworkInterfaces();
    std::string interface;
    for (const auto &it : interfaces) {
        if (it.can_send_data) {
            STUNC_LOGD("Interface %s(%s) can send data\n", it.name.c_str(), it.ip_address.c_str());
            if (interface.empty()) {
                interface = it.name;
            }
        } else {
            STUNC_LOGD("Interface %s(%s) cannot send data\n", it.name.c_str(), it.ip_address.c_str());
        }
    }
    if (g_config.interface.empty()) {
        g_config.interface = interface;
        STUNC_LOGD("No interface specified, using default: %s\n", interface.c_str());
    }

    // Initialize libevent
    g_config.base = event_base_new();
    if (!g_config.base) {
        STUNC_LOGD("Could not initialize libevent!\n");
        return 1;
    }

    // 解析DNS
    struct evdns_base *dns_base = evdns_base_new(g_config.base, 1);
    if (!dns_base) {
        STUNC_LOGD("Could not initialize evdns base!\n");
        event_base_free(g_config.base);
        return 1;
    }
    evdns_base_set_option(dns_base, "timeout", "3");
    evdns_base_set_option(dns_base, "attempts", "2");
    auto request = evdns_base_resolve_ipv4(dns_base, g_config.server_host.c_str(), 0,
        [] (int result, char type, int count, int ttl, void *addresses, void *arg) {
            (void)type;
            (void)ttl;

            if (result != 0 || count == 0) {
                STUNC_LOGD("DNS resolution failed with result: %d, %s\n", result, evutil_gai_strerror(result));
                return;
            }

            StunClientConfig *config = static_cast<StunClientConfig *>(arg);
            struct in_addr *addrs  = static_cast<struct in_addr *>(addresses);
            char ip_str[INET_ADDRSTRLEN];
            for (int i = 0; i < count; ++i) {
                if (inet_ntop(AF_INET, &addrs[i], ip_str, INET_ADDRSTRLEN)) {
                    STUNC_LOGD("Resolved STUN server IP: %s\n", ip_str);
                    config->server_ip = ip_str;
                    stun_nat_detect(config);
                    break;
                }
            }
        },
        &g_config);

    if (!request) {
        STUNC_LOGD("Could not create DNS request!\n");
        evdns_base_free(dns_base, 0);
        event_base_free(g_config.base);
        return 1;
    }

    event_base_dispatch(g_config.base);
    evdns_base_free(dns_base, 0);
    event_base_free(g_config.base);
    return 0;
}
