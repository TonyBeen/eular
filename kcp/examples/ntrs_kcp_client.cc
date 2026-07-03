#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <event2/event.h>

#include <kcpp.h>
#include <kcp_error.h>
#include <kcp_log.h>

#ifndef OS_LINUX
#error "This example requires a Linux environment."
#endif

struct NtrsMessage {
    std::string type;
    uint64_t request_id;
    std::map<std::string, std::string> fields;
};

static uint64_t g_req = 1;
static struct event_base *g_ev_base = NULL;
static struct event *g_timer_event = NULL;
static int g_tx_count = 0;
static const int kPacketCount = 20;

static std::vector<std::string> split(const std::string &s, char delim)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    out.push_back(cur);
    return out;
}

static std::string encode_message(const NtrsMessage &msg)
{
    std::string line = "TYPE=" + msg.type + "|REQ=" + std::to_string((unsigned long long)msg.request_id);
    for (std::map<std::string, std::string>::const_iterator it = msg.fields.begin(); it != msg.fields.end(); ++it) {
        line += "|" + it->first + "=" + it->second;
    }
    line += "\n";
    return line;
}

static bool decode_message(const std::string &line, NtrsMessage *msg)
{
    if (msg == NULL) {
        return false;
    }

    msg->type.clear();
    msg->request_id = 0;
    msg->fields.clear();

    std::vector<std::string> kvs = split(line, '|');
    for (size_t i = 0; i < kvs.size(); ++i) {
        size_t eq = kvs[i].find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }

        std::string key = kvs[i].substr(0, eq);
        std::string value = kvs[i].substr(eq + 1);
        if (key == "TYPE") {
            msg->type = value;
        } else if (key == "REQ") {
            msg->request_id = (uint64_t)strtoull(value.c_str(), NULL, 10);
        } else {
            msg->fields[key] = value;
        }
    }

    return !msg->type.empty();
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

        char ch = 0;
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

static int connect_ntrs(const std::string &ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static std::string detect_local_ip_from_tcp(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) != NULL) {
            return std::string(buf);
        }
    }
    return "127.0.0.1";
}

static bool expect_rsp_type(int fd, const std::string &type, NtrsMessage *out)
{
    std::string line;
    if (!recv_line_timeout(fd, 3000, &line)) {
        return false;
    }

    NtrsMessage msg;
    if (!decode_message(line, &msg)) {
        return false;
    }

    if (msg.type != type) {
        fprintf(stderr, "unexpected ntrs rsp type: %s (want %s)\n", msg.type.c_str(), type.c_str());
        return false;
    }

    if (out) {
        *out = msg;
    }
    return true;
}

static bool parse_peer_endpoint(const NtrsMessage &msg, std::string *ip, uint16_t *port)
{
    std::map<std::string, std::string>::const_iterator it_ip = msg.fields.find("peer_srflx_ip");
    std::map<std::string, std::string>::const_iterator it_port = msg.fields.find("peer_srflx_port");
    if (it_ip == msg.fields.end() || it_port == msg.fields.end()) {
        return false;
    }

    int p = atoi(it_port->second.c_str());
    if (p <= 0 || p > 65535) {
        return false;
    }

    *ip = it_ip->second;
    *port = (uint16_t)p;
    return !ip->empty();
}

static void on_kcp_error(struct KcpContext *kcp_ctx, struct KcpConnection *kcp_connection, int32_t code)
{
    (void)kcp_ctx;
    fprintf(stderr, "KCP error code=%d conn=%p\n", code, kcp_connection);
    if (kcp_connection) {
        kcp_close(kcp_connection);
    }
    if (g_ev_base) {
        event_base_loopbreak(g_ev_base);
    }
}

static void on_kcp_closed(struct KcpConnection *kcp_connection, int32_t code)
{
    char address[SOCKADDR_STRING_LEN] = {0};
    const char *addr_str = kcp_connection_remote_address(kcp_connection, address, sizeof(address));
    printf("KCP closed code=%d remote=%s\n", code, addr_str ? addr_str : "-");

    if (g_timer_event) {
        event_free(g_timer_event);
        g_timer_event = NULL;
    }

    if (g_ev_base) {
        event_base_loopbreak(g_ev_base);
    }
}

static void on_kcp_read_event(struct KcpConnection *kcp_connection, int32_t size)
{
    std::vector<char> buf((size_t)size + 1, 0);
    int32_t n = kcp_recv(kcp_connection, &buf[0], (size_t)size);
    if (n > 0) {
        printf("[KCP RX] %.*s\n", n, &buf[0]);
    }
}

static void on_kcp_timer(int fd, short ev, void *user)
{
    (void)fd;
    (void)ev;

    struct KcpConnection *conn = (struct KcpConnection *)user;
    if (conn == NULL) {
        return;
    }

    if (g_tx_count >= kPacketCount) {
        kcp_close(conn);
        return;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "NTRS+KCP same-process packet %d", g_tx_count + 1);
    if (kcp_send(conn, payload, strlen(payload)) != NO_ERROR) {
        fprintf(stderr, "kcp_send failed\n");
        kcp_close(conn);
        return;
    }
    printf("[KCP TX] %s\n", payload);
    g_tx_count++;

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    evtimer_add(g_timer_event, &tv);
}

