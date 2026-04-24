#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <set>
#include <vector>
#include <errno.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <ntrs_codec.h>
#include <stun.h>
#include <stun_types.h>
#include <socket_address.h>

struct NatInfo {
    std::string local_ip;
    uint16_t local_port;
    std::string srflx_ip;
    uint16_t srflx_port;
    std::string srflx_ip_2;
    uint16_t srflx_port_2;
    bool mapping_stable;
    std::string nat_risk;
    bool probe1_ok;
    bool probe2_ok;
    int probe1_rtt_ms;
    int probe2_rtt_ms;
    int probe_rounds;
    int probe1_success_count;
    int probe2_success_count;
    int probe1_distinct_mappings;
    int probe2_distinct_mappings;
    std::string nat_type;
    bool filter_same_ip_diff_port_rx;
    bool filter_diff_ip_rx;
};

static uint64_t g_req = 1;

static bool parse_endpoint(const std::string &input, std::string *host, uint16_t *port)
{
    size_t pos = input.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos == input.size() - 1) {
        return false;
    }

    std::string p = input.substr(pos + 1);
    int parsed = atoi(p.c_str());
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }

    *host = input.substr(0, pos);
    *port = (uint16_t)parsed;
    return true;
}

static bool send_line(int fd, const std::string &line)
{
    const char *p = line.c_str();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    return true;
}

static bool recv_line(int fd, std::string *line)
{
    line->clear();
    for (;;) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n <= 0) {
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        line->push_back(ch);
    }
}

static int connect_ntrs(const std::string &ntrs_ip, int ntrs_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons((uint16_t)ntrs_port);
    if (inet_pton(AF_INET, ntrs_ip.c_str(), &srv.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool request_probe_endpoints(int fd, std::string *stun1, std::string *stun2)
{
    eular::ntrs::Message req;
    req.type = eular::ntrs::MessageType::NAT_PROBE_REQ;
    req.request_id = g_req++;
    req.fields["version"] = "m1";
    if (!send_line(fd, eular::ntrs::encodeMessage(req))) {
        return false;
    }

    std::string line;
    if (!recv_line(fd, &line)) {
        return false;
    }

    eular::ntrs::Message rsp;
    if (!eular::ntrs::decodeMessage(line, &rsp) || rsp.type != eular::ntrs::MessageType::NAT_PROBE_RSP) {
        return false;
    }

    *stun1 = rsp.fields["stun1"];
    *stun2 = rsp.fields["stun2"];
    return !stun1->empty();
}

static bool request_filter_probe(int fd,
                                 const std::string &target_ip,
                                 uint16_t target_port,
                                 const std::string &token)
{
    eular::ntrs::Message req;
    req.type = eular::ntrs::MessageType::FILTER_PROBE_REQ;
    req.request_id = g_req++;
    req.fields["target_ip"] = target_ip;
    req.fields["target_port"] = std::to_string(target_port);
    req.fields["token"] = token;
    if (!send_line(fd, eular::ntrs::encodeMessage(req))) {
        return false;
    }

    std::string line;
    if (!recv_line(fd, &line)) {
        return false;
    }

    eular::ntrs::Message rsp;
    if (!eular::ntrs::decodeMessage(line, &rsp) || rsp.type != eular::ntrs::MessageType::FILTER_PROBE_RSP) {
        return false;
    }

    return rsp.fields["result"] == "ok" || rsp.fields["result"] == "degraded";
}

static void wait_filter_probe_packets(int sock,
                                      const std::string &token,
                                      int wait_ms,
                                      bool *same_ip_diff_port_rx,
                                      bool *diff_ip_rx)
{
    if (same_ip_diff_port_rx == NULL || diff_ip_rx == NULL) {
        return;
    }

    *same_ip_diff_port_rx = false;
    *diff_ip_rx = false;

    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        uint8_t buf[512];
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src_addr, &src_len);
        if (n <= 0) {
            break;
        }

        buf[n] = '\0';
        std::string payload((char *)buf);
        std::string prefix = "NTRS_FILTER_PROBE|" + token + "|";
        if (payload.find(prefix) != 0) {
            continue;
        }

        std::string tag = payload.substr(prefix.size());
        if (tag == "same_ip_diff_port") {
            *same_ip_diff_port_rx = true;
        } else if (tag == "diff_ip") {
            *diff_ip_rx = true;
        }

        if (*same_ip_diff_port_rx && *diff_ip_rx) {
            break;
        }
    }

    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static bool resolve_ipv4(const std::string &host, std::string *ip)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *result = NULL;
    int ret = getaddrinfo(host.c_str(), NULL, &hints, &result);
    if (ret != 0 || result == NULL) {
        return false;
    }

    char buf[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in *in = (struct sockaddr_in *)result->ai_addr;
    inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
    *ip = buf;

    freeaddrinfo(result);
    return true;
}

static int create_probe_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

static uint16_t parse_u16(const std::string &value)
{
    int v = atoi(value.c_str());
    if (v <= 0 || v > 65535) {
        return 0;
    }
    return (uint16_t)v;
}

static bool recv_line_timeout(int fd, int timeout_ms, std::string *line)
{
    line->clear();
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) {
            return false;
        }

        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n <= 0) {
            return false;
        }

        if (ch == '\n') {
            return true;
        }
        line->push_back(ch);
    }
}

