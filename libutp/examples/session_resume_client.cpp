#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <event/base.h>
#include <event/loop.h>
#include <event/timer.h>

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

std::vector<uint8_t> BuildRandomData(size_t size) {
    std::vector<uint8_t> out(size, 0);
    std::mt19937_64 rng(std::random_device{}());
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(rng() & 0xFF);
    }
    return out;
}

bool ParseUploadOk(const std::string &line, uint64_t &bytes, std::string &md5) {
    static const std::string kPrefix = "UPLOAD_OK bytes=";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    const size_t md5Pos = line.find(" md5=");
    if (md5Pos == std::string::npos || md5Pos <= kPrefix.size()) {
        return false;
    }

    const std::string bytesStr = line.substr(kPrefix.size(), md5Pos - kPrefix.size());
    const std::string md5Str = line.substr(md5Pos + 5);
    if (md5Str.empty()) {
        return false;
    }

    try {
        bytes = std::stoull(bytesStr);
    } catch (...) {
        return false;
    }

    md5 = md5Str;
    return true;
}

bool ParseMd5Reply(const std::string &line, std::string &md5, uint64_t &bytes) {
    static const std::string kPrefix = "MD5 ";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    const size_t bytesPos = line.find(" bytes=");
    if (bytesPos == std::string::npos || bytesPos <= kPrefix.size()) {
        return false;
    }

    md5 = line.substr(kPrefix.size(), bytesPos - kPrefix.size());
    const std::string bytesStr = line.substr(bytesPos + 7);
    try {
        bytes = std::stoull(bytesStr);
    } catch (...) {
        return false;
    }

    return true;
}

void PrintConnStats(const char *tag, const eular::utp::Connection::Ptr &conn) {
    if (!conn) {
        return;
    }
    const auto stat = conn->statistic();
    std::cout << tag
              << " pmtu=" << stat.pmtu
              << " rtt=" << stat.rtt
              << " rttvar=" << stat.rttvar
              << " tx=" << stat.tx_bytes
              << " rx=" << stat.rx_bytes
              << " rtx=" << stat.rtx_bytes
              << "\n";
}

void OnSignal(int) {
    std::cout << "\n[client] signal received, shutting down...\n";
    std::exit(0);
}

} // namespace

