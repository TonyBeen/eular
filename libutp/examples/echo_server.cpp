#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <event/base.h>
#include <event/loop.h>
#include <event/timer.h>

#ifdef X509_NAME
#undef X509_NAME
#endif
#include <openssl/evp.h>

#include <utils/CLI11.hpp>

#include <utp/errno.h>
#include <utp/utp.h>

namespace {

class Md5Accumulator {
public:
    Md5Accumulator() {
        m_ctx = EVP_MD_CTX_new();
        if (m_ctx != nullptr) {
            m_ok = (EVP_DigestInit_ex(m_ctx, EVP_md5(), nullptr) == 1);
        }
    }

    ~Md5Accumulator() {
        if (m_ctx != nullptr) {
            EVP_MD_CTX_free(m_ctx);
            m_ctx = nullptr;
        }
    }

    bool valid() const { return m_ok; }

    bool update(const uint8_t *data, size_t len) {
        if (!m_ok || m_finalized) {
            return false;
        }
        return EVP_DigestUpdate(m_ctx, data, len) == 1;
    }

    bool finalize(std::string &hexOut) {
        if (!m_ok || m_finalized) {
            return false;
        }

        uint8_t digest[EVP_MAX_MD_SIZE] = {0};
        unsigned int digestLen = 0;
        if (EVP_DigestFinal_ex(m_ctx, digest, &digestLen) != 1) {
            return false;
        }

        static const char *kHex = "0123456789abcdef";
        hexOut.clear();
        hexOut.reserve(digestLen * 2);
        for (unsigned int i = 0; i < digestLen; ++i) {
            hexOut.push_back(kHex[digest[i] >> 4]);
            hexOut.push_back(kHex[digest[i] & 0x0F]);
        }

        m_finalized = true;
        return true;
    }

private:
    EVP_MD_CTX *m_ctx{nullptr};
    bool m_ok{false};
    bool m_finalized{false};
};

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

bool ParseUploadHeader(const std::string &line, uint64_t &expectedBytes)
{
    static const std::string kPrefix = "UPLOAD ";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    const std::string value = line.substr(kPrefix.size());
    if (value.empty()) {
        return false;
    }
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }

    try {
        expectedBytes = std::stoull(value);
    } catch (...) {
        return false;
    }
    return expectedBytes > 0;
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
    cfg.enable_keepalive = true;
    cfg.enable_dplpmtud = false;
    cfg.mtu_base = 1400;
    cfg.ack_every_n_packets = 30;
    cfg.handshake_timeout = 3000;

    eular::utp::Context ctx(loop.loop(), &cfg);
    std::unordered_set<std::string> zeroRttAcceptedPeers;

    size_t read_size = 0;
    ev::EventTimer print_timer;
    print_timer.reset(loop.loop(), [&]() {
        printf("[server] total read so far: %zu bytes\n", read_size);
    });
    print_timer.start(1000, 1000);

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