static bool try_udp_hole_punch(int sock,
                               const std::string &peer_ip,
                               uint16_t peer_port,
                               const std::string &tag)
{
    if (peer_ip.empty() || peer_port == 0) {
        return false;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &dst.sin_addr) != 1) {
        return false;
    }

    printf("KCP punch start: %s -> %s:%u\n", tag.c_str(), peer_ip.c_str(), peer_port);
    for (int i = 0; i < 8; ++i) {
        char payload[128];
        snprintf(payload, sizeof(payload), "NTRS_KCP_PUNCH|%s|%d", tag.c_str(), i + 1);
        sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&dst, sizeof(dst));

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int ret = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(sock, &rfds)) {
            uint8_t buf[512];
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);
            ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                buf[n] = '\0';
                char src_ip[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
                printf("KCP punch rx from %s:%u payload=%s\n", src_ip, ntohs(src.sin_port), (char *)buf);
                return true;
            }
        }

        usleep(120000);
    }

    printf("KCP punch timeout for %s (%s:%u)\n", tag.c_str(), peer_ip.c_str(), peer_port);
    return false;
}

static void try_hole_punch_from_signal(int sock, const eular::ntrs::Message &signal, const char *from)
{
    std::string peer_id = signal.fields.find("peer_id") != signal.fields.end() ? signal.fields.at("peer_id") : "";
    std::string ip1 = signal.fields.find("peer_srflx_ip") != signal.fields.end() ? signal.fields.at("peer_srflx_ip") : "";
    uint16_t port1 = signal.fields.find("peer_srflx_port") != signal.fields.end() ? parse_u16(signal.fields.at("peer_srflx_port")) : 0;
    std::string ip2 = signal.fields.find("peer_srflx_ip_2") != signal.fields.end() ? signal.fields.at("peer_srflx_ip_2") : "";
    uint16_t port2 = signal.fields.find("peer_srflx_port_2") != signal.fields.end() ? parse_u16(signal.fields.at("peer_srflx_port_2")) : 0;
    std::string nat_type = signal.fields.find("peer_nat_type") != signal.fields.end() ? signal.fields.at("peer_nat_type") : "unknown";

    printf("Session signal(%s): peer=%s nat=%s endpoint1=%s:%u endpoint2=%s:%u\n",
           from,
           peer_id.c_str(),
           nat_type.c_str(),
           ip1.c_str(),
           port1,
           ip2.c_str(),
           port2);

    bool ok = false;
    if (!ip1.empty() && port1 > 0) {
        ok = try_udp_hole_punch(sock, ip1, port1, "primary");
    }
    if (!ok && !ip2.empty() && port2 > 0) {
        ok = try_udp_hole_punch(sock, ip2, port2, "secondary");
    }

    printf("KCP hole punch result (%s): %s\n", from, ok ? "ok" : "failed");
}