int main(int argc, char **argv)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 9000;
    uint32_t sizeKb = 512;
    uint32_t reconnectDelayMs = 1200;

    CLI::App app("UTP Session Resume Client");
    app.add_option("--server-ip", serverIp, "Server IP address")->check(CLI::ValidIPV4);
    app.add_option("--server-port", serverPort, "Server port")->check(CLI::Range(5000, 65535));
    app.add_option("--size-kb", sizeKb, "Random payload size in KB")->check(CLI::Range(1, 65536));
    app.add_option("--reconnect-delay-ms", reconnectDelayMs, "Delay before 0-RTT reconnect")->check(CLI::Range(100, 10000));
    CLI11_PARSE(app, argc, argv);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const size_t uploadSize = static_cast<size_t>(sizeKb) * 1024;
    std::vector<uint8_t> uploadData = BuildRandomData(uploadSize);

    Md5Accumulator fullMd5;
    if (!fullMd5.valid() || !fullMd5.update(uploadData.data(), uploadData.size())) {
        std::cerr << "[client] failed to init/update md5 for upload data\n";
        return 1;
    }
    std::string uploadMd5;
    if (!fullMd5.finalize(uploadMd5)) {
        std::cerr << "[client] failed to finalize md5 for upload data\n";
        return 1;
    }

    std::cout << "[client] prepared random payload bytes=" << uploadData.size() << " md5=" << uploadMd5 << "\n";

    ev::EventLoop loop;
    eular::utp::Config cfg;
    cfg.handshake_timeout = 5000;
    cfg.enable_keepalive = false;
    cfg.enable_dplpmtud = false;

    eular::utp::Context ctx(loop.loop(), &cfg);

    enum Phase : uint8_t {
        kPhaseHandshakeUpload = 0,
        kPhaseWaitReconnect,
        kPhaseZeroRttQuery,
        kPhaseDone,
    };

    Phase phase = kPhaseHandshakeUpload;
    eular::utp::Connection::Ptr connPhase1;
    eular::utp::Connection::Ptr connPhase2;
    eular::utp::Stream *uploadStream = nullptr;
    eular::utp::Stream *md5QueryStream = nullptr;

    std::vector<uint8_t> savedSessionToken;
    std::string savedResumptionState;
    bool sessionMaterialReady = false;
    bool uploadAckOk = false;
    bool md5CheckOk = false;
    uint8_t reconnectMaterialRetry = 0;

    std::string uploadReplyLine;
    std::string md5ReplyLine;

    size_t uploadOffset = 0;
    bool uploadHeaderSent = false;
    bool uploadFinSent = false;

    const std::string uploadHeader = "UPLOAD " + std::to_string(uploadData.size()) + "\n";
    const std::string md5Req = "REQ_MD5\n";

    ev::EventTimer reconnectTimer;
    ev::EventTimer stopTimer;
    ev::EventTimer watchdogTimer;

    auto finishWithFailure = [&](const std::string &reason) {
        std::cerr << "[client] abort: " << reason << "\n";
        phase = kPhaseDone;
        reconnectTimer.stop();
        watchdogTimer.stop();
        stopTimer.start(1);
    };

    auto attachMd5Readable = [&](eular::utp::Stream *stream) {
        if (stream == nullptr) {
            return;
        }

        md5QueryStream = stream;
        md5QueryStream->setOnReadable([&]() {
            std::vector<uint8_t> buf(4096, 0);
            for (;;) {
                const int32_t n = md5QueryStream->read(buf.data(), buf.size());
                if (n <= 0) {
                    break;
                }

                md5ReplyLine.append(reinterpret_cast<const char *>(buf.data()), static_cast<size_t>(n));
                const size_t lf = md5ReplyLine.find('\n');
                if (lf == std::string::npos) {
                    continue;
                }

                std::string line = md5ReplyLine.substr(0, lf);
                md5ReplyLine.erase(0, lf + 1);
                std::string recvMd5;
                uint64_t recvBytes = 0;
                if (!ParseMd5Reply(line, recvMd5, recvBytes)) {
                    finishWithFailure("bad md5 reply: " + line);
                    return;
                }

                md5CheckOk = (recvMd5 == uploadMd5) && (recvBytes == uploadData.size());
                std::cout << "[client] phase2 reply md5=" << recvMd5
                          << " bytes=" << recvBytes
                          << " verify=" << (md5CheckOk ? "PASS" : "FAIL") << "\n";

                PrintConnStats("[client] phase2 conn stat", connPhase2);
                const auto ctxStat = ctx.statistic();
                std::cout << "[client] phase2 ctx stat offered=" << ctxStat.zero_rtt_offered
                          << " accepted=" << ctxStat.zero_rtt_accepted
                          << " rejected=" << ctxStat.zero_rtt_rejected << "\n";

                if (connPhase2) {
                    connPhase2->close();
                }
                phase = kPhaseDone;
                watchdogTimer.stop();
                stopTimer.start(500);
                return;
            }
        });
    };

    auto kickUpload = [&]() {
        if (phase != kPhaseHandshakeUpload || uploadStream == nullptr) {
            return;
        }

        if (!uploadHeaderSent) {
            const int32_t n = uploadStream->write(uploadHeader.data(), uploadHeader.size(), false);
            if (n < 0) {
                std::cout << "[client] upload header blocked/error n=" << n << "\n";
                return;
            }
            uploadHeaderSent = true;
            std::cout << "[client] upload header sent\n";
        }

        constexpr size_t kChunk = 1024;
        while (uploadOffset < uploadData.size()) {
            const size_t left = uploadData.size() - uploadOffset;
            const size_t toSend = left < kChunk ? left : kChunk;
            const bool fin = (uploadOffset + toSend == uploadData.size());
            const int32_t n = uploadStream->write(uploadData.data() + uploadOffset, toSend, fin);
            if (n < 0) {
                std::cout << "[client] upload write blocked/error n=" << n
                          << " offset=" << uploadOffset
                          << " remain=" << left << "\n";
                return;
            }

            if (n == 0) {
                return;
            }

            uploadOffset += static_cast<size_t>(n);
            if ((uploadOffset % (1024 * 1024)) == 0 || uploadOffset == uploadData.size()) {
                std::cout << "[client] upload progress=" << uploadOffset << "/" << uploadData.size() << "\n";
            }

            if (fin && static_cast<size_t>(n) == toSend) {
                uploadFinSent = true;
                std::cout << "[client] upload payload sent bytes=" << uploadOffset << "\n";
                return;
            }
        }
    };

    reconnectTimer.reset(loop.loop(), [&]() {
        if (savedResumptionState.empty() && savedSessionToken.empty()) {
            if (reconnectMaterialRetry < 20) {
                ++reconnectMaterialRetry;
                std::cout << "[client] resumption material not ready, retry="
                          << static_cast<uint32_t>(reconnectMaterialRetry) << "\n";
                reconnectTimer.stop();
                reconnectTimer.start(100);
                return;
            }

            finishWithFailure("no resumption state/session token available, cannot start 0-rtt");
            return;
        }

        reconnectMaterialRetry = 0;

        phase = kPhaseZeroRttQuery;
        if (savedResumptionState.empty()) {
            printf("no resumption state available, start 0-rtt without resumption state\n");
            eular::utp::Context::Connect0RttInfo info;
            info.ip = serverIp;
            info.port = serverPort;
            info.timeout = 5000;
            info.session_ticket = savedSessionToken;
            info.early_data.assign(md5Req.begin(), md5Req.end());
            info.early_fin = true;

            const int32_t status = ctx.connect0Rtt(info);
            if (status != UTP_ERR_OK) {
                finishWithFailure("connect0Rtt failed: " + std::to_string(status));
                return;
            }
        } else {
            eular::utp::Context::Connect0RttWithStateInfo info;
            info.ip = serverIp;
            info.port = serverPort;
            info.timeout = 5000;
            info.early_data.assign(md5Req.begin(), md5Req.end());
            info.early_fin = true;

            const int32_t status = ctx.connect0RttWithState(info, savedResumptionState);
            if (status != UTP_ERR_OK) {
                finishWithFailure("connect0RttWithState failed: " + std::to_string(status));
                return;
            }
        }
        std::cout << "[client] started 0-rtt reconnect with early request\n";
    });

    stopTimer.reset(loop.loop(), [&]() {
        loop.breakLoop();
    });

    watchdogTimer.reset(loop.loop(), [&]() {
        std::cout << "[client] watchdog phase=" << static_cast<int>(phase)
                  << " upload=" << uploadOffset << "/" << uploadData.size()
                  << " token_ready=" << (sessionMaterialReady ? "yes" : "no")
                  << " upload_ack=" << (uploadAckOk ? "yes" : "no")
                  << " md5_ok=" << (md5CheckOk ? "yes" : "no") << "\n";
    });

    ctx.setOnConnected([&](eular::utp::Connection::Ptr conn) {
        if (phase == kPhaseHandshakeUpload) {
            connPhase1 = conn;
            std::cout << "[client] phase1 connected scid=" << conn->description().scid
                      << " dcid=" << conn->description().dcid << "\n";

            auto attachUploadStream = [&](eular::utp::Stream *stream) {
                if (stream == nullptr || uploadStream != nullptr) {
                    return;
                }

                uploadStream = stream;
                std::cout << "[client] phase1 local stream id=" << uploadStream->id() << "\n";
                uploadStream->setOnWritable([&]() {
                    kickUpload();
                });
                uploadStream->setOnReadable([&]() {
                    std::vector<uint8_t> buf(4096, 0);
                    for (;;) {
                        const int32_t n = uploadStream->read(buf.data(), buf.size());
                        if (n < 0) {
                            break;
                        }
                        if (n == 0) {
                            break;
                        }
                        uploadReplyLine.append(reinterpret_cast<const char *>(buf.data()), static_cast<size_t>(n));
                        const size_t lf = uploadReplyLine.find('\n');
                        if (lf == std::string::npos) {
                            continue;
                        }

                        std::string line = uploadReplyLine.substr(0, lf);
                        uploadReplyLine.erase(0, lf + 1);
                        uint64_t recvBytes = 0;
                        std::string recvMd5;
                        if (!ParseUploadOk(line, recvBytes, recvMd5)) {
                            finishWithFailure("bad upload reply: " + line);
                            return;
                        }

                        const bool match = (recvBytes == uploadData.size()) && (recvMd5 == uploadMd5);
                        std::cout << "[client] phase1 reply bytes=" << recvBytes
                                  << " md5=" << recvMd5
                                  << " match=" << (match ? "yes" : "no") << "\n";
                        uploadAckOk = match;

                        if (!sessionMaterialReady && connPhase1) {
                            std::vector<uint8_t> token;
                            std::string state;
                            if (connPhase1->exportSessionToken(token) == UTP_ERR_OK
                                && connPhase1->exportSessionResumptionState(state) == UTP_ERR_OK) {
                                savedSessionToken = std::move(token);
                                savedResumptionState = std::move(state);
                                sessionMaterialReady = true;
                                std::cout << "[client] saved SessionToken lazily size=" << savedSessionToken.size()
                                          << " and resumption state size=" << savedResumptionState.size() << "\n";
                            }
                        }

                        PrintConnStats("[client] phase1 conn stat", connPhase1);
                        const auto ctxStat = ctx.statistic();
                        std::cout << "[client] phase1 ctx stat offered=" << ctxStat.zero_rtt_offered
                                  << " accepted=" << ctxStat.zero_rtt_accepted
                                  << " rejected=" << ctxStat.zero_rtt_rejected << "\n";

                        if (sessionMaterialReady) {
                            phase = kPhaseWaitReconnect;
                            if (connPhase1) {
                                connPhase1->close();
                            }
                        } else {
                            std::cout << "[client] phase1 upload acked, waiting SessionToken before reconnect\n";
                        }
                        return;
                    }
                });
            };

            conn->setOnSessionTokenReady([&]() {
                std::vector<uint8_t> token;
                std::string state;
                const int32_t t = connPhase1 ? connPhase1->exportSessionToken(token) : UTP_ERR_INVALID_STATE;
                const int32_t s = connPhase1 ? connPhase1->exportSessionResumptionState(state) : UTP_ERR_INVALID_STATE;
                if (t == UTP_ERR_OK && s == UTP_ERR_OK) {
                    savedSessionToken = std::move(token);
                    savedResumptionState = std::move(state);
                    sessionMaterialReady = true;
                    std::cout << "[client] saved SessionToken size=" << savedSessionToken.size()
                              << " and resumption state size=" << savedResumptionState.size() << "\n";

                    if (phase == kPhaseHandshakeUpload && uploadAckOk && connPhase1) {
                        phase = kPhaseWaitReconnect;
                        connPhase1->close();
                    }
                }
            });

            const int32_t sid = conn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
            if (sid < 0) {
                finishWithFailure("phase1 createStream failed: " + std::to_string(sid));
                return;
            }

            attachUploadStream(conn->getStream(static_cast<uint32_t>(sid)));
            if (uploadStream == nullptr) {
                finishWithFailure("phase1 getStream failed: " + std::to_string(sid));
            }
            return;
        }

        if (phase == kPhaseZeroRttQuery) {
            connPhase2 = conn;
            std::cout << "[client] phase2 connected scid=" << conn->description().scid
                      << " dcid=" << conn->description().dcid << "\n";

            conn->setOnIncomingStream([&](eular::utp::Stream *stream) {
                if (stream == nullptr) {
                    return;
                }

                // 期待服务端在同一请求流上回包，或者创建新流回包，两者都处理。
                if (md5QueryStream == nullptr || stream->id() != md5QueryStream->id()) {
                    md5QueryStream = stream;
                }
                attachMd5Readable(md5QueryStream);
            });

            // 为 early_data 对应的本地流补建对象，确保回包可被应用层接收。
            const int32_t sid = conn->createStream(eular::utp::Connection::kStreamTypeBidirectional);
            if (sid >= 0) {
                eular::utp::Stream *s = conn->getStream(static_cast<uint32_t>(sid));
                if (s != nullptr) {
                    attachMd5Readable(s);
                }
            }
            return;
        }
    });

    ctx.setOnConnectError([&](int32_t code,
                               const std::string &reason,
                               eular::utp::Context::ConnectAttemptInfo info) {
        if (phase == kPhaseDone
            || (uploadAckOk && info.type == eular::utp::Context::kConnectAttemptNormal)) {
            std::cerr << "[client] ignored connect error code=" << code
                      << " peer=" << info.ip << ":" << info.port
                      << " reason=" << reason << "\n";
            return;
        }

        std::cerr << "[client] connect error code=" << code
                  << " peer=" << info.ip << ":" << info.port
                  << " reason=" << reason << "\n";
        finishWithFailure("connect error");
    });

    ctx.setOnConnectionClosed([&](eular::utp::Connection::Ptr c) {
        const auto desc = c->description();
        std::cout << "[client] connection closed scid=" << desc.scid << "\n";

        if (phase == kPhaseWaitReconnect && connPhase1 && c.get() == connPhase1.get()) {
            connPhase1.reset();
            uploadStream = nullptr;
            reconnectTimer.stop();
            reconnectTimer.start(reconnectDelayMs);
        }
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
        std::cerr << "[client] connect failed: " << connectStatus << "\n";
        return 1;
    }

    std::cout << "[client] phase1 start: encrypted handshake + upload "
              << uploadData.size() << " bytes\n";
    watchdogTimer.start(3000, 3000);
    loop.dispatch();

    const bool allOk = uploadAckOk && md5CheckOk;
    std::cout << "[client] final result: " << (allOk ? "PASS" : "FAIL") << "\n";
    return allOk ? 0 : 2;
}
