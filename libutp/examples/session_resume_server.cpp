#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <event/base.h>
#include <event/loop.h>

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

    Md5Accumulator(const Md5Accumulator &) = delete;
    Md5Accumulator &operator=(const Md5Accumulator &) = delete;

    Md5Accumulator(Md5Accumulator &&other) noexcept {
        m_ctx = other.m_ctx;
        m_ok = other.m_ok;
        m_finalized = other.m_finalized;
        other.m_ctx = nullptr;
        other.m_ok = false;
        other.m_finalized = false;
    }

    Md5Accumulator &operator=(Md5Accumulator &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        if (m_ctx != nullptr) {
            EVP_MD_CTX_free(m_ctx);
        }

        m_ctx = other.m_ctx;
        m_ok = other.m_ok;
        m_finalized = other.m_finalized;
        other.m_ctx = nullptr;
        other.m_ok = false;
        other.m_finalized = false;
        return *this;
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

struct StreamUploadSession {
    enum Mode : uint8_t {
        kModeUnknown = 0,
        kModeUpload,
        kModeGetMd5,
        kModeInvalid,
    };

    Mode mode{kModeUnknown};
    std::string headerLine;
    uint64_t expectedBytes{0};
    uint64_t receivedBytes{0};
    uint64_t loggedBytes{0};
    Md5Accumulator md5;
    bool finSeen{false};
};

struct ServerSharedState {
    std::string latestMd5;
    uint64_t latestBytes{0};
};

bool ParseUploadHeader(const std::string &line, uint64_t &expectedBytes) {
    static const std::string kPrefix = "UPLOAD ";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    const std::string num = line.substr(kPrefix.size());
    if (num.empty()) {
        return false;
    }

    for (char ch : num) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }

    try {
        expectedBytes = std::stoull(num);
    } catch (...) {
        return false;
    }

    return expectedBytes > 0;
}

void PrintUsage(const char *argv0) {
    std::cout << "Usage: " << argv0 << " [--bind-ip 0.0.0.0] [--bind-port 9000]\n";
}

void OnSignal(int) {
    std::cout << "\n[server] signal received, shutting down...\n";
    std::exit(0);
}

} // namespace

