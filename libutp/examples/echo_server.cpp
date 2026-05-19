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

#define XXH_INLINE_ALL
#include "../3rd/xxhash.h"
#include <utils/CLI11.hpp>

#include <utp/errno.h>
#include <utp/context.h>

namespace {

class Xxh128Accumulator {
public:
    Xxh128Accumulator() {
        m_state = XXH3_createState();
        if (m_state != nullptr) {
            m_ok = (XXH3_128bits_reset(m_state) == XXH_OK);
        }
    }

    ~Xxh128Accumulator() {
        if (m_state != nullptr) {
            XXH3_freeState(m_state);
            m_state = nullptr;
        }
    }

    bool valid() const { return m_ok; }

    bool update(const uint8_t *data, size_t len) {
        if (!m_ok || m_finalized) {
            return false;
        }
        return XXH3_128bits_update(m_state, data, len) == XXH_OK;
    }

    bool finalize(std::string &hexOut) {
        if (!m_ok || m_finalized) {
            return false;
        }

        const XXH128_hash_t digest = XXH3_128bits_digest(m_state);
        XXH128_canonical_t canonical;
        XXH128_canonicalFromHash(&canonical, digest);
        static const char *kHex = "0123456789abcdef";
        hexOut.clear();
        hexOut.reserve(sizeof(canonical.digest) * 2);
        for (uint8_t byte : canonical.digest) {
            hexOut.push_back(kHex[byte >> 4]);
            hexOut.push_back(kHex[byte & 0x0F]);
        }

        m_finalized = true;
        return true;
    }

private:
    XXH3_state_t *m_state{nullptr};
    bool m_ok{false};
    bool m_finalized{false};
};

std::string PeerKey(const std::string &ip, uint16_t port)
{
    return ip + ":" + std::to_string(port);
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
    bool silent = false;
    bool handshakeTrace = false;

    CLI::App app{"UTP Echo Server"};
    app.add_option("--bind-ip", bindIp, "IP address to bind")->check(CLI::ValidIPV4);
    app.add_option("--bind-port", bindPort, "Port to bind")->check(CLI::Range(5000, 65535));
    app.add_flag("--quiet", silent, "Suppress all server output");
    app.add_flag("--silent", silent, "Alias for --quiet");
    app.add_flag("--handshake-trace", handshakeTrace, "Print handshake progress even in quiet mode");
    CLI11_PARSE(app, argc, argv);

    if (handshakeTrace) {
        ::setenv("UTP_HANDSHAKE_TRACE_INTERNAL", "1", 1);
    }

    std::signal(SIGINT, [](int) { std::exit(0); });
    std::signal(SIGTERM, [](int) { std::exit(0); });

    ev::EventLoop loop;
    eular::utp::Config cfg;
    cfg.enable_keepalive = true;
    cfg.enable_dplpmtud = false;
    cfg.mtu_base = 1400;
    cfg.mtu_min = 1400;
    cfg.mtu_max = 1400;
    cfg.ack_every_n_packets = 30;
    cfg.handshake_timeout = 3000;
    cfg.recv_buf_size = 16 * 1024 * 1024;
    cfg.send_buf_size = 16 * 1024 * 1024;
    cfg.initial_max_stream_data_bidi_local = 1024 * 1024;
    cfg.initial_max_stream_data_bidi_remote = 1024 * 1024;
    cfg.stream_send_buffer_limit = 1024 * 1024;
    cfg.stream_unacked_data_limit = 1024 * 1024;

    eular::utp::Context ctx(loop.loop(), &cfg);
    std::unordered_set<std::string> zeroRttAcceptedPeers;
    const bool traceHandshake = !silent || handshakeTrace;

    size_t read_size = 0;
    ev::EventTimer print_timer;
    print_timer.reset(loop.loop(), [&]() {
        if (!silent) {
            std::cout << "[server] total read so far: " << read_size << " bytes\n";
        }
    });
    print_timer.start(1000, 1000);

    ctx.setOnNewConnection([&ctx, traceHandshake](const eular::utp::Context::NewConnectionInfo &info) {
        const bool zeroRttPath = info.local_cid == 0;
        if (zeroRttPath) {
            if (traceHandshake) {
                std::cout << "[server] incoming 0-rtt request from "
                          << info.remote_ip << ":" << info.remote_port
                          << ", peer_cid=" << info.peer_cid << "\n";
            }
            return true;
        }

        if (traceHandshake) {
            std::cout << "[server] incoming handshake request from "
                      << info.remote_ip << ":" << info.remote_port
                      << ", local_cid=" << info.local_cid
                      << ", peer_cid=" << info.peer_cid << "\n";
        }

        const int32_t acceptStatus = ctx.accept();
        if (acceptStatus != UTP_ERR_OK) {
            if (traceHandshake) {
                std::cerr << "[server] accept failed for local_cid=" << info.local_cid
                          << ": " << acceptStatus << "\n";
            }
            return false;
        }

        if (traceHandshake) {
            std::cout << "[server] accepted handshake local_cid=" << info.local_cid << "\n";
        }
        return true;
    });

    ctx.setOnZeroRttDecision([&](const eular::utp::Context::ZeroRttDecisionInfo &info) {
        const std::string peer = PeerKey(info.remote_ip, info.remote_port);
        if (info.accepted) {
            zeroRttAcceptedPeers.insert(peer);
            if (traceHandshake) {
                std::cout << "[server] 0-rtt accepted from "
                          << peer
                          << ", peer_cid=" << info.peer_cid
                          << ", reason=" << info.reason << "\n";
            }
            return;
        }

        zeroRttAcceptedPeers.erase(peer);
        if (traceHandshake) {
            std::cout << "[server] 0-rtt rejected from "
                      << peer
                      << ", peer_cid=" << info.peer_cid
                      << ", reason=" << info.reason << "\n";
        }
    });

    ctx.setOnConnected([&](eular::utp::Connection::Ptr conn) {
        const auto desc = conn->description();
        const std::string peer = PeerKey(desc.remoteHost, desc.remotePort);
        const bool zeroRttPath = zeroRttAcceptedPeers.find(peer) != zeroRttAcceptedPeers.end();

        if (traceHandshake) {
            std::cout << "[server] connected via "
                      << (zeroRttPath ? "0-rtt" : "handshake")
                      << " scid=" << desc.scid
                      << " dcid=" << desc.dcid
                      << " peer=" << peer << "\n";
        }

        zeroRttAcceptedPeers.erase(peer);

        conn->setOnIncomingStream([&](eular::utp::Stream *stream) {
            if (!silent) {
                std::cout << "[server] incoming stream id=" << stream->id() << "\n";
            }

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
                Xxh128Accumulator xxh128;

                std::string outbox;
                size_t outboxOffset{0};
                bool closeAfterFlush{false};
                bool failed{false};
                bool finSeen{false};
            };
            auto session = std::make_shared<Session>();
            session->outbox.reserve(256);

            auto queueLine = [session](const std::string &line) {
                session->outbox.append(line);
            };

            auto markFailed = [session, queueLine, silent](const std::string &reason) {
                if (session->failed) {
                    return;
                }
                session->failed = true;
                if (!silent) {
                    std::cerr << "[server] stream failed: " << reason << "\n";
                }
                queueLine("ERR " + reason + "\n");
                session->closeAfterFlush = true;
            };

            auto copyToWriteViews = [](eular::utp::Stream::MutableBufferView views[2],
                                       const uint8_t *src,
                                       size_t len) -> size_t {
                size_t copied = 0;
                for (size_t i = 0; i < 2 && copied < len; ++i) {
                    if (views[i].data == nullptr || views[i].len == 0) {
                        continue;
                    }
                    const size_t ncopy = std::min(views[i].len, len - copied);
                    std::memcpy(views[i].data, src + copied, ncopy);
                    copied += ncopy;
                }
                return copied;
            };

            auto tryFlushPtr = std::make_shared<std::function<void()>>();
            *tryFlushPtr = [stream, session, tryFlushPtr, silent, copyToWriteViews]() {
                while (session->outboxOffset < session->outbox.size()) {
                    const size_t left = session->outbox.size() - session->outboxOffset;
                    eular::utp::Stream::MutableBufferView views[2];
                    const size_t grant = stream->acquireWriteBuffer(views, left);
                    if (grant == 0) {
                        return;
                    }

                    const size_t copied = copyToWriteViews(
                        views,
                        reinterpret_cast<const uint8_t *>(session->outbox.data() + session->outboxOffset),
                        grant);
                    if (copied == 0) {
                        return;
                    }

                    const int32_t committed = stream->commitWrite(copied, false);
                    if (committed <= 0) {
                        if (committed < 0 && utp_get_last_error() != UTP_ERR_WOULD_BLOCK) {
                            if (!silent) {
                                std::cerr << "[server] write failed: " << utp_get_error_string() << "\n";
                            }
                            session->closeAfterFlush = true;
                        }
                        return;
                    }
                    session->outboxOffset += static_cast<size_t>(committed);
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

            stream->setOnReadable([stream, session, queueLine, markFailed, tryFlushPtr, &read_size, silent]() {
                if (session->phase == Session::kClosed) {
                    return;
                }

                auto finalizeDone = [&]() {
                    if (session->failed) {
                        return;
                    }
                    if (session->phase != Session::kReadPayload) {
                        markFailed("missing_upload_header");
                    } else if (session->receivedBytes != session->expectedBytes) {
                        markFailed("size_mismatch");
                    } else {
                        std::string hashHex;
                        if (!session->xxh128.finalize(hashHex)) {
                            markFailed("xxh128_finalize_failed");
                        } else {
                            queueLine("DONE bytes=" + std::to_string(session->receivedBytes) + " xxh128=" + hashHex + "\n");
                            session->closeAfterFlush = true;
                        }
                    }
                    (*tryFlushPtr)();
                };

                for (;;) {
                    eular::utp::Stream::ConstBufferView views[2];
                    const size_t readable = stream->acquireReadViews(views, 65536);
                    if (readable == 0) {
                        if (stream->state() == eular::utp::Stream::kStateHalfClosedRemote ||
                            stream->state() == eular::utp::Stream::kStateClosed) {
                            (void)stream->commitReadViews(0);
                            session->finSeen = true;
                            finalizeDone();
                        }
                        break;
                    }

                    size_t consumedTotal = 0;

                    auto processChunk = [&](const uint8_t *chunk, size_t left) {
                        while (left > 0 && !session->failed) {
                            if (session->phase == Session::kReadHeader) {
                                const uint8_t *lf = static_cast<const uint8_t *>(std::memchr(chunk, '\n', left));
                                if (lf == nullptr) {
                                    session->headerBuffer.append(reinterpret_cast<const char *>(chunk), left);
                                    if (session->headerBuffer.size() > 1024) {
                                        markFailed("header_too_large");
                                    }
                                    consumedTotal += left;
                                    return;
                                }

                                const size_t headPartLen = static_cast<size_t>(lf - chunk);
                                session->headerBuffer.append(reinterpret_cast<const char *>(chunk), headPartLen);
                                if (!session->headerBuffer.empty() && session->headerBuffer.back() == '\r') {
                                    session->headerBuffer.pop_back();
                                }

                                uint64_t expected = 0;
                                if (!ParseUploadHeader(session->headerBuffer, expected)) {
                                    markFailed("bad_upload_header");
                                    consumedTotal += headPartLen + 1;
                                    return;
                                }
                                session->expectedBytes = expected;
                                session->phase = Session::kReadPayload;
                                session->headerBuffer.clear();
                                if (!session->xxh128.valid()) {
                                    markFailed("xxh128_init_failed");
                                    consumedTotal += headPartLen + 1;
                                    return;
                                }

                                const size_t consume = headPartLen + 1;
                                chunk += consume;
                                left -= consume;
                                consumedTotal += consume;
                                continue;
                            }

                            if (session->phase != Session::kReadPayload) {
                                consumedTotal += left;
                                return;
                            }

                            if (session->receivedBytes >= session->expectedBytes) {
                                markFailed("payload_overflow");
                                consumedTotal += left;
                                return;
                            }

                            const uint64_t remain = session->expectedBytes - session->receivedBytes;
                            const size_t consume = static_cast<size_t>(std::min<uint64_t>(remain, left));
                            if (!session->xxh128.update(chunk, consume)) {
                                markFailed("xxh128_update_failed");
                                consumedTotal += consume;
                                return;
                            }
                            session->receivedBytes += static_cast<uint64_t>(consume);
                            consumedTotal += consume;
                            queueLine("ACK total=" + std::to_string(session->receivedBytes) + "\n");

                            chunk += consume;
                            left -= consume;

                            if (left > 0 && session->receivedBytes >= session->expectedBytes) {
                                markFailed("payload_overflow");
                                consumedTotal += left;
                                return;
                            }
                        }
                    };

                    for (size_t i = 0; i < 2; ++i) {
                        if (views[i].data == nullptr || views[i].len == 0 || session->failed) {
                            continue;
                        }
                        processChunk(static_cast<const uint8_t *>(views[i].data), views[i].len);
                    }

                    if (consumedTotal == 0) {
                        break;
                    }
                    if (stream->commitReadViews(consumedTotal) < 0) {
                        if (!silent) {
                            std::cerr << "[server] commitReadViews failed\n";
                        }
                        break;
                    }

                    read_size += consumedTotal;

                    if (!session->failed && session->phase == Session::kReadPayload &&
                        session->receivedBytes == session->expectedBytes &&
                        stream->state() != eular::utp::Stream::kStateOpen && !session->finSeen) {
                        session->finSeen = true;
                        finalizeDone();
                    }

                    (*tryFlushPtr)();
                }
            });
        });
    });

    ctx.setOnConnectError([traceHandshake](int32_t code, const std::string &reason, eular::utp::Context::ConnectAttemptInfo info) {
        if (traceHandshake) {
            std::cerr << "[server] connect error code=" << code
                      << " peer=" << info.ip << ":" << info.port
                      << " reason=" << reason << "\n";
        }
    });

    ctx.setOnConnectionClosed([traceHandshake](eular::utp::Connection::Ptr conn) {
        const auto desc = conn->description();
        if (traceHandshake) {
            std::cout << "[server] connection closed scid=" << desc.scid
                      << " peer=" << desc.remoteHost << ":" << desc.remotePort << "\n";
        }
    });

    const int32_t bindStatus = ctx.bind(bindIp, bindPort);
    if (bindStatus != UTP_ERR_OK) {
        std::cerr << "[server] bind failed: " << bindStatus
                  << " last_error=" << utp_get_last_error()
                  << " last_error_msg=" << utp_get_error_string()
                  << " bind_ip=" << bindIp
                  << " bind_port=" << bindPort
                  << "\n";
        return 1;
    }

    if (traceHandshake) {
        std::cout << "[server] listening on " << bindIp << ":" << bindPort << std::endl;
    }

    loop.dispatch();
    if (traceHandshake) {
        std::cout << "[server] shutdown\n";
    }
    return 0;
}