static void on_kcp_connected(struct KcpConnection *kcp_connection, int32_t code)
{
    if (code != NO_ERROR || kcp_connection == NULL) {
        fprintf(stderr, "kcp connect failed: %d\n", code);
        event_base_loopbreak(g_ev_base);
        return;
    }

    printf("KCP connected: %p\n", kcp_connection);
    kcp_set_read_event_cb(kcp_connection, on_kcp_read_event);

    struct KcpConfig cfg = KCP_CONFIG_FAST;
    kcp_configure(kcp_connection, CONFIG_KEY_ALL, &cfg);

    uint32_t timeout = 1000;
    kcp_ioctl(kcp_connection, IOCTL_RECEIVE_TIMEOUT, &timeout);

    g_timer_event = evtimer_new(g_ev_base, on_kcp_timer, kcp_connection);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    evtimer_add(g_timer_event, &tv);
}

static bool ntrs_auth_register(int fd,
                               const std::string &peer_id,
                               const std::string &device_id,
                               const std::string &local_ip,
                               uint16_t local_port)
{
    NtrsMessage req;
    req.type = "AUTH_REQ";
    req.request_id = g_req++;
    req.fields["peer_id"] = peer_id;
    req.fields["token"] = "demo";
    if (!send_line(fd, encode_message(req))) {
        return false;
    }

    NtrsMessage rsp;
    if (!expect_rsp_type(fd, "AUTH_RSP", &rsp)) {
        return false;
    }

    req = NtrsMessage();
    req.type = "REGISTER_REQ";
    req.request_id = g_req++;
    req.fields["peer_id"] = peer_id;
    req.fields["device_id"] = device_id;
    req.fields["local_ip"] = local_ip;
    req.fields["local_port"] = std::to_string(local_port);
    req.fields["srflx_ip"] = local_ip;
    req.fields["srflx_port"] = std::to_string(local_port);
    req.fields["srflx_ip_2"] = local_ip;
    req.fields["srflx_port_2"] = std::to_string(local_port);
    req.fields["mapping_stable"] = "true";
    req.fields["nat_risk"] = "medium";
    req.fields["probe1_ok"] = "true";
    req.fields["probe2_ok"] = "false";
    req.fields["probe1_rtt_ms"] = "1";
    req.fields["probe2_rtt_ms"] = "-1";
    req.fields["probe_rounds"] = "1";
    req.fields["probe1_success_count"] = "1";
    req.fields["probe2_success_count"] = "0";
    req.fields["probe1_distinct_mappings"] = "1";
    req.fields["probe2_distinct_mappings"] = "0";
    req.fields["filter_same_ip_diff_port_rx"] = "false";
    req.fields["filter_diff_ip_rx"] = "false";
    req.fields["nat_type"] = "unknown";

    if (!send_line(fd, encode_message(req))) {
        return false;
    }

    if (!expect_rsp_type(fd, "REGISTER_RSP", &rsp)) {
        return false;
    }

    return true;
}

static bool ntrs_wait_peer_endpoint(int fd,
                                    const std::string &peer_id,
                                    const std::string &dst_peer,
                                    std::string *out_ip,
                                    uint16_t *out_port)
{
    if (!dst_peer.empty()) {
        NtrsMessage req;
        req.type = "SESSION_CREATE_REQ";
        req.request_id = g_req++;
        req.fields["src_peer_id"] = peer_id;
        req.fields["dst_peer_id"] = dst_peer;
        if (!send_line(fd, encode_message(req))) {
            return false;
        }

        NtrsMessage rsp;
        if (!expect_rsp_type(fd, "SESSION_CREATE_RSP", &rsp)) {
            return false;
        }

        if (parse_peer_endpoint(rsp, out_ip, out_port)) {
            printf("Session create got peer endpoint: %s:%u\n", out_ip->c_str(), (unsigned int)*out_port);
            return true;
        }
    }

    // Passive side waits for SESSION_NOTIFY.
    for (int wait_sec = 0; wait_sec < 120; ++wait_sec) {
        if ((wait_sec % 5) == 0) {
            NtrsMessage hb;
            hb.type = "HEARTBEAT_REQ";
            hb.request_id = g_req++;
            hb.fields["peer_id"] = peer_id;
            hb.fields["lease_seq"] = std::to_string(wait_sec / 5 + 1);
            send_line(fd, encode_message(hb));
        }

        std::string line;
        if (!recv_line_timeout(fd, 1000, &line)) {
            continue;
        }

        NtrsMessage msg;
        if (!decode_message(line, &msg)) {
            continue;
        }

        if (msg.type == "SESSION_NOTIFY" || msg.type == "SESSION_CREATE_RSP") {
            if (parse_peer_endpoint(msg, out_ip, out_port)) {
                printf("Session signal got peer endpoint: %s:%u\n", out_ip->c_str(), (unsigned int)*out_port);
                return true;
            }
        }
    }

    return false;
}

