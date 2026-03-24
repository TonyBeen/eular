#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <event/base.h>
#include <event/loop.h>
#include <event/timer.h>

#include <utils/CLI11.hpp>

#include <utp/errno.h>
#include <utp/utp.h>

namespace {

void OnSignal(int)
{
    std::cout << "\n[client] signal received, shutting down...\n";
    std::exit(0);
}

std::string RandomString(size_t len)
{
    static const char kAlphabet[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, sizeof(kAlphabet) - 2);

    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kAlphabet[dist(rng)]);
    }
    return out;
}

void PrintUsage(const char *argv0)
{
    std::cout << "Usage: " << argv0
              << " [--server-ip 127.0.0.1] [--server-port 9000]"
              << " [--interval-ms 1000] [--count 5] [--length 16]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 9000;
    uint32_t intervalMs = 1000;
    uint32_t sendCount = 5;
    size_t msgLen = 16;

    CLI::App app("UTP echo client example");
    app.add_option("--server-ip", serverIp, "Server IP address")->check(CLI::ValidIPV4);
    app.add_option("--server-port", serverPort, "Server port")->check(CLI::Range(5000, 65535));
    app.add_option("--interval-ms", intervalMs, "Interval between messages in milliseconds")->check(CLI::Range(1, 15000));
    app.add_option("--count", sendCount, "Number of messages to send")->check(CLI::Range(1, 1024));
    app.add_option("--length", msgLen, "Length of each message")->check(CLI::Range(16, 1024));
    CLI11_PARSE(app, argc, argv);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ev::EventLoop loop;
    eular::utp::Config cfg;
    cfg.handshake_timeout = 5000;
    cfg.enable_keepalive = false;
    cfg.enable_dplpmtud = false;
    eular::utp::Context ctx(loop.loop(), &cfg);

    eular::utp::Connection::Ptr conn;
    eular::utp::Stream *stream = nullptr;
    bool connected = false;
    bool connectFailed = false;

    uint32_t sent = 0;
    uint32_t echoed = 0;
    ev::EventTimer stopTimer;
    stopTimer.reset(loop.loop(), [&]() {
        std::cout << "[client] exiting...\n";
        loop.breakLoop();
    });

    ev::EventTimer nextSendTimer;
    nextSendTimer.reset(loop.loop(), [&]() {
        if (!connectFailed && connected && stream != nullptr) {
            if (sent >= sendCount) {
                std::cout << "[client] done, sent=" << sent << ", echoed=" << echoed << "\n";
                nextSendTimer.stop();
                conn->close();
                return;
            }

            const bool fin = (sent + 1 == sendCount);
            const std::string payload = RandomString(msgLen);
            const int32_t nwrite = stream->write(payload.data(), payload.size(), fin);
            if (nwrite < 0) {
                std::cerr << "[client] write failed: " << nwrite << "\n";
                return;
            }

            ++sent;
            std::cout << "[client] send #" << sent << ": \"" << payload << "\""
                      << (fin ? " [fin]" : "") << "\n";
        }
    });

    ctx.setOnConnected([&](eular::utp::Connection::Ptr c) {
        connected = true;
        conn = c;
        std::cout << "[client] connected scid=" << conn->description().scid
                  << " dcid=" << conn->description().dcid << "\n";

        const int32_t sid = conn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
        if (sid < 0) {
            std::cerr << "[client] createStream failed: " << sid << "\n";
            connectFailed = true;
            return;
        }

        stream = conn->getStream(static_cast<uint32_t>(sid));
        if (stream == nullptr) {
            std::cerr << "[client] getStream failed for sid=" << sid << "\n";
            connectFailed = true;
            return;
        }

        std::cout << "[client] local stream id=" << stream->id() << "\n";
        nextSendTimer.start(0, intervalMs);
        stream->setOnReadable([&]() {
            std::vector<uint8_t> buffer(2048);
            for (;;) {
                const int32_t n = stream->read(buffer.data(), buffer.size());
                if (n > 0) {
                    ++echoed;
                    std::string msg(reinterpret_cast<const char *>(buffer.data()),
                                    static_cast<size_t>(n));
                    std::cout << "[client] echo #" << echoed << ": \"" << msg << "\"\n";
                    continue;
                }
                break;
            }
        });
    });

    ctx.setOnConnectError([&](int32_t code, const std::string &reason, eular::utp::Context::ConnectAttemptInfo info) {
        std::cerr << "[client] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
        connectFailed = true;
    });

    ctx.setOnConnectionClosed([&](eular::utp::Connection::Ptr c) {
        std::cout << "[client] connection closed scid=" << c->description().scid << "\n";
        stopTimer.start(1000);
    });

    const int32_t bindStatus = ctx.bind("0.0.0.0", 0);
    if (bindStatus != UTP_ERR_OK) {
        std::cerr << "[client] bind failed: " << bindStatus << "\n";
        return 1;
    }

    eular::utp::Context::ConnectInfo info;
    info.ip = serverIp;
    info.port = serverPort;
    info.timeout = 5000;
    info.encrypted = eular::utp::Context::kEncryptionNone;
    const int32_t connectStatus = ctx.connect(info);
    if (connectStatus != UTP_ERR_OK) {
        std::cerr << "[client] connect start failed: " << connectStatus << "\n";
        return 1;
    }

    std::cout << "[client] connecting to " << serverIp << ":" << serverPort
              << ", interval=" << intervalMs << "ms"
              << ", count=" << sendCount
              << ", length=" << msgLen << "\n";

    loop.dispatch();
    return 0;
}