        conn->setOnIncomingStream([&](eular::utp::Stream *stream) {
            std::cout << "[server] incoming stream id=" << stream->id() << "\n";

            struct Session {
                enum Phase : uint8_t {
                    kReadHeader = 0,
                    kReadPayload,
                    kClosed,
                };

                Phase phase{kReadHeader};
                std::string headerBuffer;
                uint64_t expectedBytes{0};
                uint64_t receivedBytes{0};
                Md5Accumulator md5;

                std::vector<uint8_t> outbox;
                size_t outboxOffset{0};
                bool closeAfterFlush{false};
                bool failed{false};
                bool finSeen{false};
            };
            auto session = std::make_shared<Session>();

            auto queueLine = [session](const std::string &line) {
                session->outbox.insert(session->outbox.end(), line.begin(), line.end());
            };

            auto markFailed = [session, queueLine](const std::string &reason) {
                if (session->failed) {
                    return;
                }
                session->failed = true;
                queueLine("ERR " + reason + "\n");
                session->closeAfterFlush = true;
            };

            auto tryFlushPtr = std::make_shared<std::function<void()>>();
            *tryFlushPtr = [stream, session, tryFlushPtr]() {
                while (session->outboxOffset < session->outbox.size()) {
                    const uint8_t *base = session->outbox.data() + session->outboxOffset;
                    const size_t left = session->outbox.size() - session->outboxOffset;
                    const int32_t nw = stream->write(base, left, false);
                    if (nw < 0) {
                        if (utp_get_last_error() == UTP_ERR_WOULD_BLOCK) {
                            return;
                        }
                        std::cerr << "[server] write failed: " << utp_get_error_string() << "\n";
                        session->closeAfterFlush = true;
                        break;
                    }
                    if (nw == 0) {
                        return;
                    }
                    session->outboxOffset += static_cast<size_t>(nw);
                }

                if (session->outboxOffset >= session->outbox.size()) {
                    session->outbox.clear();
                    session->outboxOffset = 0;
                    if (session->closeAfterFlush) {
                        session->phase = Session::kClosed;
                        stream->close();
                    }
                }
            };

            stream->setOnWritable([tryFlushPtr]() {
                (*tryFlushPtr)();
            });

            stream->setOnReadable([stream, session, queueLine, markFailed, tryFlushPtr, &read_size]() {
                if (session->phase == Session::kClosed) {
                    return;
                }

                std::vector<uint8_t> buffer(32 * 1024, 0);
                for (;;) {
                    const int32_t n = stream->read(buffer.data(), buffer.size());
                    if (n < 0) {
                        break;
                    }
                    read_size += n;

                    if (n == 0) {
                        session->finSeen = true;
                        if (!session->failed) {
                            if (session->phase != Session::kReadPayload) {
                                markFailed("missing_upload_header");
                            } else if (session->receivedBytes != session->expectedBytes) {
                                markFailed("size_mismatch");
                            } else {
                                std::string md5Hex;
                                if (!session->md5.finalize(md5Hex)) {
                                    markFailed("md5_finalize_failed");
                                } else {
                                    queueLine("DONE bytes=" + std::to_string(session->receivedBytes) + " md5=" + md5Hex + "\n");
                                    session->closeAfterFlush = true;
                                }
                            }
                        }

                        (*tryFlushPtr)();
                        break;
                    }

                    const uint8_t *chunk = buffer.data();
                    size_t left = static_cast<size_t>(n);
                    while (left > 0 && !session->failed) {
                        if (session->phase == Session::kReadHeader) {
                            const uint8_t *lf = static_cast<const uint8_t *>(std::memchr(chunk, '\n', left));
                            if (lf == nullptr) {
                                session->headerBuffer.append(reinterpret_cast<const char *>(chunk), left);
                                if (session->headerBuffer.size() > 1024) {
                                    markFailed("header_too_large");
                                }
                                left = 0;
                                break;
                            }

                            const size_t headPartLen = static_cast<size_t>(lf - chunk);
                            session->headerBuffer.append(reinterpret_cast<const char *>(chunk), headPartLen);
                            if (!session->headerBuffer.empty() && session->headerBuffer.back() == '\r') {
                                session->headerBuffer.pop_back();
                            }

                            uint64_t expected = 0;
                            if (!ParseUploadHeader(session->headerBuffer, expected)) {
                                markFailed("bad_upload_header");
                                left = 0;
                                break;
                            }
                            session->expectedBytes = expected;
                            session->phase = Session::kReadPayload;
                            session->headerBuffer.clear();
                            if (!session->md5.valid()) {
                                markFailed("md5_init_failed");
                                left = 0;
                                break;
                            }

                            const size_t consume = headPartLen + 1;
                            chunk += consume;
                            left -= consume;
                            continue;
                        }

                        if (session->phase != Session::kReadPayload) {
                            left = 0;
                            break;
                        }

                        if (session->receivedBytes >= session->expectedBytes) {
                            markFailed("payload_overflow");
                            left = 0;
                            break;
                        }

                        const uint64_t remain = session->expectedBytes - session->receivedBytes;
                        const size_t consume = static_cast<size_t>(std::min<uint64_t>(remain, left));
                        if (!session->md5.update(chunk, consume)) {
                            markFailed("md5_update_failed");
                            left = 0;
                            break;
                        }
                        session->receivedBytes += static_cast<uint64_t>(consume);
                        queueLine("ACK total=" + std::to_string(session->receivedBytes) + "\n");

                        chunk += consume;
                        left -= consume;

                        if (left > 0 && session->receivedBytes >= session->expectedBytes) {
                            markFailed("payload_overflow");
                            left = 0;
                            break;
                        }
                    }

                    (*tryFlushPtr)();
                }
            });
        });
    });

    ctx.setOnConnectError([](int32_t code, const std::string &reason, eular::utp::Context::ConnectAttemptInfo info) {
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

    std::cout << "[server] listening on " << bindIp << ":" << bindPort << std::endl;

    loop.dispatch();
    std::cout << "[server] shutdown\n";
    return 0;
}