static void ntrs_unregister_best_effort(int fd, const std::string &peer_id)
{
    NtrsMessage req;
    req.type = "UNREGISTER_REQ";
    req.request_id = g_req++;
    req.fields["peer_id"] = peer_id;
    req.fields["reason"] = "test_done";
    send_line(fd, encode_message(req));
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        printf("Usage: %s <ntrs_ip> <ntrs_port> <peer_id> <device_id> <kcp_local_port> <dst_peer|->\n", argv[0]);
        printf("Example caller : %s 127.0.0.1 19000 peer_a dev_a 30001 peer_b\n", argv[0]);
        printf("Example callee : %s 127.0.0.1 19000 peer_b dev_b 30002 -\n", argv[0]);
        return 1;
    }

    std::string ntrs_ip = argv[1];
    int ntrs_port = atoi(argv[2]);
    std::string peer_id = argv[3];
    std::string device_id = argv[4];
    int local_port_i = atoi(argv[5]);
    std::string dst_peer = argv[6];
    if (dst_peer == "-") {
        dst_peer.clear();
    }

    if (local_port_i <= 0 || local_port_i > 65535) {
        fprintf(stderr, "invalid kcp local port: %d\n", local_port_i);
        return 1;
    }
    uint16_t local_port = (uint16_t)local_port_i;

    kcp_log_level(LOG_LEVEL_DEBUG);

    int ntrs_fd = connect_ntrs(ntrs_ip, ntrs_port);
    if (ntrs_fd < 0) {
        perror("connect ntrs");
        return 1;
    }

    std::string local_ip = detect_local_ip_from_tcp(ntrs_fd);
    printf("ntrs connected. local_ip=%s\n", local_ip.c_str());

    if (!ntrs_auth_register(ntrs_fd, peer_id, device_id, local_ip, local_port)) {
        fprintf(stderr, "ntrs auth/register failed\n");
        close(ntrs_fd);
        return 1;
    }

    std::string peer_ip;
    uint16_t peer_port = 0;
    if (!ntrs_wait_peer_endpoint(ntrs_fd, peer_id, dst_peer, &peer_ip, &peer_port)) {
        fprintf(stderr, "failed to get peer endpoint from ntrs\n");
        ntrs_unregister_best_effort(ntrs_fd, peer_id);
        close(ntrs_fd);
        return 1;
    }

    sockaddr_t local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin.sin_family = AF_INET;
    local_addr.sin.sin_port = htons(local_port);
    local_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);

    sockaddr_t remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin.sin_family = AF_INET;
    remote_addr.sin.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &remote_addr.sin.sin_addr) != 1) {
        fprintf(stderr, "invalid peer ip: %s\n", peer_ip.c_str());
        ntrs_unregister_best_effort(ntrs_fd, peer_id);
        close(ntrs_fd);
        return 1;
    }

    g_ev_base = event_base_new();
    if (!g_ev_base) {
        fprintf(stderr, "event_base_new failed\n");
        ntrs_unregister_best_effort(ntrs_fd, peer_id);
        close(ntrs_fd);
        return 1;
    }

    struct KcpContext *ctx = kcp_context_create(g_ev_base, on_kcp_error, NULL);
    if (!ctx) {
        fprintf(stderr, "kcp_context_create failed\n");
        event_base_free(g_ev_base);
        ntrs_unregister_best_effort(ntrs_fd, peer_id);
        close(ntrs_fd);
        return 1;
    }

    if (kcp_bind(ctx, &local_addr, NULL) != NO_ERROR) {
        fprintf(stderr, "kcp_bind failed on port %u\n", (unsigned int)local_port);
        kcp_context_destroy(ctx);
        event_base_free(g_ev_base);
        ntrs_unregister_best_effort(ntrs_fd, peer_id);
        close(ntrs_fd);
        return 1;
    }

    kcp_set_close_cb(ctx, on_kcp_closed);
    if (kcp_connect(ctx, &remote_addr, 1500, on_kcp_connected) != NO_ERROR) {
        fprintf(stderr, "kcp_connect failed\n");
        kcp_context_destroy(ctx);
        event_base_free(g_ev_base);
        ntrs_unregister_best_effort(ntrs_fd, peer_id);
        close(ntrs_fd);
        return 1;
    }

    printf("KCP start: local=%u remote=%s:%u\n", (unsigned int)local_port, peer_ip.c_str(), (unsigned int)peer_port);
    event_base_dispatch(g_ev_base);

    ntrs_unregister_best_effort(ntrs_fd, peer_id);
    close(ntrs_fd);

    kcp_context_destroy(ctx);
    event_base_free(g_ev_base);
    g_ev_base = NULL;

    return 0;
}
