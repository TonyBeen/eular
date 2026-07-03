#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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

uint64_t NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string MakePayload(size_t len)
{
    static const char kAlphabet[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kAlphabet[i % (sizeof(kAlphabet) - 1)]);
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 9000;
    std::string bindIp = "0.0.0.0";
    uint16_t bindPort = 0;
    uint32_t sendCount = 5;
    size_t msgLen = 16;
    uint64_t totalBytes = 0;
    bool silent = false;

    CLI::App app("UTP echo client example");
    app.add_option("--server-ip", serverIp, "Server IP address")->check(CLI::ValidIPV4);
    app.add_option("--server-port", serverPort, "Server port")->check(CLI::Range(5000, 65535));
    app.add_option("--bind-ip", bindIp, "Local bind IP address")->check(CLI::ValidIPV4);
    app.add_option("--bind-port", bindPort, "Local bind port")->check(CLI::Range(0, 65535));
    app.add_option("--count", sendCount, "Number of messages to send")->check(CLI::Range(1, 200000));
    app.add_option("--length", msgLen, "Length of each message")->check(CLI::Range(16, 16384));
    app.add_option("--total-bytes", totalBytes, "Total bytes to send; if > 0, overrides --count")->check(CLI::Range(static_cast<uint64_t>(0), static_cast<uint64_t>(4294967296ULL)));
    app.add_flag("--quiet", silent, "Suppress all client output");
    app.add_flag("--silent", silent, "Alias for --quiet");
    CLI11_PARSE(app, argc, argv);

    std::signal(SIGINT, [](int) { std::exit(0); });
    std::signal(SIGTERM, [](int) { std::exit(0); });

    ev::EventLoop loop;
    eular::utp::Config cfg;
    cfg.handshake_timeout = 3000;
    cfg.enable_keepalive = false;
    cfg.enable_dplpmtud = false;
    cfg.zero_rtt_token_max_lifetime = 0;
    cfg.mtu_base = 1400;
    cfg.mtu_min = 1400;
    cfg.mtu_max = 1400;
    cfg.recv_buf_size = 16 * 1024 * 1024;
    cfg.send_buf_size = 16 * 1024 * 1024;
    cfg.initial_max_stream_data_bidi_local = 1024 * 1024;
    cfg.initial_max_stream_data_bidi_remote = 1024 * 1024;
    cfg.stream_send_buffer_limit = 1024 * 1024;
    cfg.stream_unacked_data_limit = 1024 * 1024;
    eular::utp::Context ctx(loop.loop(), &cfg);

    eular::utp::Connection::Ptr conn;
    eular::utp::Stream *stream = nullptr;
    bool connected = false;
    bool connectFailed = false;
    bool finalResultPrinted = false;

    uint32_t sent = 0;
    uint64_t sentBytes = 0;
    bool sendDone = false;
    bool closeIssued = false;
    bool hashFinalized = false;
    bool doneReceived = false;
    uint64_t serverDoneBytes = 0;
    uint64_t waitDoneStartMs = 0;
    const uint64_t startMs = NowMs();
    uint64_t connectedMs = 0;
    uint64_t uploadDoneMs = 0;
    uint64_t doneMs = 0;
    std::string localHash;
    std::string serverHash;
    size_t ackCount = 0;
    const uint64_t drainCheckIntervalMs = 1000;
    const uint64_t drainMaxWaitMs = 180000;
    const uint64_t targetBytes = (totalBytes > 0)
        ? totalBytes
        : (static_cast<uint64_t>(sendCount) * static_cast<uint64_t>(msgLen));

    const std::string uploadHeader = "UPLOAD " + std::to_string(targetBytes) + "\n";
    size_t headerOffset = 0;
    std::string recvLineBuffer;
    Xxh128Accumulator hash;
    if (!hash.valid()) {
        if (!silent) {
            std::cerr << "[client] failed to initialize xxh128\n";
        }
        return 1;
    }

    // Pre-generate payload to avoid CPU bottleneck during high-speed tests
    const std::string fixedPayload = MakePayload(msgLen);

    auto copyToWriteViews = [&](eular::utp::Stream::MutableBufferView views[2],
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

    auto printFinalResult = [&](const std::string &reason) {
        if (finalResultPrinted) {
            return;
        }
        finalResultPrinted = true;

        const bool bytesMatch = doneReceived && (serverDoneBytes == sentBytes);
        const bool hashMatch = doneReceived && (serverHash == localHash);
        const bool pass = doneReceived && bytesMatch && hashMatch && !connectFailed;

        std::ostream &out = pass ? std::cout : std::cerr;
        out << "[client] result=" << (pass ? "PASS" : "FAIL")
            << " reason=" << reason
            << " sent_bytes=" << sentBytes
            << " done_bytes=" << serverDoneBytes
            << " connect_ms=" << (connectedMs > 0 ? connectedMs - startMs : 0)
            << " upload_ms=" << (uploadDoneMs > 0 ? uploadDoneMs - startMs : 0)
            << " done_ms=" << (doneMs > 0 ? doneMs - startMs : 0)
            << " total_ms=" << (NowMs() - startMs)
            << " local_xxh128=" << (localHash.empty() ? "<pending>" : localHash)
            << " server_xxh128=" << (serverHash.empty() ? "<pending>" : serverHash)
            << " ack_count=" << ackCount
            << " done_received=" << (doneReceived ? 1 : 0)
            << "\n";
    };

    ev::EventTimer drainTimer;
    drainTimer.reset(loop.loop(), [&]() {
        if (!closeIssued && connected && conn) {
            const uint64_t nowMs = NowMs();
            if (waitDoneStartMs != 0 && nowMs < waitDoneStartMs + drainMaxWaitMs) {
                drainTimer.start(drainCheckIntervalMs, drainCheckIntervalMs);
                return;
            }
            if (!silent) {
                std::cout << "[client] drain timeout, closing connection\n";
            }
            printFinalResult("drain-timeout");
            closeIssued = true;
            conn->close();
        }
    });

    auto finalizeLocalHash = [&]() -> bool {
        if (hashFinalized) {
            return true;
        }
        if (!hash.finalize(localHash)) {
            if (!silent) {
                std::cerr << "[client] xxh128 finalize failed\n";
            }
            return false;
        }
        hashFinalized = true;
        return true;
    };

    auto onSendFinished = [&]() {
        if (sendDone) {
            return;
        }
        sendDone = true;
        uploadDoneMs = NowMs();
        if (!finalizeLocalHash()) {
            connectFailed = true;
            if (!closeIssued && conn) {
                closeIssued = true;
                conn->close();
            }
            return;
        }
        if (!silent) {
            std::cout << "[client] upload finished, waiting DONE, sent_bytes=" << sentBytes
                      << " xxh128=" << localHash << "\n";
        }
        waitDoneStartMs = NowMs();
        drainTimer.start(drainCheckIntervalMs, drainCheckIntervalMs);
    };

    auto parseServerLine = [&](const std::string &line) {
        if (line.rfind("DONE bytes=", 0) == 0) {
            const size_t hashPos = line.find(" xxh128=");
            if (hashPos == std::string::npos) {
                if (!silent) {
                    std::cerr << "[client] bad DONE line: " << line << "\n";
                }
                return;
            }

            try {
                serverDoneBytes =
                    std::stoull(line.substr(std::strlen("DONE bytes="), hashPos - std::strlen("DONE bytes=")));
            } catch (...) {
                if (!silent) {
                    std::cerr << "[client] bad DONE bytes: " << line << "\n";
                }
                return;
            }
            serverHash = line.substr(hashPos + std::strlen(" xxh128="));
            doneReceived = true;
            doneMs = NowMs();
            drainTimer.stop();

            if (!hashFinalized && !finalizeLocalHash()) {
                connectFailed = true;
            }

            const bool bytesMatch = (serverDoneBytes == sentBytes);
            const bool hashMatch = (serverHash == localHash);
            if (!silent) {
                std::cout << "[client] done bytes=" << serverDoneBytes
                          << " xxh128=" << serverHash
                          << " local_bytes=" << sentBytes
                          << " local_xxh128=" << localHash
                          << " result=" << ((bytesMatch && hashMatch) ? "PASS" : "FAIL")
                          << "\n";
            }
            printFinalResult((bytesMatch && hashMatch) ? "done" : "mismatch");

            if (!closeIssued && conn) {
                closeIssued = true;
                conn->close();
            }
            return;
        }

        if (line.rfind("ERR ", 0) == 0) {
            if (!silent) {
                std::cerr << "[client] server error: " << line << "\n";
            }
            connectFailed = true;
            printFinalResult("server-error");
            if (!closeIssued && conn) {
                closeIssued = true;
                conn->close();
            }
            return;
        }

        if (!silent) {
            std::cout << "[client] server: " << line << "\n";
        }
    };

    ctx.setOnConnected([&](eular::utp::Connection::Ptr c) {
        connected = true;
        connectedMs = NowMs();
        conn = c;
        if (!silent) {
            std::cout << "[client] connected scid=" << conn->description().scid << " dcid=" << conn->description().dcid << "\n";
        }

        const int32_t sid = conn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
        if (sid < 0) {
            if (!silent) {
                std::cerr << "[client] createStream failed: " << sid << "\n";
            }
            connectFailed = true;
            return;
        }

        stream = conn->getStream(static_cast<uint32_t>(sid));
        if (stream == nullptr) {
            if (!silent) {
                std::cerr << "[client] getStream failed for sid=" << sid << "\n";
            }
            connectFailed = true;
            return;
        }

        if (!silent) {
            std::cout << "[client] local stream id=" << stream->id() << "\n";
        }

        auto trySendPtr = std::make_shared<std::function<void()>>();
        *trySendPtr = [&]() {
            if (connectFailed || !connected || stream == nullptr || sendDone) {
                return;
            }

            while (true) {
                if (headerOffset < uploadHeader.size()) {
                    const size_t remaining = uploadHeader.size() - headerOffset;
                    eular::utp::Stream::MutableBufferView views[2];
                    const size_t grant = stream->acquireWriteBuffer(views, remaining);
                    if (grant == 0) {
                        return;
                    }
                    const size_t copied = copyToWriteViews(
                        views,
                        reinterpret_cast<const uint8_t *>(uploadHeader.data() + headerOffset),
                        grant);
                    if (copied == 0) {
                        return;
                    }
                    const int32_t committed = stream->commitWrite(copied, false);
                    if (committed <= 0) {
                        if (committed < 0 && utp_get_last_error() != UTP_ERR_WOULD_BLOCK) {
                            if (!silent) {
                                std::cerr << "[client] write header failed: " << utp_get_error_string() << "\n";
                            }
                            connectFailed = true;
                        }
                        return;
                    }
                    headerOffset += static_cast<size_t>(committed);
                    if (headerOffset < uploadHeader.size()) {
                        return;
                    }
                    continue;
                }

                if (sentBytes >= targetBytes || (totalBytes == 0 && sent >= sendCount)) {
                    onSendFinished();
                    return;
                }

                const uint64_t remaining = targetBytes - sentBytes;
                const size_t chunkLen = static_cast<size_t>(std::min<uint64_t>(remaining, static_cast<uint64_t>(msgLen)));
                const bool fin = (chunkLen == remaining);
                eular::utp::Stream::MutableBufferView views[2];
                const size_t grant = stream->acquireWriteBuffer(views, chunkLen);
                if (grant < chunkLen) {
                    return;
                }

                const size_t copied = copyToWriteViews(
                    views,
                    reinterpret_cast<const uint8_t *>(fixedPayload.data()),
                    chunkLen);
                if (copied != chunkLen) {
                    return;
                }

                const int32_t committed = stream->commitWrite(chunkLen, fin);
                if (committed < 0) {
                    if (utp_get_last_error() != UTP_ERR_WOULD_BLOCK) {
                        if (!silent) {
                            std::cerr << "[client] write payload failed: " << utp_get_error_string() << "\n";
                        }
                        connectFailed = true;
                    }
                    return;
                }

                if (!hash.update(reinterpret_cast<const uint8_t *>(fixedPayload.data()), chunkLen)) {
                    if (!silent) {
                        std::cerr << "[client] xxh128 update failed\n";
                    }
                    connectFailed = true;
                    return;
                }

                ++sent;
                sentBytes += static_cast<uint64_t>(chunkLen);

                if (!silent) {
                    std::cout << "[client] send #" << sent
                              << " bytes=" << chunkLen
                              << (fin ? " [fin]" : "") << "\n";
                }

                if (fin) {
                    onSendFinished();
                    return;
                }
            }
        };

        stream->setOnWritable([trySendPtr]() {
            (*trySendPtr)();
        });

        stream->setOnReadable([&]() {
            for (;;) {
                eular::utp::Stream::ConstBufferView views[2];
                const size_t readable = stream->acquireReadViews(views, 65536);
                if (readable == 0) {
                    if (stream->state() == eular::utp::Stream::kStateHalfClosedRemote ||
                        stream->state() == eular::utp::Stream::kStateClosed) {
                        (void)stream->commitReadViews(0);
                        if (!doneReceived) {
                            if (!silent) {
                                std::cerr << "[client] server closed before DONE\n";
                            }
                        }
                    }
                    break;
                }

                size_t consumed = 0;
                auto processReadableChunk = [&](const char *data, size_t len) {
                    const char *cursor = data;
                    size_t left = len;
                    while (left > 0) {
                        const void *lfPos = std::memchr(cursor, '\n', left);
                        if (lfPos == nullptr) {
                            recvLineBuffer.append(cursor, left);
                            consumed += left;
                            return;
                        }

                        const char *lf = static_cast<const char *>(lfPos);
                        const size_t partLen = static_cast<size_t>(lf - cursor);
                        if (recvLineBuffer.empty()) {
                            std::string line(cursor, partLen);
                            if (!line.empty() && line.back() == '\r') {
                                line.pop_back();
                            }
                            parseServerLine(line);
                        } else {
                            recvLineBuffer.append(cursor, partLen);
                            if (!recvLineBuffer.empty() && recvLineBuffer.back() == '\r') {
                                recvLineBuffer.pop_back();
                            }
                            parseServerLine(recvLineBuffer);
                            recvLineBuffer.clear();
                        }

                        const size_t step = partLen + 1;
                        cursor += step;
                        left -= step;
                        consumed += step;
                    }
                };

                for (size_t i = 0; i < 2 && consumed < readable; ++i) {
                    if (views[i].data == nullptr || views[i].len == 0) {
                        continue;
                    }
                    processReadableChunk(static_cast<const char *>(views[i].data), views[i].len);
                }

                if (stream->commitReadViews(consumed) < 0) {
                    if (!silent) {
                        std::cerr << "[client] commitReadViews failed\n";
                    }
                    connectFailed = true;
                    break;
                }
            }
        });

        (*trySendPtr)();
    });

    ctx.setOnConnectError([&](int32_t code, const std::string &reason, eular::utp::Context::ConnectAttemptInfo info) {
        if (!silent) {
            std::cerr << "[client] connect error code=" << code
                      << " peer=" << info.ip << ":" << info.port
                      << " reason=" << reason << "\n";
        }
        connectFailed = true;
        printFinalResult("connect-error");
    });

    ctx.setOnConnectionClosed([&](eular::utp::Connection::Ptr c) {
        if (!silent) {
            std::cout << "[client] connection closed scid=" << c->description().scid << "\n";
        }
        if (!finalResultPrinted) {
            printFinalResult(doneReceived ? "closed" : "closed-before-done");
        }
        loop.breakLoop();
    });

    const int32_t bindStatus = ctx.bind(bindIp, bindPort);
    if (bindStatus != UTP_ERR_OK) {
        std::cerr << "[client] bind failed: " << bindStatus
                  << " last_error=" << utp_get_last_error()
                  << " last_error_msg=" << utp_get_error_string()
                  << " bind_ip=" << bindIp
                  << " bind_port=" << bindPort << "\n";
        printFinalResult("bind-failed");
        return 1;
    }

    eular::utp::Context::ConnectInfo info;
    info.ip = serverIp;
    info.port = serverPort;
    info.timeout = 3000;
    info.encrypted = eular::utp::Context::kEncryptionNone;
    const int32_t connectStatus = ctx.connect(info);
    if (connectStatus != UTP_ERR_OK) {
        if (!silent) {
            std::cerr << "[client] connect start failed: " << connectStatus << "\n";
        }
        printFinalResult("connect-start-failed");
        return 1;
    }

    if (!silent) {
        std::cout << "[client] connecting to " << serverIp << ":" << serverPort
                  << ", count=" << sendCount
                  << ", length=" << msgLen
                  << ", total_bytes=" << totalBytes
                  << ", target_bytes=" << targetBytes
                  << ", silent=0\n";
    }

    loop.dispatch();
    if (!finalResultPrinted) {
        printFinalResult("loop-exit");
    }
    return 0;
}
