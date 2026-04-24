#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <ctime>
#include <errno.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <ntrs_codec.h>
#include <mqtt_client.h>
#include <stun.h>
#include <stun_types.h>
#include <socket_address.h>
#include "../3rd_party/include/stun/msg.h"

struct PeerSession {
    std::string peer_id;
    std::string device_id;
    std::string local_ip;
    uint16_t local_port;
    std::string srflx_ip;
    uint16_t srflx_port;
    std::string srflx_ip_2;
    uint16_t srflx_port_2;
    std::string nat_type;
    int fd;
    time_t expire_at;
};

static const int kLeaseSec = 30;

static bool parse_endpoint(const std::string &input, std::string *host, uint16_t *port)
{
    size_t pos = input.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos == input.size() - 1) {
        return false;
    }

    int parsed = atoi(input.substr(pos + 1).c_str());
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }

    *host = input.substr(0, pos);
    *port = (uint16_t)parsed;
    return true;
}

static uint16_t endpoint_port(const std::string &endpoint)
{
    std::string host;
    uint16_t port = 0;
    if (!parse_endpoint(endpoint, &host, &port)) {
        return 0;
    }
    return port;
}

static std::string now_iso8601()
{
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

static void append_unique(std::vector<std::string> *out, const std::string &value)
{
    if (value.empty()) {
        return;
    }
    for (size_t i = 0; i < out->size(); ++i) {
        if (out->at(i) == value) {
            return;
        }
    }
    out->push_back(value);
}

static std::vector<std::string> build_probe_controls(const std::string &p1,
                                                     const std::string &p2,
                                                     const std::string &b1,
                                                     const std::string &fallback)
{
    std::vector<std::string> out;
    append_unique(&out, p1);
    append_unique(&out, p2);
    append_unique(&out, b1);
    append_unique(&out, fallback);
    return out;
}

static void erase_peer_for_fd(std::map<std::string, PeerSession> *peers, int fd)
{
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end();) {
        if (it->second.fd == fd) {
            printf("peer offline: %s\n", it->first.c_str());
            std::map<std::string, PeerSession>::iterator to_erase = it++;
            peers->erase(to_erase);
        } else {
            ++it;
        }
    }
}