static bool detect_nat_basic(int sock,
                             const std::string &stun_host,
                             uint16_t stun_port,
                             NatInfo *info,
                             int *out_rtt_ms,
                             std::string *out_ip,
                             uint16_t *out_port)
{
    if (info == NULL || out_rtt_ms == NULL || out_ip == NULL || out_port == NULL) {
        return false;
    }

    std::string stun_ip = stun_host;
    if (inet_addr(stun_host.c_str()) == INADDR_NONE) {
        std::string resolved;
        if (resolve_ipv4(stun_host, &resolved)) {
            stun_ip = resolved;
        }
    }

    eular::stun::StunMsgBuilder builder;
    builder.setMsgType(ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST));
    std::vector<uint8_t> msg = builder.message();

    struct sockaddr_in stun_addr;
    memset(&stun_addr, 0, sizeof(stun_addr));
    stun_addr.sin_family = AF_INET;
    stun_addr.sin_port = htons(stun_port);
    inet_pton(AF_INET, stun_ip.c_str(), &stun_addr.sin_addr);

    ssize_t sent = sendto(sock, msg.data(), msg.size(), 0, (struct sockaddr *)&stun_addr, sizeof(stun_addr));
    if (sent < 0) {
        printf("STUN send failed %s:%u errno=%d(%s)\n",
            stun_host.c_str(),
            stun_port,
            errno,
            strerror(errno));
        return false;
    }

    struct timeval t0;
    gettimeofday(&t0, NULL);

    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(sock, (struct sockaddr *)&local_addr, &local_len) == 0) {
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &local_addr.sin_addr, ip, sizeof(ip));
        info->local_ip = ip;
        info->local_port = ntohs(local_addr.sin_port);
    }

    bool ok = false;
    uint8_t buf[1500];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src_addr, &src_len);
    if (n > 0) {
        struct in_addr expected_addr;
        inet_pton(AF_INET, stun_ip.c_str(), &expected_addr);
        if (src_addr.sin_family != AF_INET ||
            src_addr.sin_port != htons(stun_port) ||
            src_addr.sin_addr.s_addr != expected_addr.s_addr) {
            return false;
        }

        eular::stun::StunMsgParser parser;
        bool parsed = parser.parse(buf, (size_t)n);
        if (parsed && parser.msgType() == ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_RESPONSE)) {
            const eular::any *xorMapped = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_XOR_MAPPED_ADDRESS));
            const eular::stun::SocketAddress *addr = NULL;
            if (xorMapped != NULL) {
                addr = eular::any_cast<eular::stun::SocketAddress>(xorMapped);
            } else {
                const eular::any *mapped = parser.getAttribute(ENUM_CLASS(eular::stun::StunAttributeType::STUN_ATTR_MAPPED_ADDRESS));
                if (mapped != NULL) {
                    addr = eular::any_cast<eular::stun::SocketAddress>(mapped);
                }
            }

            if (addr != NULL) {
                *out_ip = addr->getIp();
                *out_port = addr->getPort();
                struct timeval t1;
                gettimeofday(&t1, NULL);
                long delta_us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L + (long)(t1.tv_usec - t0.tv_usec);
                if (delta_us < 0) {
                    delta_us = 0;
                }
                *out_rtt_ms = (int)(delta_us / 1000L);
                ok = true;
            }
        }
    } else if (n == 0) {
        printf("STUN recv empty %s:%u\n", stun_host.c_str(), stun_port);
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("STUN recv timeout %s:%u\n", stun_host.c_str(), stun_port);
        } else {
            printf("STUN recv failed %s:%u errno=%d(%s)\n",
                stun_host.c_str(),
                stun_port,
                errno,
                strerror(errno));
        }
    }

    return ok;
}

