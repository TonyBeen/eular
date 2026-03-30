#include <csignal>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
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

std::vector<uint8_t> MakePayload(size_t n)
{
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    return out;
}

uint64_t NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

int main(int argc, char **argv)
{
    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 9100;
    uint32_t totalBytes = 512 * 1024;
    uint32_t chunkBytes = 1200;
    uint32_t idleSeconds = 8;
    uint32_t reportMs = 1000;

    CLI::App app("UTP MTU+keepalive client example");
    app.add_option("--server-ip", serverIp, "Server IPv4")->check(CLI::ValidIPV4);
    app.add_option("--server-port", serverPort, "Server port")->check(CLI::Range(1025, 65535));
    app.add_option("--total-bytes", totalBytes, "Total bytes to send before idle")->check(CLI::Range(1024, 8 * 1024 * 1024));
    app.add_option("--chunk-bytes", chunkBytes, "Write chunk size")->check(CLI::Range(128, 4096));
    app.add_option("--idle-seconds", idleSeconds, "Idle seconds after payload to observe keepalive/mtu")->check(CLI::Range(1, 120));
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
    cfg.mtu_probe_timeout = 800;

    eular::utp::Context ctx(loop.loop(), &cfg);

    eular::utp::Connection::Ptr conn;
    eular::utp::Stream *stream = nullptr;

    bool connected = false;
    bool sendDone = false;
    uint64_t bytesSent = 0;
    uint64_t bytesEchoed = 0;
    uint64_t idleStartMs = 0;

    ev::EventTimer stopTimer;
    stopTimer.reset(loop.loop(), [&]() {
        if (g_stop) {
            std::cout << "[client] stop requested\n";
            loop.breakLoop();
            return;
        }
        stopTimer.start(100);
    });
    stopTimer.start(100);

    ev::EventTimer sendTimer;
    sendTimer.reset(loop.loop(), [&]() {
        if (!connected || stream == nullptr || sendDone) {
            return;
        }

        const uint32_t remain = static_cast<uint32_t>(totalBytes - std::min<uint64_t>(bytesSent, totalBytes));
        if (remain == 0) {
            sendDone = true;
            idleStartMs = NowMs();
            std::cout << "[client] payload done, entering idle stage for " << idleSeconds << "s\n";
            return;
        }

        const size_t n = std::min<size_t>(chunkBytes, remain);
        const std::vector<uint8_t> payload = MakePayload(n);
        const int32_t wn = stream->write(payload.data(), payload.size(), false);
        if (wn < 0) {
            const int32_t err = GetLastError();
            if (err == UTP_ERR_WOULD_BLOCK) {
                sendTimer.start(5);
                return;
            }
            std::cerr << "[client] write failed err=" << err << "\n";
            conn->close();
            return;
        }

        bytesSent += static_cast<uint64_t>(wn);
        sendTimer.start(1);
    });

    ev::EventTimer statTimer;
    statTimer.reset(loop.loop(), [&]() {
        if (conn) {
            const auto d = conn->description();
            const auto s = conn->statistic();
            std::cout << "[client][stat] scid=" << d.scid
                      << " pmtu=" << s.pmtu
                      << " rtt(us)=" << s.rtt
                      << " rttvar(us)=" << s.rttvar
                      << " bw(B/s)=" << s.bw_estimate
                      << " rx=" << s.rx_bytes
                      << " tx=" << s.tx_bytes
                      << " rtx=" << s.rtx_bytes
                      << " app_sent=" << bytesSent
                      << " app_echoed=" << bytesEchoed
                      << "\n";
        }

        if (sendDone && idleStartMs > 0) {
            const uint64_t nowMs = NowMs();
            if (nowMs >= idleStartMs + static_cast<uint64_t>(idleSeconds) * 1000) {
                std::cout << "[client] idle stage done, closing connection\n";
                if (conn) {
                    conn->close();
                }
                return;
            }
        }

        statTimer.start(reportMs);
    });

    ctx.setOnConnected([&](eular::utp::Connection::Ptr c) {
        connected = true;
        conn = c;

        const auto d = conn->description();
        std::cout << "[client] connected scid=" << d.scid
                  << " dcid=" << d.dcid
                  << " peer=" << d.remoteHost << ":" << d.remotePort << "\n";

        const int32_t sid = conn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
        if (sid < 0) {
            std::cerr << "[client] createStream failed err=" << GetLastError() << "\n";
            conn->close();
            return;
        }

        stream = conn->getStream(static_cast<uint32_t>(sid));
        if (!stream) {
            std::cerr << "[client] getStream failed sid=" << sid << "\n";
            conn->close();
            return;
        }

        stream->setOnReadable([&]() {
            std::vector<uint8_t> buf(4096);
            for (;;) {
                const int32_t n = stream->read(buf.data(), buf.size());
                if (n > 0) {
                    bytesEchoed += static_cast<uint64_t>(n);
                    continue;
                }
                break;
            }
        });

        conn->setOnError([](const eular::utp::Connection::ConnectionErrorInfo &info) {
            std::cerr << "[client] conn error code=" << info.error_code
                      << " reason=" << info.error_reason
                      << " fatal=" << (info.fatal ? 1 : 0) << "\n";
        });

        sendTimer.start(1);
        statTimer.start(reportMs);
    });

    ctx.setOnConnectError([&](int32_t code, const std::string &reason, eular::utp::Context::ConnectAttemptInfo info) {
        std::cerr << "[client] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
        loop.breakLoop();
    });

    ctx.setOnConnectionClosed([&](eular::utp::Connection::Ptr c) {
        const auto d = c->description();
        std::cout << "[client] closed scid=" << d.scid
                  << " total_sent=" << bytesSent
                  << " total_echoed=" << bytesEchoed << "\n";
        loop.breakLoop();
    });

    if (ctx.bind("0.0.0.0", 0) != 0) {
        std::cerr << "[client] bind failed err=" << GetLastError() << "\n";
        return 1;
    }

    eular::utp::Context::ConnectInfo info;
    info.ip = serverIp;
    info.port = serverPort;
    info.timeout = 5000;
    info.encrypted = eular::utp::Context::kEncryptionNone;

    if (ctx.connect(info) != 0) {
        std::cerr << "[client] connect start failed err=" << GetLastError() << "\n";
        return 1;
    }

    std::cout << "[client] target=" << serverIp << ":" << serverPort
              << " total_bytes=" << totalBytes
              << " chunk=" << chunkBytes
              << " idle_seconds=" << idleSeconds
              << " (keepalive+dplpmtud enabled)\n";

    loop.dispatch();
    return 0;
}