static void sweep_expired_peers(std::map<std::string, PeerSession> *peers)
{
    time_t now = time(NULL);
    for (std::map<std::string, PeerSession>::iterator it = peers->begin(); it != peers->end();) {
        if (it->second.expire_at <= now) {
            printf("peer lease expired: %s\n", it->first.c_str());
            std::map<std::string, PeerSession>::iterator to_erase = it++;
            peers->erase(to_erase);
        } else {
            ++it;
        }
    }
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

static bool fetch_peer_stun_endpoint(const std::string &peer_control, std::string *stun_endpoint)
{
    std::string host;
    uint16_t port = 0;
    if (!parse_endpoint(peer_control, &host, &port)) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    eular::ntrs::Message req;
    req.type = eular::ntrs::MessageType::SERVER_INFO_REQ;
    req.request_id = 1;
    req.fields["query"] = "stun_endpoint";
    if (!send_line(fd, eular::ntrs::encodeMessage(req))) {
        close(fd);
        return false;
    }

    std::string line;
    if (!recv_line(fd, &line)) {
        close(fd);
        return false;
    }

    eular::ntrs::Message rsp;
    if (!eular::ntrs::decodeMessage(line, &rsp) || rsp.type != eular::ntrs::MessageType::SERVER_INFO_RSP) {
        close(fd);
        return false;
    }

    std::string endpoint = rsp.fields["stun_endpoint"];
    if (endpoint.empty()) {
        close(fd);
        return false;
    }

    *stun_endpoint = endpoint;
    close(fd);
    return true;
}

static int create_stun_socket(const std::string &self_stun_endpoint)
{
    uint16_t port = endpoint_port(self_stun_endpoint);
    if (port == 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int create_stun_socket_with_port(uint16_t port)
{
    if (port == 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool send_udp_filter_probe(int udp_fd,
                                  const std::string &target_ip,
                                  uint16_t target_port,
                                  const std::string &token,
                                  const std::string &tag)
{
    if (udp_fd < 0 || target_ip.empty() || target_port == 0 || token.empty()) {
        return false;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(target_port);
    if (inet_pton(AF_INET, target_ip.c_str(), &dst.sin_addr) != 1) {
        return false;
    }

    char payload[256];
    snprintf(payload, sizeof(payload), "NTRS_FILTER_PROBE|%s|%s", token.c_str(), tag.c_str());
    ssize_t n = sendto(udp_fd, payload, strlen(payload), 0, (struct sockaddr *)&dst, sizeof(dst));
    return n > 0;
}

static bool request_peer_send_probe(const std::string &peer_control,
                                    const std::string &target_ip,
                                    uint16_t target_port,
                                    const std::string &token)
{
    std::string host;
    uint16_t port = 0;
    if (!parse_endpoint(peer_control, &host, &port)) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    eular::ntrs::Message req;
    req.type = eular::ntrs::MessageType::SERVER_SEND_PROBE_REQ;
    req.request_id = 1;
    req.fields["target_ip"] = target_ip;
    req.fields["target_port"] = std::to_string(target_port);
    req.fields["token"] = token;
    if (!send_line(fd, eular::ntrs::encodeMessage(req))) {
        close(fd);
        return false;
    }

    std::string line;
    if (!recv_line(fd, &line)) {
        close(fd);
        return false;
    }

    eular::ntrs::Message rsp;
    bool ok = eular::ntrs::decodeMessage(line, &rsp) &&
              rsp.type == eular::ntrs::MessageType::SERVER_SEND_PROBE_RSP &&
              rsp.fields["result"] == "ok";
    close(fd);
    return ok;
}

static void handle_stun_packet(int stun_fd)
{
    uint8_t buf[2048];
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    ssize_t n = recvfrom(stun_fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer_addr, &peer_len);
    if (n <= 0) {
        return;
    }

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer_addr.sin_addr, ip, sizeof(ip));

    eular::stun::StunMsgParser parser;
    if (!parser.parse(buf, (size_t)n)) {
        return;
    }

    if (parser.msgType() != ENUM_CLASS(eular::stun::StunMsgType::STUN_BINDING_REQUEST)) {
        return;
    }

    const uint8_t *trx = parser.transactionId();
    if (trx == NULL) {
        return;
    }

    uint8_t rsp_buf[256];
    memset(rsp_buf, 0, sizeof(rsp_buf));

    stun_msg_hdr *rsp_hdr = (stun_msg_hdr *)rsp_buf;
    stun_msg_hdr_init(rsp_hdr, STUN_BINDING_RESPONSE, trx);
    if (stun_attr_xor_sockaddr_add(rsp_hdr, STUN_ATTR_XOR_MAPPED_ADDRESS, (const struct sockaddr *)&peer_addr) != STUN_OK) {
        printf("STUN build response failed\n");
        return;
    }

    size_t rsp_len = stun_msg_len(rsp_hdr);
    ssize_t sent = sendto(stun_fd, rsp_buf, rsp_len, 0, (struct sockaddr *)&peer_addr, peer_len);
    if (sent <= 0) {
        printf("STUN rsp send failed errno=%d(%s)\n", errno, strerror(errno));
    }
}

static void send_error(int fd, uint64_t req, const char *code, const char *message)
{
    eular::ntrs::Message rsp;
    rsp.type = eular::ntrs::MessageType::ERROR_RSP;
    rsp.request_id = req;
    rsp.fields["code"] = code;
    rsp.fields["message"] = message;
    send_line(fd, eular::ntrs::encodeMessage(rsp));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    int port = 19000;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    std::string self_stun_endpoint;
    if (argc > 2) {
        self_stun_endpoint = argv[2];
    }
    std::string peer_control_endpoint;
    if (argc > 3) {
        peer_control_endpoint = argv[3];
    }

    std::string mqtt_broker = (argc > 4) ? argv[4] : "";
    int mqtt_port = (argc > 5) ? atoi(argv[5]) : 1883;
    std::string node_id = (argc > 6) ? argv[6] : "";
    std::string region = (argc > 7) ? argv[7] : "default";
    std::string mqtt_username = (argc > 8) ? argv[8] : "";
    std::string mqtt_password = (argc > 9) ? argv[9] : "";

    if (self_stun_endpoint.empty()) {
        printf("Usage: %s <control_port> <self_stun_host:port> [peer_control_host:port] [mqtt_broker] [mqtt_port] [node_id] [region] [mqtt_username] [mqtt_password]\n", argv[0]);
        return 1;
    }

    bool use_hub_assignment = (!mqtt_broker.empty() && !node_id.empty());
    std::string assignment_p1;
    std::string assignment_p2;
    std::string assignment_b1;
    std::string assignment_version = "0";
    std::unique_ptr<eular::orion::MqttClient> mqtt_client;

    if (use_hub_assignment) {
        mqtt_client.reset(new eular::orion::MqttClient(
            mqtt_broker,
            mqtt_port,
            "ntrs-server-" + node_id + "-" + std::to_string((unsigned long long)getpid()),
            mqtt_username,
            mqtt_password
        ));

        eular::ntrs::Message will;
        will.type = eular::ntrs::MessageType::NODE_PRESENCE;
        will.request_id = 0;
        will.fields["node_id"] = node_id;
        will.fields["boot_id"] = std::to_string((unsigned long long)time(NULL)) + "-" + std::to_string((unsigned long long)getpid());
        will.fields["status"] = "offline";
        will.fields["reason"] = "lwt";
        will.fields["ts"] = now_iso8601();
        mqtt_client->setWillMessage("ntrs/node/" + node_id + "/presence", eular::ntrs::encodeMessage(will), 1, true);

        if (!mqtt_client->connect()) {
            printf("hub mode disabled because mqtt connect failed: %s:%d\n", mqtt_broker.c_str(), mqtt_port);
            mqtt_client.reset();
            use_hub_assignment = false;
        }
    }

    std::string self_stun_host;
    uint16_t self_stun_port = 0;
    if (!parse_endpoint(self_stun_endpoint, &self_stun_host, &self_stun_port)) {
        self_stun_host = "127.0.0.1";
    }

    std::string boot_id = std::to_string((unsigned long long)time(NULL)) + "-" + std::to_string((unsigned long long)getpid());
    if (use_hub_assignment && mqtt_client) {
        std::string assignment_topic = "ntrs/hub/node/" + node_id + "/assignment";
        mqtt_client->setMessageCallback([&](const std::string &topic, const std::string &payload) {
            eular::ntrs::Message msg;
            if (!eular::ntrs::decodeMessage(payload, &msg)) {
                return;
            }
            if (topic == assignment_topic &&
                msg.type == eular::ntrs::MessageType::HUB_CLUSTER_EVENT &&
                msg.fields["event"] == "assignment") {
                assignment_p1 = msg.fields["primary1_control"];
                assignment_p2 = msg.fields["primary2_control"];
                assignment_b1 = msg.fields["backup1_control"];
                assignment_version = std::to_string((unsigned long long)msg.request_id);
                printf("assignment update: v=%s p1=%s p2=%s b1=%s\n",
                    assignment_version.c_str(),
                    assignment_p1.c_str(),
                    assignment_p2.c_str(),
                    assignment_b1.c_str());
            }
        });

        mqtt_client->subscribe(assignment_topic, 1);

        eular::ntrs::Message reg;
        reg.type = eular::ntrs::MessageType::NODE_REGISTER;
        reg.request_id = 1;
        reg.fields["node_id"] = node_id;
        reg.fields["boot_id"] = boot_id;
        reg.fields["region"] = region;
        reg.fields["stun_endpoint"] = self_stun_endpoint;
        reg.fields["control_endpoint"] = self_stun_host + ":" + std::to_string(port);
        reg.fields["nat_type"] = "server";
        reg.fields["ts"] = now_iso8601();
        mqtt_client->publish("ntrs/node/" + node_id + "/register", eular::ntrs::encodeMessage(reg), 1, true);

        eular::ntrs::Message online;
        online.type = eular::ntrs::MessageType::NODE_PRESENCE;
        online.request_id = 2;
        online.fields["node_id"] = node_id;
        online.fields["boot_id"] = boot_id;
        online.fields["status"] = "online";
        online.fields["reason"] = "startup";
        online.fields["ts"] = now_iso8601();
        mqtt_client->publish("ntrs/node/" + node_id + "/presence", eular::ntrs::encodeMessage(online), 1, true);
    }

    int stun_fd = create_stun_socket(self_stun_endpoint);
    if (stun_fd < 0) {
        printf("failed to start built-in STUN on %s\n", self_stun_endpoint.c_str());
        return 1;
    }

    uint16_t stun_port = endpoint_port(self_stun_endpoint);
    int stun_alt_fd = -1;
    if (stun_port > 0 && stun_port < 65535) {
        stun_alt_fd = create_stun_socket_with_port((uint16_t)(stun_port + 1));
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    printf("NTRS M1 server listening on :%d self_stun=%s alt_stun_port=%d peer_control=%s (built-in STUN enabled)\n",
        port,
        self_stun_endpoint.c_str(),
        stun_alt_fd >= 0 ? (int)(stun_port + 1) : -1,
        peer_control_endpoint.empty() ? "-" : peer_control_endpoint.c_str());

    std::set<int> clients;
    std::map<int, std::string> recv_buf;
    std::map<std::string, PeerSession> peers;
    time_t last_hb_ts = 0;

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        FD_SET(stun_fd, &rfds);
        int maxfd = listen_fd;
        if (stun_fd > maxfd) {
            maxfd = stun_fd;
        }
        if (stun_alt_fd >= 0) {
            FD_SET(stun_alt_fd, &rfds);
            if (stun_alt_fd > maxfd) {
                maxfd = stun_alt_fd;
            }
        }
        for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
            FD_SET(*it, &rfds);
            if (*it > maxfd) {
                maxfd = *it;
            }
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select");
            continue;
        }

        sweep_expired_peers(&peers);

        if (use_hub_assignment && mqtt_client) {
            mqtt_client->poll(5);
            time_t now = time(NULL);
            if (now - last_hb_ts >= 5) {
                eular::ntrs::Message hb;
                hb.type = eular::ntrs::MessageType::NODE_HEARTBEAT;
                hb.request_id = (uint64_t)now;
                hb.fields["node_id"] = node_id;
                hb.fields["boot_id"] = boot_id;
                hb.fields["status"] = "online";
                hb.fields["load"] = std::to_string((int)clients.size());
                hb.fields["nat_type"] = "server";
                hb.fields["assignment_version"] = assignment_version;
                hb.fields["ts"] = now_iso8601();
                mqtt_client->publish("ntrs/node/" + node_id + "/heartbeat", eular::ntrs::encodeMessage(hb), 1, false);
                last_hb_ts = now;
            }
        }

        if (FD_ISSET(stun_fd, &rfds)) {
            handle_stun_packet(stun_fd);
        }
        if (stun_alt_fd >= 0 && FD_ISSET(stun_alt_fd, &rfds)) {
            handle_stun_packet(stun_alt_fd);
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                clients.insert(fd);
                recv_buf[fd] = "";
                printf("client connected fd=%d\n", fd);
            }
        }

        std::vector<int> closed;
        for (std::set<int>::iterator it = clients.begin(); it != clients.end(); ++it) {
            int fd = *it;
            if (!FD_ISSET(fd, &rfds)) {
                continue;
            }

            char buf[2048];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                closed.push_back(fd);
                continue;
            }

            recv_buf[fd].append(buf, (size_t)n);
            for (;;) {
                size_t pos = recv_buf[fd].find('\n');
                if (pos == std::string::npos) {
                    break;
                }

                std::string line = recv_buf[fd].substr(0, pos);
                recv_buf[fd].erase(0, pos + 1);

                eular::ntrs::Message msg;
                if (!eular::ntrs::decodeMessage(line, &msg)) {
                    send_error(fd, 0, "BAD_MESSAGE", "decode failed");
                    continue;
                }

                switch (msg.type) {
                case eular::ntrs::MessageType::AUTH_REQ: {
                    std::string token = msg.fields["token"];
                    if (token.empty()) {
                        send_error(fd, msg.request_id, "AUTH_FAILED", "token required");
                        break;
                    }

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::AUTH_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["result"] = "ok";
                    rsp.fields["lease_default_sec"] = "30";
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::REGISTER_REQ: {
                    std::string peer_id = msg.fields["peer_id"];
                    std::string device_id = msg.fields["device_id"];
                    if (peer_id.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }

                    PeerSession s;
                    s.peer_id = peer_id;
                    s.device_id = device_id;
                    s.local_ip = msg.fields["local_ip"];
                    s.local_port = (uint16_t)atoi(msg.fields["local_port"].c_str());
                    s.srflx_ip = msg.fields["srflx_ip"];
                    s.srflx_port = (uint16_t)atoi(msg.fields["srflx_port"].c_str());
                    s.srflx_ip_2 = msg.fields["srflx_ip_2"];
                    s.srflx_port_2 = (uint16_t)atoi(msg.fields["srflx_port_2"].c_str());
                    s.nat_type = msg.fields["nat_type"];
                    s.fd = fd;
                    s.expire_at = time(NULL) + kLeaseSec;
                    peers[peer_id] = s;

                    printf("REGISTER peer=%s device=%s local=%s:%s srflx=%s:%s srflx2=%s:%s stable=%s risk=%s type=%s probe1_ok=%s probe2_ok=%s probe1_rtt_ms=%s probe2_rtt_ms=%s rounds=%s p1succ=%s p2succ=%s p1distinct=%s p2distinct=%s f_same_ip_port=%s f_diff_ip=%s\n",
                        peer_id.c_str(),
                        device_id.c_str(),
                        msg.fields["local_ip"].c_str(),
                        msg.fields["local_port"].c_str(),
                        msg.fields["srflx_ip"].c_str(),
                        msg.fields["srflx_port"].c_str(),
                        msg.fields["srflx_ip_2"].c_str(),
                        msg.fields["srflx_port_2"].c_str(),
                        msg.fields["mapping_stable"].c_str(),
                        msg.fields["nat_risk"].c_str(),
                        msg.fields["nat_type"].c_str(),
                        msg.fields["probe1_ok"].c_str(),
                        msg.fields["probe2_ok"].c_str(),
                        msg.fields["probe1_rtt_ms"].c_str(),
                        msg.fields["probe2_rtt_ms"].c_str(),
                        msg.fields["probe_rounds"].c_str(),
                        msg.fields["probe1_success_count"].c_str(),
                        msg.fields["probe2_success_count"].c_str(),
                        msg.fields["probe1_distinct_mappings"].c_str(),
                        msg.fields["probe2_distinct_mappings"].c_str(),
                        msg.fields["filter_same_ip_diff_port_rx"].c_str(),
                        msg.fields["filter_diff_ip_rx"].c_str());

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::REGISTER_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["lease_sec"] = "30";
                    rsp.fields["heartbeat_interval_sec"] = "10";
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::HEARTBEAT_REQ: {
                    std::string peer_id = msg.fields["peer_id"];
                    if (peer_id.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator pit = peers.find(peer_id);
                    if (pit == peers.end() || pit->second.fd != fd) {
                        send_error(fd, msg.request_id, "NOT_REGISTERED", "peer not registered");
                        break;
                    }
                    pit->second.expire_at = time(NULL) + kLeaseSec;

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::HEARTBEAT_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["lease_sec"] = "30";
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::UNREGISTER_REQ: {
                    std::string peer_id = msg.fields["peer_id"];
                    if (peer_id.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "peer_id required");
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator pit = peers.find(peer_id);
                    if (pit != peers.end() && pit->second.fd == fd) {
                        printf("UNREGISTER peer=%s reason=%s\n",
                            peer_id.c_str(),
                            msg.fields["reason"].c_str());
                        peers.erase(pit);
                    }

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::UNREGISTER_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["result"] = "ok";
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::SESSION_CREATE_REQ: {
                    std::string src = msg.fields["src_peer_id"];
                    std::string dst = msg.fields["dst_peer_id"];
                    if (src.empty() || dst.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "src/dst peer required");
                        break;
                    }

                    std::map<std::string, PeerSession>::iterator src_it = peers.find(src);
                    std::map<std::string, PeerSession>::iterator dst_it = peers.find(dst);
                    if (src_it == peers.end() || src_it->second.fd != fd) {
                        send_error(fd, msg.request_id, "NOT_REGISTERED", "src peer not registered on this connection");
                        break;
                    }
                    if (dst_it == peers.end()) {
                        send_error(fd, msg.request_id, "DST_OFFLINE", "dst peer not online");
                        break;
                    }

                    char sid[64];
                    snprintf(sid, sizeof(sid), "sess_%s_%s", src.c_str(), dst.c_str());

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::SESSION_CREATE_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["session_id"] = sid;
                    rsp.fields["role"] = "initiator";
                    rsp.fields["token"] = "demo_token";
                    rsp.fields["expire_at"] = "60";
                    rsp.fields["peer_id"] = dst_it->second.peer_id;
                    rsp.fields["peer_srflx_ip"] = dst_it->second.srflx_ip;
                    rsp.fields["peer_srflx_port"] = std::to_string(dst_it->second.srflx_port);
                    rsp.fields["peer_srflx_ip_2"] = dst_it->second.srflx_ip_2;
                    rsp.fields["peer_srflx_port_2"] = std::to_string(dst_it->second.srflx_port_2);
                    rsp.fields["peer_nat_type"] = dst_it->second.nat_type;
                    send_line(fd, eular::ntrs::encodeMessage(rsp));

                    eular::ntrs::Message notify;
                    notify.type = eular::ntrs::MessageType::SESSION_NOTIFY;
                    notify.request_id = 0;
                    notify.fields["session_id"] = sid;
                    notify.fields["src_peer_id"] = src;
                    notify.fields["dst_peer_id"] = dst;
                    notify.fields["peer_id"] = src_it->second.peer_id;
                    notify.fields["peer_srflx_ip"] = src_it->second.srflx_ip;
                    notify.fields["peer_srflx_port"] = std::to_string(src_it->second.srflx_port);
                    notify.fields["peer_srflx_ip_2"] = src_it->second.srflx_ip_2;
                    notify.fields["peer_srflx_port_2"] = std::to_string(src_it->second.srflx_port_2);
                    notify.fields["peer_nat_type"] = src_it->second.nat_type;
                    send_line(dst_it->second.fd, eular::ntrs::encodeMessage(notify));
                    break;
                }
                case eular::ntrs::MessageType::NAT_PROBE_REQ: {
                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::NAT_PROBE_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["stun1"] = self_stun_endpoint;

                    std::string peer_stun;
                    std::vector<std::string> controls = build_probe_controls(
                        assignment_p1,
                        assignment_p2,
                        assignment_b1,
                        peer_control_endpoint
                    );

                    bool ok = false;
                    for (size_t i = 0; i < controls.size(); ++i) {
                        if (fetch_peer_stun_endpoint(controls[i], &peer_stun)) {
                            rsp.fields["selected_control"] = controls[i];
                            ok = true;
                            break;
                        }
                    }

                    if (ok) {
                        rsp.fields["stun2"] = peer_stun;
                        rsp.fields["federation"] = "ok";
                    } else {
                        rsp.fields["stun2"] = "";
                        rsp.fields["federation"] = "degraded";
                    }
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::FILTER_PROBE_REQ: {
                    std::string target_ip = msg.fields["target_ip"];
                    uint16_t target_port = (uint16_t)atoi(msg.fields["target_port"].c_str());
                    std::string token = msg.fields["token"];
                    if (target_ip.empty() || target_port == 0 || token.empty()) {
                        send_error(fd, msg.request_id, "INVALID_PARAM", "target_ip/target_port/token required");
                        break;
                    }

                    bool same_ip_diff_port = false;
                    if (stun_alt_fd >= 0) {
                        same_ip_diff_port = send_udp_filter_probe(stun_alt_fd, target_ip, target_port, token, "same_ip_diff_port");
                    }

                    bool diff_ip = false;
                    std::vector<std::string> controls = build_probe_controls(
                        assignment_p1,
                        assignment_p2,
                        assignment_b1,
                        peer_control_endpoint
                    );
                    for (size_t i = 0; i < controls.size(); ++i) {
                        if (request_peer_send_probe(controls[i], target_ip, target_port, token)) {
                            diff_ip = true;
                            break;
                        }
                    }

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::FILTER_PROBE_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["same_ip_diff_port_sent"] = same_ip_diff_port ? "true" : "false";
                    rsp.fields["diff_ip_sent"] = diff_ip ? "true" : "false";
                    rsp.fields["result"] = (same_ip_diff_port || diff_ip) ? "ok" : "degraded";
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::SERVER_INFO_REQ: {
                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::SERVER_INFO_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["stun_endpoint"] = self_stun_endpoint;
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                case eular::ntrs::MessageType::SERVER_SEND_PROBE_REQ: {
                    std::string target_ip = msg.fields["target_ip"];
                    uint16_t target_port = (uint16_t)atoi(msg.fields["target_port"].c_str());
                    std::string token = msg.fields["token"];
                    bool ok = send_udp_filter_probe(stun_fd, target_ip, target_port, token, "diff_ip");

                    eular::ntrs::Message rsp;
                    rsp.type = eular::ntrs::MessageType::SERVER_SEND_PROBE_RSP;
                    rsp.request_id = msg.request_id;
                    rsp.fields["result"] = ok ? "ok" : "failed";
                    send_line(fd, eular::ntrs::encodeMessage(rsp));
                    break;
                }
                default:
                    send_error(fd, msg.request_id, "UNSUPPORTED", "message unsupported in M1");
                    break;
                }
            }
        }

        for (size_t i = 0; i < closed.size(); ++i) {
            int fd = closed[i];
            close(fd);
            clients.erase(fd);
            recv_buf.erase(fd);
            erase_peer_for_fd(&peers, fd);
        }
    }

    return 0;
}
