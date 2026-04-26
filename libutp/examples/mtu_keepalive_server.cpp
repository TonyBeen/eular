#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <event/loop.h>
#include <event/timer.h>

#include <utils/CLI11.hpp>

#include <utp/errno.h>
#include <utp/utp.h>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int)
{
    g_stop = 1;
}

struct ServerState {
    std::unordered_map<uint32_t, eular::utp::Connection::Ptr> conns;
};

} // namespace

int main(int argc, char **argv)
{
    std::string bindIp = "0.0.0.0";
    uint16_t bindPort = 9100;
    uint32_t reportMs = 1000;

    CLI::App app("UTP MTU+keepalive server example");
    app.add_option("--bind-ip", bindIp, "Bind IPv4 address")->check(CLI::ValidIPV4);
    app.add_option("--bind-port", bindPort, "Bind port")->check(CLI::Range(1025, 65535));
    app.add_option("--report-ms", reportMs, "Statistic print interval(ms)")->check(CLI::Range(200, 5000));
    CLI11_PARSE(app, argc, argv);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ev::EventLoop loop;

    eular::utp::Config cfg;
    cfg.enable_keepalive = true;
    cfg.keepalive_interval = 1000;
    cfg.keepalive_timeout = 1500;
    cfg.keepalive_probes = 5;
    cfg.max_idle_timeout = 15000;

    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_base = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_probe_interval = 1;
    cfg.mtu_probe_step = 16;
    cfg.mtu_probe_timeout = 3000;

    eular::utp::Context ctx(loop.loop(), &cfg);
    ServerState state;

    ev::EventTimer stopTimer;
    stopTimer.reset(loop.loop(), [&]() {
        if (g_stop) {
            std::cout << "[server] stop requested\n";
            loop.breakLoop();
            return;
        }
        stopTimer.start(100);
    });
    stopTimer.start(100);

    ev::EventTimer statTimer;
    statTimer.reset(loop.loop(), [&]() {
        for (const auto &entry : state.conns) {
            const auto &conn = entry.second;
            if (!conn) {
                continue;
            }
            const auto d = conn->description();
            const auto s = conn->statistic();
            std::cout << "[server][stat] scid=" << d.scid
                      << " peer=" << d.remoteHost << ":" << d.remotePort
                      << " pmtu=" << s.pmtu
                      << " rtt(us)=" << s.rtt
                      << " rttvar(us)=" << s.rttvar
                      << " bw(B/s)=" << s.bw_estimate
                      << " rx=" << s.rx_bytes
                      << " tx=" << s.tx_bytes
                      << " rtx=" << s.rtx_bytes
                      << "\n";
        }
        statTimer.start(reportMs);
    });
    statTimer.start(reportMs);

    ctx.setOnNewConnection([&ctx](const eular::utp::Context::NewConnectionInfo &info) {
        const int32_t acceptStatus = ctx.accept();
        if (acceptStatus != 0) {
            std::cerr << "[server] accept failed ip=" << info.remote_ip
                      << ":" << info.remote_port
                      << " err=" << utp_get_last_error() << "\n";
            return false;
        }
        return true;
    });

    ctx.setOnConnected([&](eular::utp::Connection::Ptr conn) {
        const auto d = conn->description();
        std::cout << "[server] connected scid=" << d.scid
                  << " dcid=" << d.dcid
                  << " peer=" << d.remoteHost << ":" << d.remotePort << "\n";
        state.conns[d.scid] = conn;

        conn->setOnIncomingStream([](eular::utp::Stream *stream) {
            stream->setOnReadable([stream]() {
                std::vector<uint8_t> buf(4096);
                for (;;) {
                    const int32_t n = stream->read(buf.data(), buf.size());
                    if (n > 0) {
                        const int32_t wn = stream->write(buf.data(), static_cast<size_t>(n), false);
                        if (wn < 0 && utp_get_last_error() != UTP_ERR_WOULD_BLOCK) {
                            std::cerr << "[server] echo write failed err=" << utp_get_last_error() << "\n";
                        }
                        continue;
                    }
                    if (n == 0) {
                        stream->close();
                    }
                    break;
                }
            });
        });

        conn->setOnError([scid = d.scid](const eular::utp::Connection::ConnectionErrorInfo &info) {
            std::cerr << "[server] conn error scid=" << scid
                      << " code=" << info.error_code
                      << " reason=" << info.error_reason
                      << " fatal=" << (info.fatal ? 1 : 0) << "\n";
        });
    });

    ctx.setOnConnectionClosed([&](eular::utp::Connection::Ptr conn) {
        const auto d = conn->description();
        std::cout << "[server] closed scid=" << d.scid
                  << " peer=" << d.remoteHost << ":" << d.remotePort << "\n";
        state.conns.erase(d.scid);
    });

    ctx.setOnConnectError([](int32_t code, const std::string &reason, eular::utp::Context::ConnectAttemptInfo info) {
        std::cerr << "[server] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
    });

    if (ctx.bind(bindIp, bindPort) != 0) {
        std::cerr << "[server] bind failed err=" << utp_get_last_error() << "\n";
        return 1;
    }

    std::cout << "[server] listening " << bindIp << ":" << bindPort
              << " (keepalive+dplpmtud enabled)\n";

    loop.dispatch();
    return 0;
}
