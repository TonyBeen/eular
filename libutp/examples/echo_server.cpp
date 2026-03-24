#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <event/base.h>
#include <event/loop.h>
#include <event/timer.h>

#include <utils/CLI11.hpp>

#include <utp/errno.h>
#include <utp/utp.h>

namespace {

ev::EventLoop loop;

std::string PeerKey(const std::string &ip, uint16_t port)
{
    return ip + ":" + std::to_string(port);
}

void OnSignal(int)
{
    std::cout << "\n[server] signal received, shutting down...\n";
    std::exit(0);
}

void PrintUsage(const char *argv0)
{
    std::cout << "Usage: " << argv0 << " [--bind-ip 0.0.0.0] [--bind-port 9000]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string bindIp = "0.0.0.0";
    uint16_t bindPort = 9000;

    CLI::App app{"UTP Echo Server"};
    app.add_option("--bind-ip", bindIp, "IP address to bind")->check(CLI::ValidIPV4);
    app.add_option("--bind-port", bindPort, "Port to bind")->check(CLI::Range(5000, 65535));
    CLI11_PARSE(app, argc, argv);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ev::EventLoop loop;
    eular::utp::Config cfg;
    cfg.handshake_timeout = 5000;
    cfg.enable_keepalive = false;
    cfg.enable_dplpmtud = false;
    eular::utp::Context ctx(loop.loop(), &cfg);
    std::unordered_set<std::string> zeroRttAcceptedPeers;

    ctx.setOnNewConnection([&ctx](const eular::utp::Context::NewConnectionInfo &info) {
        const bool zeroRttPath = info.local_cid == 0;
        if (zeroRttPath) {
            std::cout << "[server] incoming 0-rtt request from "
                      << info.remote_ip << ":" << info.remote_port
                      << ", peer_cid=" << info.peer_cid << "\n";
            return true;
        }

        std::cout << "[server] incoming handshake request from "
                  << info.remote_ip << ":" << info.remote_port
                  << ", local_cid=" << info.local_cid
                  << ", peer_cid=" << info.peer_cid << "\n";

        const int32_t acceptStatus = ctx.accept();
        if (acceptStatus != UTP_ERR_OK) {
            std::cerr << "[server] accept failed for local_cid=" << info.local_cid
                      << ": " << acceptStatus << "\n";
            return false;
        }

        std::cout << "[server] accepted handshake local_cid=" << info.local_cid << "\n";
        return true;
    });

    ctx.setOnZeroRttDecision([&](const eular::utp::Context::ZeroRttDecisionInfo &info) {
        const std::string peer = PeerKey(info.remote_ip, info.remote_port);
        if (info.accepted) {
            zeroRttAcceptedPeers.insert(peer);
            std::cout << "[server] 0-rtt accepted from "
                      << peer
                      << ", peer_cid=" << info.peer_cid
                      << ", reason=" << info.reason << "\n";
            return;
        }

        zeroRttAcceptedPeers.erase(peer);
        std::cout << "[server] 0-rtt rejected from "
                  << peer
                  << ", peer_cid=" << info.peer_cid
                  << ", reason=" << info.reason << "\n";
    });

    ctx.setOnConnected([&](eular::utp::Connection::Ptr conn) {
        const auto desc = conn->description();
        const std::string peer = PeerKey(desc.remoteHost, desc.remotePort);
        const bool zeroRttPath = zeroRttAcceptedPeers.find(peer) != zeroRttAcceptedPeers.end();

        std::cout << "[server] connected via "
                  << (zeroRttPath ? "0-rtt" : "handshake")
                  << " scid=" << desc.scid
                  << " dcid=" << desc.dcid
                  << " peer=" << peer << "\n";

        zeroRttAcceptedPeers.erase(peer);

        conn->registerStreamCreated([](eular::utp::Stream *stream) {
            std::cout << "[server] stream created id=" << stream->id() << "\n";
            stream->setOnReadable([stream]() {
                std::vector<uint8_t> buffer(2048);
                for (;;) {
                    const int32_t n = stream->read(buffer.data(), buffer.size());
                    if (n > 0) {
                        const std::string msg(reinterpret_cast<const char *>(buffer.data()),
                                              static_cast<size_t>(n));
                        std::cout << "[server] recv stream=" << stream->id() << " msg=\"" << msg << "\"\n";
                        (void)stream->write(buffer.data(), static_cast<size_t>(n), false);
                        continue;
                    }

                    if (n == 0) {
                        // Peer sent FIN and no buffered data remains.
                        stream->close();
                    }
                    break;
                }
            });
        });
    });

    ctx.setOnConnectError([](int32_t code, const std::string &reason, eular::utp::Context::ConnectInfo info) {
        std::cerr << "[server] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
    });

    ctx.setOnConnectionClosed([](eular::utp::Connection::Ptr conn) {
        const auto desc = conn->description();
        std::cout << "[server] connection closed scid=" << desc.scid
                  << " peer=" << desc.remoteHost << ":" << desc.remotePort << "\n";
    });

    const int32_t bindStatus = ctx.bind(bindIp, bindPort);
    if (bindStatus != UTP_ERR_OK) {
        std::cerr << "[server] bind failed: " << bindStatus << "\n";
        return 1;
    }

    std::cout << "[server] listening on " << bindIp << ":" << bindPort << "\n";

    loop.dispatch();
    std::cout << "[server] shutdown\n";
    return 0;
}
