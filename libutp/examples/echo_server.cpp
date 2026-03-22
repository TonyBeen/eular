#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
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

void PrintUsage(const char *argv0)
{
    std::cout << "Usage: " << argv0 << " [--bind-ip 0.0.0.0] [--bind-port 9000]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string bindIp = "0.0.0.0";
    uint16_t bindPort = 9000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bind-ip") == 0 && i + 1 < argc) {
            bindIp = argv[++i];
            continue;
        }

        if (std::strcmp(argv[i], "--bind-port") == 0 && i + 1 < argc) {
            bindPort = static_cast<uint16_t>(std::stoul(argv[++i]));
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

    ctx.setOnNewConnection([](const eular::utp::Context::NewConnectionInfo &info) {
        std::cout << "[server] new connection from "
                  << info.remote_ip << ":" << info.remote_port
                  << ", local_cid=" << info.local_cid
                  << ", peer_cid=" << info.peer_cid << "\n";
        return true;
    });

    ctx.setOnConnected([](eular::utp::Connection::Ptr conn) {
        std::cout << "[server] connected scid=" << conn->description().scid
                  << " dcid=" << conn->description().dcid << "\n";

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
        std::cout << "[server] connection closed scid=" << conn->description().scid << "\n";
    });

    const int32_t bindStatus = ctx.bind(bindIp, bindPort);
    if (bindStatus != UTP_ERR_OK) {
        std::cerr << "[server] bind failed: " << bindStatus << "\n";
        return 1;
    }

    std::cout << "[server] listening on " << bindIp << ":" << bindPort << "\n";

    while (g_running.load(std::memory_order_acquire)) {
        loop.dispatch(EVLOOP_NONBLOCK | EVLOOP_ONCE);

        for (;;) {
            const int32_t acceptStatus = ctx.accept();
            if (acceptStatus == UTP_ERR_OK) {
                std::cout << "[server] accepted one pending incoming connection\n";
                continue;
            }
            if (acceptStatus != UTP_ERR_WOULD_BLOCK) {
                std::cerr << "[server] accept returned " << acceptStatus << "\n";
            }
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "[server] shutdown\n";
    return 0;
}