int main(int argc, char **argv)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::string bindIp = "0.0.0.0";
    uint16_t bindPort = 9000;

    CLI::App app{"UTP Session Resume Server"};
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
    auto shared = std::make_shared<ServerSharedState>();

    ctx.setOnNewConnection([&ctx](const eular::utp::Context::NewConnectionInfo &info) {
        std::cout << "[server] incoming connection remote="
                  << info.remote_ip << ":" << info.remote_port
                  << " local_cid=" << info.local_cid
                  << " peer_cid=" << info.peer_cid
                  << " enc=" << static_cast<int>(info.encrypted) << "\n";

        if (info.local_cid != 0) {
            const int32_t acceptStatus = ctx.accept();
            if (acceptStatus != UTP_ERR_OK) {
                std::cerr << "[server] accept failed local_cid=" << info.local_cid
                          << " status=" << acceptStatus << "\n";
                return false;
            }
        }
        return true;
    });

    ctx.setOnZeroRttDecision([](const eular::utp::Context::ZeroRttDecisionInfo &info) {
        std::cout << "[server] 0-rtt " << (info.accepted ? "accepted" : "rejected")
                  << " remote=" << info.remote_ip << ":" << info.remote_port
                  << " peer_cid=" << info.peer_cid
                  << " reason=" << info.reason << "\n";
    });

    ctx.setOnConnected([shared](eular::utp::Connection::Ptr conn) {
        const auto desc = conn->description();
        std::cout << "[server] connected scid=" << desc.scid
                  << " dcid=" << desc.dcid
                  << " peer=" << desc.remoteHost << ":" << desc.remotePort << "\n";

        auto sessions = std::make_shared<std::unordered_map<uint32_t, StreamUploadSession>>();

        conn->setOnIncomingStream([conn, shared, sessions](eular::utp::Stream *stream) {
            if (stream == nullptr) {
                return;
            }

            const uint32_t sid = stream->id();
            std::cout << "[server] incoming stream id=" << sid << "\n";
            sessions->try_emplace(sid);

            stream->setOnReadable([conn, shared, sessions, stream]() {
                if (stream == nullptr) {
                    return;
                }

                const uint32_t sid = stream->id();
                auto it = sessions->find(sid);
                if (it == sessions->end()) {
                    return;
                }

                StreamUploadSession &session = it->second;
                std::vector<uint8_t> buffer(32 * 1024, 0);

                auto replyError = [stream](const std::string &reason) {
                    const std::string line = "ERR " + reason + "\n";
                    (void)stream->write(line.data(), line.size(), true);
                    stream->close();
                };

                auto tryFinalize = [&]() {
                    if (!session.finSeen) {
                        return;
                    }

                    std::cout << "[server] eof stream id=" << sid
                              << " mode=" << static_cast<int>(session.mode)
                              << " received=" << session.receivedBytes
                              << " expected=" << session.expectedBytes << "\n";

                    if (session.mode == StreamUploadSession::kModeUpload) {
                        if (session.receivedBytes != session.expectedBytes) {
                            std::cerr << "[server] upload size mismatch, expected="
                                      << session.expectedBytes << " received=" << session.receivedBytes << "\n";
                            replyError("size_mismatch");
                            sessions->erase(sid);
                            return;
                        }

                        std::string md5;
                        if (!session.md5.finalize(md5)) {
                            replyError("md5_finalize_failed");
                            sessions->erase(sid);
                            return;
                        }

                        shared->latestMd5 = md5;
                        shared->latestBytes = session.receivedBytes;

                        const std::string line = "UPLOAD_OK bytes=" + std::to_string(shared->latestBytes)
                                              + " md5=" + shared->latestMd5 + "\n";
                        (void)stream->write(line.data(), line.size(), true);
                        std::cout << "[server] upload completed bytes=" << shared->latestBytes
                                  << " md5=" << shared->latestMd5 << "\n";
                        sessions->erase(sid);
                        return;
                    }

                    if (session.mode == StreamUploadSession::kModeGetMd5) {
                        std::string line;
                        if (shared->latestMd5.empty()) {
                            line = "MD5 NONE bytes=0\n";
                        } else {
                            line = "MD5 " + shared->latestMd5 + " bytes=" + std::to_string(shared->latestBytes) + "\n";
                        }
                        (void)stream->write(line.data(), line.size(), true);
                        std::cout << "[server] replied md5="
                                  << (shared->latestMd5.empty() ? "NONE" : shared->latestMd5) << "\n";
                        sessions->erase(sid);
                        return;
                    }

                    replyError("bad_mode");
                    sessions->erase(sid);
                };

                for (;;) {
                    const int32_t n = stream->read(buffer.data(), buffer.size());
                    if (n < 0) {
                        std::cout << "[server] stream id=" << sid
                                  << " read would block received=" << session.receivedBytes
                                  << "/" << session.expectedBytes << "\n";
                        break;
                    }

                    if (n == 0) {
                        session.finSeen = true;
                        tryFinalize();
                        break;
                    }

                    const uint8_t *chunk = buffer.data();
                    size_t left = static_cast<size_t>(n);

                    while (left > 0) {
                        if (session.mode == StreamUploadSession::kModeUnknown) {
                            if (session.headerLine.size() > 1024) {
                                session.mode = StreamUploadSession::kModeInvalid;
                                replyError("header_too_large");
                                sessions->erase(sid);
                                return;
                            }

                            const uint8_t *lf = static_cast<const uint8_t *>(std::memchr(chunk, '\n', left));
                            if (lf == nullptr) {
                                session.headerLine.append(reinterpret_cast<const char *>(chunk), left);
                                left = 0;
                                continue;
                            }

                            const size_t headerPart = static_cast<size_t>(lf - chunk);
                            session.headerLine.append(reinterpret_cast<const char *>(chunk), headerPart);
                            chunk = lf + 1;
                            left -= (headerPart + 1);

                            if (session.headerLine == "REQ_MD5") {
                                session.mode = StreamUploadSession::kModeGetMd5;
                                std::cout << "[server] stream id=" << sid << " request=REQ_MD5\n";
                                continue;
                            }

                            uint64_t expected = 0;
                            if (ParseUploadHeader(session.headerLine, expected)) {
                                session.mode = StreamUploadSession::kModeUpload;
                                session.expectedBytes = expected;
                                std::cout << "[server] stream id=" << sid
                                          << " upload expected=" << session.expectedBytes << "\n";
                                if (!session.md5.valid()) {
                                    session.mode = StreamUploadSession::kModeInvalid;
                                    replyError("md5_init_failed");
                                    sessions->erase(sid);
                                    return;
                                }
                                continue;
                            }

                            session.mode = StreamUploadSession::kModeInvalid;
                            replyError("invalid_header");
                            sessions->erase(sid);
                            return;
                        }

                        if (session.mode == StreamUploadSession::kModeUpload) {
                            const uint64_t remain = (session.expectedBytes > session.receivedBytes)
                                                 ? (session.expectedBytes - session.receivedBytes)
                                                 : 0;
                            const size_t take = static_cast<size_t>(std::min<uint64_t>(remain, left));
                            if (take > 0) {
                                if (!session.md5.update(chunk, take)) {
                                    session.mode = StreamUploadSession::kModeInvalid;
                                    replyError("md5_update_failed");
                                    sessions->erase(sid);
                                    return;
                                }
                                session.receivedBytes += static_cast<uint64_t>(take);
                                if (session.receivedBytes - session.loggedBytes >= (1024 * 1024)
                                    || session.receivedBytes == session.expectedBytes) {
                                    session.loggedBytes = session.receivedBytes;
                                    std::cout << "[server] upload progress stream id=" << sid
                                              << " bytes=" << session.receivedBytes
                                              << "/" << session.expectedBytes << "\n";
                                }
                                chunk += take;
                                left -= take;
                            }

                            if (left > 0) {
                                session.mode = StreamUploadSession::kModeInvalid;
                                replyError("payload_overflow");
                                sessions->erase(sid);
                                return;
                            }
                            continue;
                        }

                        if (session.mode == StreamUploadSession::kModeGetMd5) {
                            // REQ_MD5 不应携带额外 payload。
                            if (left > 0) {
                                session.mode = StreamUploadSession::kModeInvalid;
                                replyError("unexpected_payload");
                                sessions->erase(sid);
                                return;
                            }
                        }

                        break;
                    }
                }
            });
        });
    });

    ctx.setOnConnectError([](int32_t code,
                             const std::string &reason,
                             eular::utp::Context::ConnectAttemptInfo info) {
        std::cerr << "[server] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
    });

    ctx.setOnConnectionClosed([](eular::utp::Connection::Ptr conn) {
        const auto desc = conn->description();
        std::cout << "[server] connection closed scid=" << desc.scid << " peer=" << desc.remoteHost << ":" << desc.remotePort << "\n";
        const auto statistic = conn->statistic();
        printf("[server] connection statistic scid=%u pmtu=%u rtt=%u rttvar=%u bw_estimate=%u rx_bytes=%lu tx_bytes=%lu rtx_bytes=%lu\n",
               desc.scid, statistic.pmtu, statistic.rtt, statistic.rttvar, statistic.bw_estimate,
               statistic.rx_bytes, statistic.tx_bytes, statistic.rtx_bytes);
    });

    const int32_t bindStatus = ctx.bind(bindIp, bindPort);
    if (bindStatus != UTP_ERR_OK) {
        std::cerr << "[server] bind failed: " << bindStatus << "\n";
        return 1;
    }

    std::cout << "[server] listening on " << bindIp << ":" << bindPort << "\n";
    loop.dispatch();
    return 0;
}