static bool detect_nat_with_retry(int sock,
                                  const std::string &stun_host,
                                  uint16_t stun_port,
                                  int retries,
                                  NatInfo *out,
                                  bool first_probe)
{
    if (out == NULL) {
        return false;
    }

    for (int i = 0; i < retries; ++i) {
        int rtt_ms = -1;
        std::string ip = "0.0.0.0";
        uint16_t port = 0;
        bool ok = detect_nat_basic(sock, stun_host, stun_port, out, &rtt_ms, &ip, &port);
        if (ok) {
            if (first_probe) {
                out->srflx_ip = ip;
                out->srflx_port = port;
                out->probe1_ok = true;
                out->probe1_rtt_ms = rtt_ms;
            } else {
                out->srflx_ip_2 = ip;
                out->srflx_port_2 = port;
                out->probe2_ok = true;
                out->probe2_rtt_ms = rtt_ms;
            }
            return true;
        }
    }

    return false;
}

static void collect_probe_samples(int sock,
                                  const std::string &stun_host,
                                  uint16_t stun_port,
                                  int rounds,
                                  int retries,
                                  NatInfo *out,
                                  bool first_probe)
{
    std::set<std::string> mappings;
    int success_count = 0;
    long rtt_sum_ms = 0;

    for (int round = 0; round < rounds; ++round) {
        NatInfo sample = *out;
        bool ok = detect_nat_with_retry(sock, stun_host, stun_port, retries, &sample, first_probe);
        if (ok) {
            ++success_count;
            const std::string ip = first_probe ? sample.srflx_ip : sample.srflx_ip_2;
            const uint16_t port = first_probe ? sample.srflx_port : sample.srflx_port_2;
            mappings.insert(ip + ":" + std::to_string(port));

            if (first_probe) {
                out->local_ip = sample.local_ip;
                out->local_port = sample.local_port;
                out->srflx_ip = sample.srflx_ip;
                out->srflx_port = sample.srflx_port;
                out->probe1_rtt_ms = sample.probe1_rtt_ms;
                rtt_sum_ms += sample.probe1_rtt_ms;
            } else {
                out->srflx_ip_2 = sample.srflx_ip_2;
                out->srflx_port_2 = sample.srflx_port_2;
                out->probe2_rtt_ms = sample.probe2_rtt_ms;
                rtt_sum_ms += sample.probe2_rtt_ms;
            }
        }
        usleep(30000);
    }

    if (first_probe) {
        out->probe1_ok = success_count > 0;
        out->probe1_success_count = success_count;
        out->probe1_distinct_mappings = (int)mappings.size();
        out->probe1_rtt_ms = success_count > 0 ? (int)(rtt_sum_ms / success_count) : -1;
    } else {
        out->probe2_ok = success_count > 0;
        out->probe2_success_count = success_count;
        out->probe2_distinct_mappings = (int)mappings.size();
        out->probe2_rtt_ms = success_count > 0 ? (int)(rtt_sum_ms / success_count) : -1;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: %s <ntrs_ip> <ntrs_port> [peer_id device_id [dst_peer|-] [stun1_host:port] [stun2_host:port]]\n", argv[0]);
        return 1;
    }

    std::string ntrs_ip = argv[1];
    int ntrs_port = atoi(argv[2]);
    bool probe_only = argc < 5;

    std::string peer_id = (argc > 3) ? argv[3] : "";
    std::string device_id = (argc > 4) ? argv[4] : "";
    std::string dst_peer = (argc > 5) ? argv[5] : "";
    if (dst_peer == "-") {
        dst_peer.clear();
    }

    std::string stun1 = (argc > 6) ? argv[6] : "";
    std::string stun2 = (argc > 7) ? argv[7] : "";

    int fd = connect_ntrs(ntrs_ip, ntrs_port);
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    if (stun1.empty()) {
        if (!request_probe_endpoints(fd, &stun1, &stun2)) {
            printf("request NAT probe endpoints failed\n");
            close(fd);
            return 1;
        }
        printf("NTRS probe endpoints: stun1=%s stun2=%s\n", stun1.c_str(), stun2.empty() ? "-" : stun2.c_str());
    }

    std::string stun1_host;
    uint16_t stun1_port = 0;
    if (!parse_endpoint(stun1, &stun1_host, &stun1_port)) {
        printf("invalid stun1 endpoint: %s\n", stun1.c_str());
        return 1;
    }

    std::string stun2_host;
    uint16_t stun2_port = 0;
    bool has_stun2 = false;
    if (!stun2.empty()) {
        if (!parse_endpoint(stun2, &stun2_host, &stun2_port)) {
            printf("invalid stun2 endpoint: %s\n", stun2.c_str());
            return 1;
        }
        has_stun2 = true;
    }

    NatInfo nat;
    nat.local_ip = "0.0.0.0";
    nat.local_port = 0;
    nat.srflx_ip = "0.0.0.0";
    nat.srflx_port = 0;
    nat.srflx_ip_2 = "0.0.0.0";
    nat.srflx_port_2 = 0;
    nat.mapping_stable = false;
    nat.nat_risk = "high";
    nat.probe1_ok = false;
    nat.probe2_ok = false;
    nat.probe1_rtt_ms = -1;
    nat.probe2_rtt_ms = -1;
    nat.probe_rounds = 3;
    nat.probe1_success_count = 0;
    nat.probe2_success_count = 0;
    nat.probe1_distinct_mappings = 0;
    nat.probe2_distinct_mappings = 0;
    nat.nat_type = "unknown";
    nat.filter_same_ip_diff_port_rx = false;
    nat.filter_diff_ip_rx = false;

    int probe_sock = create_probe_socket();
    if (probe_sock < 0) {
        printf("create probe socket failed\n");
        close(fd);
        return 1;
    }

    collect_probe_samples(probe_sock, stun1_host, stun1_port, nat.probe_rounds, 3, &nat, true);
    bool ok1 = nat.probe1_ok;
    bool ok2 = false;
    if (ok1 && has_stun2) {
        std::string token = std::to_string((unsigned long long)g_req) + "_" + std::to_string((unsigned long long)getpid());
        if (request_filter_probe(fd, nat.srflx_ip, nat.srflx_port, token)) {
            wait_filter_probe_packets(probe_sock,
                                      token,
                                      900,
                                      &nat.filter_same_ip_diff_port_rx,
                                      &nat.filter_diff_ip_rx);
        }
    }

    if (has_stun2) {
        collect_probe_samples(probe_sock, stun2_host, stun2_port, nat.probe_rounds, 3, &nat, false);
        ok2 = nat.probe2_ok;
    }

    if (!ok1) {
        nat.mapping_stable = false;
        nat.nat_risk = "high";
        nat.nat_type = "udp_blocked_or_restricted";
    } else if (ok1 && !has_stun2) {
        nat.mapping_stable = true;
        nat.nat_risk = "medium";
        nat.nat_type = "single_stun_limited";
    } else if (ok1 && ok2) {
        nat.mapping_stable = (nat.srflx_ip == nat.srflx_ip_2 && nat.srflx_port == nat.srflx_port_2);
        bool unstable_mapping = nat.probe1_distinct_mappings > 1 || nat.probe2_distinct_mappings > 1;
        if (!nat.mapping_stable || unstable_mapping) {
            nat.nat_risk = "high";
            nat.nat_type = "symmetric_nat";
        } else {
            if (nat.filter_same_ip_diff_port_rx && nat.filter_diff_ip_rx) {
                nat.nat_risk = "low";
                nat.nat_type = "full_cone_nat";
            } else if (nat.filter_same_ip_diff_port_rx && !nat.filter_diff_ip_rx) {
                nat.nat_risk = "medium";
                nat.nat_type = "ip_restricted_nat";
            } else {
                nat.nat_risk = "high";
                nat.nat_type = "port_restricted_nat";
            }
        }

        if (nat.probe1_success_count < 2 || nat.probe2_success_count < 2) {
            nat.nat_risk = "medium";
            if (nat.nat_type == "full_cone_nat") {
                nat.nat_type = "full_cone_nat_unstable_network";
            }
        }
    } else {
        nat.mapping_stable = false;
        nat.nat_risk = "high";
        nat.nat_type = "partial_udp_reachability";
    }

    printf("NAT detect #1 (%s:%u): local=%s:%u srflx=%s:%u\n",
        stun1_host.c_str(), stun1_port, nat.local_ip.c_str(), nat.local_port, nat.srflx_ip.c_str(), nat.srflx_port);
    printf("NAT probe #1: ok=%s rtt_ms=%d success=%d/%d distinct=%d\n",
        nat.probe1_ok ? "true" : "false",
        nat.probe1_rtt_ms,
        nat.probe1_success_count,
        nat.probe_rounds,
        nat.probe1_distinct_mappings);
    if (has_stun2) {
        printf("NAT detect #2 (%s:%u): srflx=%s:%u stable=%s risk=%s\n",
            stun2_host.c_str(),
            stun2_port,
            nat.srflx_ip_2.c_str(),
            nat.srflx_port_2,
            nat.mapping_stable ? "true" : "false",
            nat.nat_risk.c_str());
        printf("NAT probe #2: ok=%s rtt_ms=%d success=%d/%d distinct=%d\n",
            nat.probe2_ok ? "true" : "false",
            nat.probe2_rtt_ms,
            nat.probe2_success_count,
            nat.probe_rounds,
            nat.probe2_distinct_mappings);
    } else {
        printf("NAT detect: single STUN only, risk=%s\n", nat.nat_risk.c_str());
    }
    printf("NAT summary: type=%s risk=%s\n", nat.nat_type.c_str(), nat.nat_risk.c_str());
    if (has_stun2) {
        printf("NAT filter test: same_ip_diff_port_rx=%s diff_ip_rx=%s\n",
            nat.filter_same_ip_diff_port_rx ? "true" : "false",
            nat.filter_diff_ip_rx ? "true" : "false");
    }

    std::string line;
    eular::ntrs::Message msg;

    if (probe_only) {
        close(probe_sock);
        close(fd);
        return 0;
    }

    msg.type = eular::ntrs::MessageType::AUTH_REQ;
    msg.request_id = g_req++;
    msg.fields["peer_id"] = peer_id;
    msg.fields["token"] = "demo";
    send_line(fd, eular::ntrs::encodeMessage(msg));
    recv_line(fd, &line);
    printf("RX: %s\n", line.c_str());

    msg = eular::ntrs::Message();
    msg.type = eular::ntrs::MessageType::REGISTER_REQ;
    msg.request_id = g_req++;
    msg.fields["peer_id"] = peer_id;
    msg.fields["device_id"] = device_id;
    msg.fields["local_ip"] = nat.local_ip;
    msg.fields["local_port"] = std::to_string(nat.local_port);
    msg.fields["srflx_ip"] = nat.srflx_ip;
    msg.fields["srflx_port"] = std::to_string(nat.srflx_port);
    msg.fields["srflx_ip_2"] = nat.srflx_ip_2;
    msg.fields["srflx_port_2"] = std::to_string(nat.srflx_port_2);
    msg.fields["mapping_stable"] = nat.mapping_stable ? "true" : "false";
    msg.fields["nat_risk"] = nat.nat_risk;
    msg.fields["probe1_ok"] = nat.probe1_ok ? "true" : "false";
    msg.fields["probe2_ok"] = nat.probe2_ok ? "true" : "false";
    msg.fields["probe1_rtt_ms"] = std::to_string(nat.probe1_rtt_ms);
    msg.fields["probe2_rtt_ms"] = std::to_string(nat.probe2_rtt_ms);
    msg.fields["probe_rounds"] = std::to_string(nat.probe_rounds);
    msg.fields["probe1_success_count"] = std::to_string(nat.probe1_success_count);
    msg.fields["probe2_success_count"] = std::to_string(nat.probe2_success_count);
    msg.fields["probe1_distinct_mappings"] = std::to_string(nat.probe1_distinct_mappings);
    msg.fields["probe2_distinct_mappings"] = std::to_string(nat.probe2_distinct_mappings);
    msg.fields["filter_same_ip_diff_port_rx"] = nat.filter_same_ip_diff_port_rx ? "true" : "false";
    msg.fields["filter_diff_ip_rx"] = nat.filter_diff_ip_rx ? "true" : "false";
    msg.fields["nat_type"] = nat.nat_type;
    send_line(fd, eular::ntrs::encodeMessage(msg));
    recv_line(fd, &line);
    printf("RX: %s\n", line.c_str());

    if (!dst_peer.empty()) {
        msg = eular::ntrs::Message();
        msg.type = eular::ntrs::MessageType::SESSION_CREATE_REQ;
        msg.request_id = g_req++;
        msg.fields["src_peer_id"] = peer_id;
        msg.fields["dst_peer_id"] = dst_peer;
        send_line(fd, eular::ntrs::encodeMessage(msg));
        recv_line(fd, &line);
        printf("RX: %s\n", line.c_str());

        eular::ntrs::Message session_rsp;
        if (eular::ntrs::decodeMessage(line, &session_rsp) &&
            session_rsp.type == eular::ntrs::MessageType::SESSION_CREATE_RSP) {
            try_hole_punch_from_signal(probe_sock, session_rsp, "session_create_rsp");
        }
    }

    for (int i = 0; i < 6; ++i) {
        msg = eular::ntrs::Message();
        msg.type = eular::ntrs::MessageType::HEARTBEAT_REQ;
        msg.request_id = g_req++;
        msg.fields["peer_id"] = peer_id;
        msg.fields["lease_seq"] = std::to_string(i + 1);
        send_line(fd, eular::ntrs::encodeMessage(msg));

        bool got_hb_rsp = false;
        for (int wait_round = 0; wait_round < 4; ++wait_round) {
            if (!recv_line_timeout(fd, 400, &line)) {
                continue;
            }

            eular::ntrs::Message rx;
            if (!eular::ntrs::decodeMessage(line, &rx)) {
                printf("RX undecodable: %s\n", line.c_str());
                continue;
            }

            if (rx.type == eular::ntrs::MessageType::SESSION_NOTIFY) {
                printf("RX: %s\n", line.c_str());
                try_hole_punch_from_signal(probe_sock, rx, "session_notify");
                continue;
            }

            if (rx.type == eular::ntrs::MessageType::HEARTBEAT_RSP && rx.request_id == msg.request_id) {
                printf("RX: %s\n", line.c_str());
                got_hb_rsp = true;
                break;
            }

            printf("RX(other): %s\n", line.c_str());
        }

        if (!got_hb_rsp) {
            printf("heartbeat rsp timeout req=%llu\n", (unsigned long long)msg.request_id);
        }
        sleep(5);
    }

    msg = eular::ntrs::Message();
    msg.type = eular::ntrs::MessageType::UNREGISTER_REQ;
    msg.request_id = g_req++;
    msg.fields["peer_id"] = peer_id;
    msg.fields["reason"] = "client_exit";
    send_line(fd, eular::ntrs::encodeMessage(msg));
    if (recv_line(fd, &line)) {
        printf("RX: %s\n", line.c_str());
    }

    close(probe_sock);
    close(fd);
    return 0;
}
