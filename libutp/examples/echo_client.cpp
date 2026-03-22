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

#include <event2/event.h>
#include <event/loop.h>

#include <utp/errno.h>
#include <utp/utp.h>

namespace {

std::atomic<bool> g_running(true);

void OnSignal(int)
{
    g_running.store(false, std::memory_order_release);
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

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server-ip") == 0 && i + 1 < argc) {
            serverIp = argv[++i];
            continue;
        }

        if (std::strcmp(argv[i], "--server-port") == 0 && i + 1 < argc) {
            serverPort = static_cast<uint16_t>(std::stoul(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            intervalMs = static_cast<uint32_t>(std::stoul(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            sendCount = static_cast<uint32_t>(std::stoul(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "--length") == 0 && i + 1 < argc) {
            msgLen = static_cast<size_t>(std::stoul(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }

        std::cerr << "unknown argument: " << argv[i] << "\n";
        PrintUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ev::EventLoop loop;
    eular::utp::Config cfg;
    cfg.handshake_timeout = 5000;
    eular::utp::Context ctx(loop.loop(), &cfg);

    eular::utp::Connection::Ptr conn;
    eular::utp::Stream *stream = nullptr;
    bool connected = false;
    bool connectFailed = false;

    uint32_t sent = 0;
    uint32_t echoed = 0;

    const auto start = std::chrono::steady_clock::now();
    auto nextSendAt = start;
    auto lastSendAt = start;

    ctx.setOnConnected([&](eular::utp::Connection::Ptr c) {
        connected = true;
        conn = c;
        std::cout << "[client] connected scid=" << conn->description().scid
                  << " dcid=" << conn->description().dcid << "\n";

        conn->registerStreamCreated([&](eular::utp::Stream *s) {
            stream = s;
            std::cout << "[client] stream created id=" << stream->id() << "\n";
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

        const int32_t sid = conn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
        if (sid < 0) {
            std::cerr << "[client] createStream failed: " << sid << "\n";
            connectFailed = true;
            g_running.store(false, std::memory_order_release);
            return;
        }

        // Give the transport time to finish HandshakeDone exchange on the passive side.
        nextSendAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    });

    ctx.setOnConnectError([&](int32_t code, const std::string &reason, eular::utp::Context::ConnectInfo info) {
        std::cerr << "[client] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
        connectFailed = true;
        g_running.store(false, std::memory_order_release);
    });

    ctx.setOnConnectionClosed([&](eular::utp::Connection::Ptr c) {
        std::cout << "[client] connection closed scid=" << c->description().scid << "\n";
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
    info.encrypted = false;
    const int32_t connectStatus = ctx.connect(info);
    if (connectStatus != UTP_ERR_OK) {
        std::cerr << "[client] connect start failed: " << connectStatus << "\n";
        return 1;
    }

    std::cout << "[client] connecting to " << serverIp << ":" << serverPort
              << ", interval=" << intervalMs << "ms"
              << ", count=" << sendCount
              << ", length=" << msgLen << "\n";

    bool allSent = false;
    while (g_running.load(std::memory_order_acquire)) {
        loop.dispatch(EVLOOP_NONBLOCK | EVLOOP_ONCE);

        const auto now = std::chrono::steady_clock::now();
        if (!connectFailed && connected && stream != nullptr && !allSent && now >= nextSendAt) {
            const bool fin = (sent + 1 == sendCount);
            const std::string payload = RandomString(msgLen);
            const int32_t nwrite = stream->write(payload.data(), payload.size(), fin);
            if (nwrite < 0) {
                std::cerr << "[client] write failed: " << nwrite << "\n";
                break;
            }

            ++sent;
            lastSendAt = now;
            nextSendAt = now + std::chrono::milliseconds(intervalMs);
            std::cout << "[client] send #" << sent << ": \"" << payload << "\""
                      << (fin ? " [fin]" : "") << "\n";

            if (sent >= sendCount) {
                allSent = true;
            }
        }

        if (allSent) {
            const auto elapsedAfterLastSend = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSendAt);
            if (echoed >= sent || elapsedAfterLastSend.count() > 5000) {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (conn) {
        conn->close();
    }

    for (int i = 0; i < 100; ++i) {
        loop.dispatch(EVLOOP_NONBLOCK | EVLOOP_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (connectFailed) {
        return 1;
    }

    std::cout << "[client] done, sent=" << sent << ", echoed=" << echoed << "\n";
    return 0;
}
