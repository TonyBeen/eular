/*************************************************************************
    > File Name: context_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:32 PM CST
 ************************************************************************/

#include "context/context_impl.h"
#include "context/packet_decode_helper.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unordered_set>
#include <vector>

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "proto/proto.h"
#include "proto/packet_in.h"
#include "proto/frame/version.h"
#include "proto/frame/crypto.h"
#include "proto/frame/transport_params.h"
#include "proto/frame/ack_frequency.h"
#include "proto/frame/handshake_delay.h"
#include "proto/frame/handshake_done.h"
#include "proto/frame/handshake_helper.h"
#include "proto/frame/session_token.h"
#include "proto/frame/stream.h"
#include "proto/frame/connection_close.h"
#include "proto/frame/path.h"
#include "proto/frame/padding.h"
#include "proto/frame/reset_stream.h"
#include "context/connection_impl.h"
#include "crypto/base64.h"
#include "crypto/aes_gcm_context.h"
#include "crypto/resumption_state_codec.h"
#include "crypto/token.h"
#include "crypto/x25519_wrapper.h"
#include "util/error.h"
#include "util/random.hpp"
#include "util/time.h"
#include "context_impl.h"
#include "make_unique.hpp"
#include "logger/logger.h"

static std::atomic<uint32_t>    g_contextId{0};
static eular::utp::Config       g_defaultConfig;

namespace {
const std::array<uint8_t, eular::utp::ResumptionStateCodec::KEY_SIZE> kDefaultResumptionSecret = {
    0x33, 0x81, 0x7a, 0x14, 0x9d, 0xbe, 0x20, 0x4f,
    0x72, 0x61, 0x99, 0xca, 0x5a, 0x3e, 0x1d, 0xb0,
    0xa7, 0x25, 0xe3, 0x44, 0x8c, 0x9f, 0x10, 0x6d,
    0xfa, 0x57, 0x02, 0x3b, 0xc4, 0x88, 0x6e, 0x11,
};

bool HandshakeTraceEnabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = std::getenv("UTP_HANDSHAKE_TRACE_INTERNAL");
        enabled = (env != nullptr && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return enabled == 1;
}

void HandshakeTracePrint(const char *fmt, ...)
{
    if (!HandshakeTraceEnabled()) {
        return;
    }

    std::fprintf(stderr, "[utp-handshake] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

uint32_t PendingHandshakeBaseTimeoutMs(const eular::utp::Config &cfg,
                                       const eular::utp::TransportParams &peerTp)
{
    const uint32_t localTimeoutMs = std::max<uint16_t>(cfg.handshake_timeout, 1);
    if ((peerTp.flags & eular::utp::TransportParams::kHandshakeTimeout) != 0 && peerTp.handshake_timeout > 0) {
        return std::min<uint32_t>(localTimeoutMs, peerTp.handshake_timeout);
    }
    return localTimeoutMs;
}

uint64_t PendingHandshakeRoundTimeoutUs(const eular::utp::Config &cfg,
                                        const eular::utp::TransportParams &peerTp,
                                        uint8_t round)
{
    const uint64_t baseTimeoutUs = static_cast<uint64_t>(PendingHandshakeBaseTimeoutMs(cfg, peerTp)) * 1000ULL;
    if (round >= 63) {
        return UINT64_MAX;
    }

    const uint64_t scaled = baseTimeoutUs << round;
    return scaled < baseTimeoutUs ? UINT64_MAX : scaled;
}

uint64_t PendingHandshakeRetryDueUs(const eular::utp::Config &cfg,
                                    const eular::utp::TransportParams &peerTp,
                                    utp_time_t lastHandshakeSentUs,
                                    uint8_t handshakeRetryCount)
{
    if (lastHandshakeSentUs == 0) {
        return UINT64_MAX;
    }

    const uint64_t roundTimeoutUs = PendingHandshakeRoundTimeoutUs(cfg, peerTp, handshakeRetryCount);
    if (roundTimeoutUs == UINT64_MAX) {
        return UINT64_MAX;
    }

    const uint64_t baseUs = static_cast<uint64_t>(lastHandshakeSentUs);
    const uint64_t dueUs = baseUs + roundTimeoutUs;
    return dueUs < baseUs ? UINT64_MAX : dueUs;
}

uint8_t PendingHandshakeMaxRetries(const eular::utp::Config &cfg)
{
    return cfg.handshake_max_retries;
}

size_t PendingPreHandshakeBufferMaxPackets(const eular::utp::Config &cfg)
{
    return std::max<size_t>(cfg.pending_pre_handshake_buffer_packets, 1);
}

size_t PendingPreHandshakeBufferMaxBytes(const eular::utp::Config &cfg)
{
    return std::max<size_t>(cfg.pending_pre_handshake_buffer_bytes, 1024);
}

uint32_t ConnectionWdrQuantum(const eular::utp::Config &cfg)
{
    return std::max<uint32_t>(cfg.connection_wdrr_quantum, 256);
}

uint32_t ConnectionWdrDeficitCap(const eular::utp::Config &cfg)
{
    const uint32_t quantum = ConnectionWdrQuantum(cfg);
    return std::max<uint32_t>(cfg.connection_wdrr_deficit_cap, quantum);
}

eular::utp::ConnectionSchedulerMode ConnectionScheduler(const eular::utp::Config &cfg)
{
    switch (cfg.connection_scheduler_mode) {
    case eular::utp::kConnectionSchedulerDisabled:
    case eular::utp::kConnectionSchedulerStrict:
    case eular::utp::kConnectionSchedulerWdrr:
        return cfg.connection_scheduler_mode;
    default:
        return eular::utp::kConnectionSchedulerWdrr;
    }
}

eular::utp::FrameCryptoType EncryptionModeToFrameCryptoType(eular::utp::Context::EncryptionMode mode)
{
    switch (mode) {
    case eular::utp::Context::kEncryptionAesGcm256:
        return eular::utp::kFrameCryptoAESGCM256;
    case eular::utp::Context::kEncryptionAesGcm128:
    default:
        return eular::utp::kFrameCryptoAESGCM128;
    }
}

eular::utp::Context::EncryptionMode FrameCryptoTypeToEncryptionMode(eular::utp::FrameCryptoType type)
{
    switch (type) {
    case eular::utp::kFrameCryptoAESGCM256:
        return eular::utp::Context::kEncryptionAesGcm256;
    case eular::utp::kFrameCryptoAESGCM128:
    default:
        return eular::utp::Context::kEncryptionAesGcm128;
    }
}


int32_t AppendPaddingToTargetPayloadSize(size_t targetPayloadSize,
                                         std::vector<uint8_t> &payload)
{
    if (targetPayloadSize <= payload.size()) {
        return UTP_ERR_OK;
    }

    const size_t remain = targetPayloadSize - payload.size();
    if (remain < FRAME_PADDING_HDR_SIZE) {
        return UTP_ERR_OK;
    }

    eular::utp::FramePadding padding;
    padding.padding_length = static_cast<uint16_t>(remain - FRAME_PADDING_HDR_SIZE);

    const size_t oldSize = payload.size();
    payload.resize(targetPayloadSize, 0);
    eular::utp::Status st;
    const int32_t encoded = padding.encode(payload.data() + oldSize, remain, st);
    if (!st.ok() || static_cast<size_t>(encoded) != remain) {
        return -1;
    }

    return UTP_ERR_OK;
}

uint16_t ConfiguredMinPacketSize(const eular::utp::Config &config,
                                 eular::utp::Address::Family family)
{
    const uint16_t targetMtu = eular::utp::MtuDiscovery::NormalizeMtu(config.mtu_min, family);
    return eular::utp::MtuDiscovery::PacketSizeFromMtu(targetMtu, family);
}

bool InitAesContextByCryptoType(eular::utp::FrameCryptoType type,
                                const eular::utp::X25519Wrapper::PublicKey &peerPublicKey,
                                const std::shared_ptr<eular::utp::X25519Wrapper> &x25519,
                                const std::shared_ptr<eular::utp::AesGcmContext> &aesCtx,
                                uint32_t noncePrefix)
{
    if (!x25519 || !aesCtx) {
        return false;
    }

    if (type == eular::utp::kFrameCryptoAESGCM256) {
        auto sharedSecret = x25519->deriveSharedSecret(peerPublicKey);
        eular::utp::AesGcmContext::AesKey256 key256;
        std::copy(sharedSecret.begin(), sharedSecret.end(), key256.begin());
        return aesCtx->init(key256, noncePrefix).ok();
    }

    auto sharedSecretShort = x25519->deriveSharedSecretShort(peerPublicKey);
    eular::utp::AesGcmContext::AesKey128 key128;
    std::copy(sharedSecretShort.begin(), sharedSecretShort.end(), key128.begin());
    return aesCtx->init(key128, noncePrefix).ok();
}

bool DecodeTokenEncryptionMode(uint8_t modeRaw, eular::utp::Context::EncryptionMode &mode)
{
    switch (modeRaw) {
    case eular::utp::Context::kEncryptionNone:
        mode = eular::utp::Context::kEncryptionNone;
        return true;
    case eular::utp::Context::kEncryptionAesGcm128:
        mode = eular::utp::Context::kEncryptionAesGcm128;
        return true;
    case eular::utp::Context::kEncryptionAesGcm256:
        mode = eular::utp::Context::kEncryptionAesGcm256;
        return true;
    default:
        return false;
    }
}

eular::utp::Context::ConnectAttemptInfo MakeConnectAttemptInfo(const eular::utp::Context::ConnectInfo &info)
{
    eular::utp::Context::ConnectAttemptInfo attempt;
    attempt.ip = info.ip;
    attempt.port = info.port;
    attempt.timeout = info.timeout;
    attempt.retries = info.retries;
    attempt.encrypted = info.encrypted;
    attempt.type = eular::utp::Context::kConnectAttemptNormal;
    return attempt;
}

eular::utp::Context::ConnectAttemptInfo MakeConnectAttemptInfo(const eular::utp::Context::Connect0RttInfo &info)
{
    eular::utp::Context::ConnectAttemptInfo attempt;
    attempt.ip = info.ip;
    attempt.port = info.port;
    attempt.timeout = info.timeout;
    attempt.retries = info.retries;
    attempt.encrypted = eular::utp::Context::kEncryptionNone;
    attempt.type = eular::utp::Context::kConnectAttemptZeroRttToken;
    attempt.session_token_size = static_cast<uint32_t>(info.session_ticket.size());
    attempt.early_data_size = static_cast<uint32_t>(info.early_data.size());
    attempt.early_fin = info.early_fin;
    return attempt;
}

eular::utp::Context::ConnectAttemptInfo MakeConnectAttemptInfo(const eular::utp::Context::Connect0RttWithStateInfo &info,
                                                               size_t stateSize,
                                                               eular::utp::Context::EncryptionMode encrypted = eular::utp::Context::kEncryptionNone)
{
    eular::utp::Context::ConnectAttemptInfo attempt;
    attempt.ip = info.ip;
    attempt.port = info.port;
    attempt.timeout = info.timeout;
    attempt.retries = info.retries;
    attempt.encrypted = encrypted;
    attempt.type = eular::utp::Context::kConnectAttemptZeroRttState;
    attempt.resumption_state_size = static_cast<uint32_t>(stateSize);
    attempt.early_data_size = static_cast<uint32_t>(info.early_data.size());
    attempt.early_fin = info.early_fin;
    return attempt;
}
} // namespace

namespace eular {
namespace utp {
ContextImpl::ContextImpl(event_base *base, Config *config) :
    m_base(base),
    m_config(config != nullptr ? *config : g_defaultConfig),
    m_udpSocket(m_config)
{
    m_pendingHandshakeTimer.reset(m_base, [this] () {
        onPendingHandshakeTimeout();
    });
    m_recvMsgScratch.resize(32);

    uint32_t id = g_contextId.fetch_add(1, std::memory_order_relaxed);
    m_tag = "[ContextImpl " + std::to_string(id) + "]";
}

ContextImpl::~ContextImpl()
{
}

const std::string &ContextImpl::tag() const
{
    return m_tag;
}

void ContextImpl::setOnConnected(const Context::OnConnected &cb)
{
    m_onConnected = cb;
}

void ContextImpl::setOnConnectError(const Context::OnConnectError &cb)
{
    m_onConnectError = cb;
}

void ContextImpl::reportConnectError(int32_t errorCode,
                                     const std::string &reason,
                                     const Context::ConnectAttemptInfo &info)
{
    if (m_onConnectError) {
        m_onConnectError(errorCode, reason, info);
    }
}

void ContextImpl::setOnNewConnection(const Context::OnNewConnection &cb)
{
    m_onNewConnection = cb;
}

void ContextImpl::setOnConnectionClosed(const Context::OnConnectionClosed &cb)
{
    m_onConnectionClosed = cb;
}

void ContextImpl::setOnZeroRttDecision(const Context::OnZeroRttDecision &cb)
{
    m_onZeroRttDecision = cb;
}

void ContextImpl::setResumptionSecret(const std::vector<uint8_t> &secret)
{
    if (secret.size() != ResumptionStateCodec::KEY_SIZE) {
        return;
    }

    std::memcpy(m_resumptionSecret.data(), secret.data(), ResumptionStateCodec::KEY_SIZE);
    m_hasCustomResumptionSecret = true;
}

void ContextImpl::clearResumptionSecret()
{
    m_resumptionSecret.fill(0);
    m_hasCustomResumptionSecret = false;
}

void ContextImpl::wantWrite(ConnectionImpl *conn)
{
    if (conn == nullptr) {
        return;
    }

    if (m_wantWriteConnSet.find(conn) != m_wantWriteConnSet.end()) {
        return;
    }

    m_wantWriteConns.push_back(conn);
    m_wantWriteConnSet.insert(conn);
    (void)m_wdrrDeficit.emplace(conn, 0u);

    if (!m_udpSocket.isValid()) {
        return;
    }

    if (m_inWriteDispatch) {
        return;
    }
    if (m_writeEvent.hasPending()) {
        return;
    }
    m_writeEvent.start();
}

bool ContextImpl::findManagedConnection(ConnectionImpl *conn, ConnectionImpl::SP &outConn)
{
    if (conn == nullptr) {
        return false;
    }

    auto it = std::find_if(m_connections.begin(), m_connections.end(), [conn] (const ConnectionMap::value_type &entry) {
        return entry.second.get() == conn;
    });
    if (it == m_connections.end()) {
        outConn.reset();
        return false;
    }

    outConn = it->second;
    return true;
}

void ContextImpl::removeFromWriteQueue(ConnectionImpl *conn)
{
    if (conn == nullptr) {
        return;
    }

    m_wantWriteConns.remove(conn);
    m_wantWriteConnSet.erase(conn);
    m_wdrrDeficit.erase(conn);
}

void ContextImpl::handleConnectionState(ConnectionImpl *conn)
{
    if (conn == nullptr) {
        return;
    }

    ConnectionImpl::SP current;
    if (!findManagedConnection(conn, current)) {
        removeFromWriteQueue(conn);
        return;
    }

    const ConnectionImpl::State state = current->state();

    auto pendingIt = m_pendingConnections.find(current.get());
    if (state == ConnectionImpl::kStateConnected) {
        if (pendingIt != m_pendingConnections.end()) {
            m_pendingConnections.erase(pendingIt);
            if (m_onConnected) {
                m_onConnected(current);
            }
        }
        return;
    }

    if (state == ConnectionImpl::kStateCloseSent || state == ConnectionImpl::kStateCloseReceived ||
        state == ConnectionImpl::kStatePtoTimedWait) {
        if (pendingIt != m_pendingConnections.end()) {
            if (pendingIt->second.retriesRemaining > 0) {
                return;
            }

            m_pendingConnections.erase(pendingIt);

            int32_t errorCode = current->lastErrorCode();
            if (errorCode == UTP_ERR_OK) {
                errorCode = UTP_ERR_CANCELLED;
            }

            std::string reason = current->lastErrorReason();
            if (reason.empty()) {
                reason = "connection closed during handshake";
            }

            reportConnectError(errorCode, reason, current->connectAttemptInfo());
        }
        return;
    }

    if (state != ConnectionImpl::kStateDisconnected) {
        return;
    }

    removeFromWriteQueue(current.get());

    if (pendingIt != m_pendingConnections.end()) {
        PendingConnectAttempt attempt = pendingIt->second;
        const int32_t retriesLeft = static_cast<int32_t>(attempt.retriesRemaining);
        m_pendingConnections.erase(pendingIt);
        auto eraseIt = std::find_if(m_connections.begin(), m_connections.end(), [conn] (const ConnectionMap::value_type &entry) {
            return entry.second.get() == conn;
        });
        if (eraseIt != m_connections.end()) {
            m_connections.erase(eraseIt);
        }

        if (retriesLeft > 0) {
            attempt.retriesRemaining = static_cast<int8_t>(retriesLeft - 1);
            if (startPendingConnectAttempt(attempt).ok()) {
                return;
            }
        }

        int32_t errorCode = current->lastErrorCode();
        if (errorCode == UTP_ERR_OK) {
            errorCode = UTP_ERR_INVALID_STATE;
        }

        std::string reason = current->lastErrorReason();
        if (reason.empty()) {
            reason = "connection failed before established";
        }

        reportConnectError(errorCode, reason, current->connectAttemptInfo());
    } else {
        if (m_onConnectionClosed) {
            m_onConnectionClosed(current);
        }
        auto eraseIt = std::find_if(m_connections.begin(), m_connections.end(), [conn] (const ConnectionMap::value_type &entry) {
            return entry.second.get() == conn;
        });
        if (eraseIt != m_connections.end()) {
            m_connections.erase(eraseIt);
        }
    }
}

Status ContextImpl::startPendingConnectAttempt(const PendingConnectAttempt &attempt)
{
    uint32_t cid = 0;
    if (!allocLocalCid(cid)) {
        return Status::ErrorLiteral(UTP_ERR_NO_MEMORY, "allocate local cid failed");
    }

    ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(this, &m_udpSocket, cid);
    const ConnectionImpl::ZeroRttConfig *zeroRtt = attempt.hasZeroRtt ? &attempt.zeroRtt : nullptr;
    const Status connStatus = conn->connect(attempt.connectInfo, zeroRtt);
    if (!connStatus.ok()) {
        return connStatus;
    }

    m_pendingConnections[conn.get()] = attempt;
    m_connections[cid] = std::move(conn);
    return Status::OK();
}

Status ContextImpl::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    Status status = m_udpSocket.bind(ip, port, ifname);
    if (!status.ok()) {
        return status;
    }

    m_writeEvent.reset(m_base, m_udpSocket.fd(), ev::EventPoll::WriteOnce, [this] (socket_t, ev::EventPoll::event_t) {
        onWriteEvent();
    });

    m_readEvent.reset(m_base, m_udpSocket.fd(), ev::EventPoll::Read | ev::EventPoll::EdgeTrigger, [this] (socket_t, ev::EventPoll::event_t) {
        onReadEvent();
    });
    m_readEvent.start();
    m_udpSocket.updateTag(tag());
    return Status::OK();
}

Status ContextImpl::connect(const Context::ConnectInfo &info)
{
    return connectInternal(info, nullptr);
}

Status ContextImpl::connectInternal(const Context::ConnectInfo &info,
                                     const ConnectionImpl::ZeroRttConfig *zeroRtt)
{
    Context::ConnectAttemptInfo attempt = MakeConnectAttemptInfo(info);
    if (zeroRtt != nullptr) {
        attempt.session_token_size = static_cast<uint32_t>(zeroRtt->sessionTicket.size());
        attempt.early_data_size = static_cast<uint32_t>(zeroRtt->earlyData.size());
        attempt.early_fin = zeroRtt->earlyFin;
        if (zeroRtt->source == ConnectionImpl::ZeroRttConfig::kSourceSessionToken) {
            attempt.type = Context::kConnectAttemptZeroRttToken;
        } else if (zeroRtt->source == ConnectionImpl::ZeroRttConfig::kSourceResumptionState) {
            attempt.type = Context::kConnectAttemptZeroRttState;
        }
    }

    if (info.ip.empty() || info.port == 0) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid connect info");
    }

    if (!m_udpSocket.isValid()) {
        return Status::ErrorLiteral(UTP_ERR_SOCKET_NOT_BOUND, "UDP socket is not bound");
    }

    for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
        const ConnectionImpl::SP &conn = it->second;
        if (conn->connectInfo() == info) {
            auto pendingIt = m_pendingConnections.find(conn.get());
            if (pendingIt != m_pendingConnections.end()) {
                return Status::ErrorLiteral(UTP_ERR_IN_PROGRESS, "connection already in progress");
            }

            return Status::ErrorLiteral(UTP_ERR_SOCKET_CONNECTED, "already connected");
        }
    }

    PendingConnectAttempt pending;
    pending.connectInfo = info;
    pending.hasZeroRtt = (zeroRtt != nullptr);
    if (zeroRtt != nullptr) {
        pending.zeroRtt = *zeroRtt;
    }
    pending.retriesRemaining = std::max<int8_t>(info.retries, 0);
    return startPendingConnectAttempt(pending);
}

Status ContextImpl::connect0Rtt(const Context::Connect0RttInfo &info)
{
    const Context::ConnectAttemptInfo attempt = MakeConnectAttemptInfo(info);
    if (info.ip.empty() || info.port == 0 || info.session_ticket.empty()) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid param");
    }

    size_t payloadSize = FRAME_SESSION_TOKEN_HDR_SIZE + info.session_ticket.size();
    if (!info.early_data.empty()) {
        payloadSize += FRAME_STREAM_HDR_SIZE + info.early_data.size();
    }
    if (UTP_HEADER_SIZE + payloadSize > 1280) {
        return Status::ErrorLiteral(UTP_ERR_OVERFLOW, "payload size exceeds max packet size");
    }

    Context::ConnectInfo base;
    base.ip = info.ip;
    base.port = info.port;
    base.timeout = info.timeout;
    base.retries = info.retries;
    base.encrypted = Context::kEncryptionNone;

    ConnectionImpl::ZeroRttConfig zeroRtt;
    zeroRtt.sessionTicket = info.session_ticket;
    zeroRtt.earlyData = info.early_data;
    zeroRtt.earlyFin = info.early_fin;
    zeroRtt.source = ConnectionImpl::ZeroRttConfig::kSourceSessionToken;
    const uint64_t nowSec = time::RealtimeMs() / 1000;
    const uint64_t lifetime = std::max<uint32_t>(m_config.zero_rtt_token_max_lifetime, 1);
    zeroRtt.expiresAtSec = nowSec + lifetime;

    return connectInternal(base, &zeroRtt);
}

Status ContextImpl::connect0RttWithState(const Context::Connect0RttWithStateInfo &info,
                                          const std::string &state)
{
    Context::ConnectAttemptInfo attempt = MakeConnectAttemptInfo(info, state.size());
    if (info.ip.empty() || info.port == 0 || state.empty()) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid connect0RttWithState input");
    }

    Context::ConnectInfo base;
    CachedResumptionState parsed;
    uint64_t expiresAt = 0;
    Status st = parseSessionResumptionState(state, parsed, expiresAt);
    if (!st.ok()) {
        return Status::Error(st.code(), fmt::format("parse resumption state failed: {}", st.message()));
    }

    attempt.encrypted = parsed.encrypted;

    const uint64_t nowSec = time::RealtimeMs() / 1000;
    if (expiresAt > 0 && nowSec > expiresAt) {
        return Status::Error(UTP_ERR_TIMEOUT, fmt::format("{} session resumption state expired: now={}, expires_at={}",
                            tag(), nowSec, expiresAt));
    }

    base.ip = info.ip;
    base.port = info.port;
    base.timeout = info.timeout;
    base.retries = info.retries;
    base.encrypted = parsed.encrypted;

    ConnectionImpl::ZeroRttConfig zeroRtt;
    zeroRtt.sessionTicket = parsed.sessionTicket;
    zeroRtt.resumptionPsk = parsed.resumptionPsk;
    zeroRtt.earlyData = info.early_data;
    zeroRtt.earlyFin = info.early_fin;
    zeroRtt.source = ConnectionImpl::ZeroRttConfig::kSourceResumptionState;
    zeroRtt.expiresAtSec = expiresAt;

    return connectInternal(base, &zeroRtt);
}

ResumptionStateCodec::Key ContextImpl::activeResumptionSecret() const
{
    if (m_hasCustomResumptionSecret) {
        return m_resumptionSecret;
    }
    return kDefaultResumptionSecret;
}


Status ContextImpl::parseSessionResumptionState(const std::string &state,
                                                 CachedResumptionState &outInfo,
                                                 uint64_t &expiresAt) const
{
    std::vector<uint8_t> sealed;
    if (!Base64::DecodeStd(state, sealed)) {
        return Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("{} invalid standard base64 session resumption state", tag()));
    }

    std::vector<uint8_t> plain;
    const auto key = activeResumptionSecret();
    if (!ResumptionStateCodec::Open(key, sealed, plain)) {
        return Status::Error(UTP_ERR_CRYPTO_DECRYPTION, fmt::format("{} failed to decrypt session resumption state", tag()));
    }

    const uint8_t *offset = plain.data();
    size_t left = plain.size();
    uint32_t version = 0;
    uint8_t modeRaw = 0;
    uint16_t ticketSize = 0;
    uint8_t pskSize = 0;

    offset = Serialize::DeserializeFrom(offset, left, version);
    offset = Serialize::DeserializeFrom(offset, left, modeRaw);
    offset = Serialize::DeserializeFrom(offset, left, expiresAt);
    offset = Serialize::DeserializeFrom(offset, left, ticketSize);
    if (offset == nullptr || left < ticketSize) {
        return Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("{} malformed session resumption state(ticket)", tag()));
    }

    outInfo.sessionTicket.assign(offset, offset + ticketSize);
    offset += ticketSize;
    left -= ticketSize;

    offset = Serialize::DeserializeFrom(offset, left, pskSize);
    if (offset == nullptr || left < pskSize) {
        return Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("{} malformed session resumption state(psk)", tag()));
    }

    outInfo.resumptionPsk.assign(offset, offset + pskSize);
    Context::EncryptionMode mode = Context::kEncryptionNone;
    if (!DecodeTokenEncryptionMode(modeRaw, mode)) {
        return Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("{} invalid encryption_mode in resumption state", tag()));
    }

    outInfo.encrypted = mode;
    if (outInfo.encrypted != Context::kEncryptionNone && outInfo.resumptionPsk.size() != ResumptionStateCodec::KEY_SIZE) {
        return Status::Error(UTP_ERR_INVALID_PARAM, fmt::format("{} invalid resumption_psk size in encrypted state: {}",
                               tag(), outInfo.resumptionPsk.size()));
    }

    (void)version;
    return Status::OK();
}

Status ContextImpl::accept()
{
    if (m_pendingIncomingQueue.empty()) {
        return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "no pending incoming connections");
    }
    const uint32_t localCid = m_pendingIncomingQueue.front();
    m_pendingIncomingQueue.pop_front();

    auto it = m_pendingIncoming.find(localCid);
    if (it == m_pendingIncoming.end()) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_STATE, "pending connection not found");
    }

    PendingIncomingConnection &pending = it->second;
    if (pending.handshakeSent) {
        return Status::ErrorLiteral(UTP_ERR_IN_PROGRESS, "handshake already sent");
    }

    if (!sendPendingHandshake(pending).ok()) {
        sendPendingConnectionClose(pending, UTP_ERR_INTERNAL_ERROR, "send handshake failed");

        Context::ConnectAttemptInfo info;
        info.ip = pending.peerIp;
        info.port = pending.peerAddress.port();
        info.timeout = m_config.handshake_timeout;
        info.encrypted = pending.encrypted;
        info.type = Context::kConnectAttemptPassive;
        reportConnectError(UTP_ERR_INTERNAL_ERROR, "send passive handshake failed", info);

        removePendingIncoming(localCid);
        return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "send passive handshake failed");
    }

    pending.handshakeSent = true;
    pending.acceptStartUs = time::MonotonicUs();
    pending.lastHandshakeSentUs = pending.acceptStartUs;
    pending.handshakeRetryCount = 0;
    m_waitHandshakeDone.insert(localCid);
    refreshPendingHandshakeTimer();

    return Status::OK();
}

bool ContextImpl::buildZeroRttSessionToken(const Address &peerAddress,
                                           uint32_t cid,
                                           Context::EncryptionMode encrypted,
                                           uint16_t &validityPeriod,
                                           std::vector<uint8_t> &outToken)
{
    outToken.clear();
    validityPeriod = 0;

    if (!peerAddress.isValid() || cid == 0) {
        return false;
    }

    const uint32_t maxLifetime = m_config.zero_rtt_token_max_lifetime;
    if (maxLifetime == 0) {
        return false;
    }

    TokenAuth *auth = tokenAuth();
    if (auth == nullptr) {
        return false;
    }

    TokenMeta meta;
    meta.token_type = static_cast<uint8_t>(TokenType::kZeroRttResumption);
    meta.timestamp = static_cast<uint32_t>(time::RealtimeMs() / 1000);
    meta.cid = cid;
    meta.encryption_mode = static_cast<uint8_t>(encrypted);
    meta.version = 1;
    meta.secret = 0;
    meta.family = static_cast<uint16_t>(peerAddress.family());

    if (peerAddress.isIPv4()) {
        sockaddr_in addr4{};
        if (!peerAddress.toSockAddrIn(addr4)) {
            return false;
        }
        meta.host_v4 = addr4.sin_addr;
    } else if (peerAddress.isIPv6()) {
        sockaddr_in6 addr6{};
        if (!peerAddress.toSockAddrIn6(addr6)) {
            return false;
        }
        meta.host_v6 = addr6.sin6_addr;
    } else {
        return false;
    }

    TokenAuth::TokenBuf tokenBuf{};
    if (!auth->seal(meta, tokenBuf)) {
        return false;
    }

    validityPeriod = static_cast<uint16_t>(std::min<uint32_t>(maxLifetime, UINT16_MAX));
    outToken.assign(tokenBuf.begin(), tokenBuf.end());
    return true;
}

Status  ContextImpl::sendPendingPacket(PendingIncomingConnection &pending,
                                       uint8_t packetType,
                                       const void *payload,
                                       size_t payloadLen,
                                       utp_packno_t *outPacketNo)
{
    if (!pending.peerAddress.isValid() || payloadLen > UINT16_MAX) {
        return Status::ErrorLiteral(UTP_ERR_INVALID_PARAM, "invalid param");
    }

    const utp_packno_t packetNo = pending.packetNumber;

    std::array<uint8_t, UTP_HEADER_SIZE> header;
    uint8_t *offset = header.data();
    size_t left = header.size();

    offset = Serialize::SerializeTo(offset, left, pending.localCid);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "serialize failed");
    offset = Serialize::SerializeTo(offset, left, pending.peerCid);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "serialize failed");
    offset = Serialize::SerializeTo(offset, left, packetNo);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "serialize failed");
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(payloadLen));
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "serialize failed");
    offset = Serialize::SerializeTo(offset, left, packetType);
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "serialize failed");
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    if (offset == nullptr) return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "serialize failed");

    UdpSocket::MsgMetaInfo msg;
    msg.data = nullptr;
    msg.len = 0;
    msg.slice_count = 0;
    msg.slices[0].data = header.data();
    msg.slices[0].len = header.size();
    msg.metaInfo.peerAddress = pending.peerAddress;
    if (payloadLen > 0 && payload != nullptr) {
        msg.slices[1].data = payload;
        msg.slices[1].len = payloadLen;
        msg.slice_count = 2;
    } else {
        msg.slice_count = 1;
    }

    Status sendStatus;
    int32_t sent = m_udpSocket.send(msg, sendStatus);
    if (sent > 0) {
        pending.packetNumber++;
        if (outPacketNo != nullptr) {
            *outPacketNo = packetNo;
        }
        return Status::OK();
    }

    if (sendStatus.ok() || sendStatus.code() == UTP_ERR_WOULD_BLOCK) {
        return Status::ErrorLiteral(UTP_ERR_WOULD_BLOCK, "UDP send would block");
    }
    return sendStatus;
}

Status  ContextImpl::sendPendingHandshake(PendingIncomingConnection &pending)
{
    FrameVersion version;
    version.version = UTP_PROTOCOL_VERSION;
    Status st;
    std::array<uint8_t, 256> payload;
    size_t                   payloadLen = 0;

    const int32_t versionLen = version.encode(payload.data() + payloadLen, payload.size() - payloadLen, st);
    if (!st.ok() || versionLen < 0) {
        return st;
    }
    payloadLen += static_cast<size_t>(versionLen);

    TransportParams localTp;
    localTp.handshake_timeout = m_config.handshake_timeout;
    localTp.init_max_streams_bidi = m_config.init_max_streams_bidi;
    localTp.init_max_streams_uni = m_config.init_max_streams_uni;
    localTp.ack_delay_exponent = m_config.ack_delay_exponent;

    FrameTransportParams transportParams;
    transportParams.params = &localTp;
    const int32_t tpLen = transportParams.encode(payload.data() + payloadLen, payload.size() - payloadLen, st);
    if (!st.ok() || tpLen < 0) {
        return st;
    }
    payloadLen += static_cast<size_t>(tpLen);

    if (pending.encrypted != Context::kEncryptionNone && pending.x25519) {
        FrameCrypto crypto;
        crypto.crypto_type = EncryptionModeToFrameCryptoType(pending.encrypted);
        crypto.eph_pubkey = const_cast<uint8_t *>(pending.x25519->publicKey().data());

        const int32_t cryptoLen = crypto.encode(payload.data() + payloadLen, payload.size() - payloadLen, st);
        if (!st.ok() || cryptoLen < 0) {
            return st;
        }
        payloadLen += static_cast<size_t>(cryptoLen);
    }

    FrameAckFrequency ackFreq;
    ackFreq.ack_eliciting_threshold =
        static_cast<uint8_t>(std::min<uint16_t>(m_config.ack_every_n_packets, UINT8_MAX));
    ackFreq.reordering_threshold = 3;
    ackFreq.max_ack_delay_ms = m_config.ack_delay;
    ackFreq.normalize();
    const int32_t ackLen = ackFreq.encode(payload.data() + payloadLen, payload.size() - payloadLen, st);
    if (!st.ok() || ackLen < 0) {
        return st;
    }
    payloadLen += static_cast<size_t>(ackLen);

    const utp_time_t nowUs = time::MonotonicUs();
    const utp_time_t baseUs = (pending.initialReceivedUs > 0 && nowUs >= pending.initialReceivedUs)
                              ? pending.initialReceivedUs
                              : nowUs;
    size_t delayLen = 0;
    if (BuildHandshakeDelayFrame(nowUs - baseUs,
                                 payload.data() + payloadLen,
                                 payload.size() - payloadLen,
                                 delayLen) != UTP_ERR_OK) {
        return Status::ErrorLiteral(UTP_ERR_INTERNAL_ERROR, "internal logic error");
    }
    payloadLen += delayLen;

    utp_packno_t sentPacketNo = 0;
    Status status = sendPendingPacket(pending,
                                             UTP_TYPE_HANDSHAKE,
                                             payload.data(),
                                             payloadLen,
                                             &sentPacketNo);
    if (status.ok()) {
        pending.lastHandshakePacketNo = sentPacketNo;
        HandshakeTracePrint("server pending handshake sent: peer=%s:%u local_cid=%u peer_cid=%u pn=%" PRIu64 " retry=%u",
                            pending.peerIp.c_str(), pending.peerAddress.port(), pending.localCid, pending.peerCid,
                            sentPacketNo, static_cast<unsigned>(pending.handshakeRetryCount));
    } else {
        HandshakeTracePrint("server pending handshake send failed: peer=%s:%u local_cid=%u code=%d message=%s",
                            pending.peerIp.c_str(), pending.peerAddress.port(), pending.localCid,
                            status.code(), status.message());
    }

    return status;
}

Status  ContextImpl::sendPendingConnectionClose(PendingIncomingConnection &pending,
                                                uint16_t errorCode,
                                                const std::string &reason)
{
    FrameConnectionClose close;
    close.error_code = errorCode;
    close.reason_phrase = reason;
    close.reason_length = static_cast<uint16_t>(reason.size());

    std::vector<uint8_t> payload(static_cast<size_t>(close.frameSize()));
    Status st;
    int32_t frameLen = close.encode(payload.data(), payload.size(), st);
    if (!st.ok()) {
        return st;
    }

    return sendPendingPacket(pending,
                             UTP_TYPE_CONNECTION_CLOSE,
                             payload.data(),
                             static_cast<size_t>(frameLen));
}

bool ContextImpl::decodeIncomingPendingPacket(const UdpSocket::MsgMetaInfo &msg,
                                              PendingIncomingConnection &pending,
                                              PacketIn &packet)
{
    return detail::DecodeUdpPacketWithOptionalAead(msg, m_mm, pending.aesCtx, packet);
}

void ContextImpl::parsePendingNegotiationFrame(PendingIncomingConnection &pending,
                                               uint8_t frameType,
                                               const uint8_t *frameData,
                                               size_t frameLen)
{
    if (frameType == kFrameTransportParams) {
        TransportParams peerTp;
        FrameTransportParams transportParams;
        transportParams.params = &peerTp;
        Status st;
        transportParams.decode(frameData, frameLen, st);
        if (st.ok()) {
            pending.peerTp = peerTp;
        }
        return;
    }

    if (frameType == kFrameAckFrequency) {
        FrameAckFrequency ackFreq;
        Status st;
        ackFreq.decode(frameData, frameLen, st);
        if (st.ok()) {
            ackFreq.normalize();
            pending.peerAckFrequency = ackFreq;
            pending.hasPeerAckFrequency = true;
        }
    }
}

ConnectionImpl::SP ContextImpl::createAndInsertPassiveConnection(uint32_t localCid,
                                                                 const Context::ConnectInfo &info,
                                                                 const Address &peerAddress,
                                                                 uint32_t peerCid,
                                                                 const TransportParams &peerTp,
                                                                 const FrameAckFrequency *peerAckFrequency,
                                                                 const std::shared_ptr<X25519Wrapper> &x25519,
                                                                 const std::shared_ptr<AesGcmContext> &aesCtx,
                                                                 const std::string &collisionReason,
                                                                 uint32_t sessionTokenSize)
{
    ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(this, &m_udpSocket, localCid);
    if (conn->initPassive(info, peerAddress, peerCid, peerTp, peerAckFrequency, x25519, aesCtx) != UTP_ERR_OK) {
        return ConnectionImpl::SP();
    }

    auto inserted = m_connections.emplace(localCid, conn);
    if (!inserted.second) {
        Context::ConnectAttemptInfo attempt;
        attempt.ip = info.ip;
        attempt.port = info.port;
        attempt.timeout = info.timeout;
        attempt.encrypted = info.encrypted;
        attempt.type = Context::kConnectAttemptPassive;
        attempt.session_token_size = sessionTokenSize;
        reportConnectError(UTP_ERR_CID_CONFLICT, collisionReason, attempt);
        return ConnectionImpl::SP();
    }

    return conn;
}

void ContextImpl::replayBufferedPendingPackets(ConnectionImpl *conn,
                                               const std::deque<std::vector<uint8_t>> &buffered,
                                               const UdpSocket::MsgMetaInfo &templateMsg)
{
    if (conn == nullptr) {
        return;
    }

    for (const auto &cached : buffered) {
        if (cached.size() < UTP_HEADER_SIZE) {
            continue;
        }

        UdpSocket::MsgMetaInfo replay{};
        replay.data = cached.data();
        replay.len = cached.size();
        replay.metaInfo = templateMsg.metaInfo;
        conn->onUdpPacket(replay);
    }
}

void ContextImpl::reportZeroRttDecision(const PendingIncomingConnection &pending,
                                        bool accepted,
                                        const std::string &reason)
{
    if (m_onZeroRttDecision) {
        Context::ZeroRttDecisionInfo info;
        info.remote_ip = pending.peerIp;
        info.remote_port = pending.peerAddress.port();
        info.local_cid = pending.localCid;
        info.peer_cid = pending.peerCid;
        info.accepted = accepted;
        info.reason = reason;
        m_onZeroRttDecision(info);
    }
}

TokenAuth *ContextImpl::tokenAuth()
{
    if (m_tokenAuth) {
        return m_tokenAuth.get();
    }

    if (m_base == nullptr) {
        return nullptr;
    }

    try {
        m_tokenAuth = std::make_unique<TokenAuth>(m_base);
    } catch (const std::exception &) {
        m_tokenAuth.reset();
    }

    return m_tokenAuth.get();
}

bool ContextImpl::validateZeroRttTicket(const Address &peerAddress,
                                        const std::vector<uint8_t> &ticket,
                                        uint16_t validityPeriod,
                                        uint32_t &ticketCid,
                                        Context::EncryptionMode &encryptionMode)
{
    if (!peerAddress.isValid() || ticket.size() != TOKEN_SIZE) {
        return false;
    }

    TokenAuth *auth = tokenAuth();
    if (auth == nullptr) {
        return false;
    }

    TokenAuth::TokenBuf tokenBuf{};
    std::memcpy(tokenBuf.data(), ticket.data(), tokenBuf.size());

    TokenMeta tokenMeta;
    if (!auth->open(tokenBuf, tokenMeta, TokenType::kZeroRttResumption)) {
        return false;
    }

    if (tokenMeta.family != static_cast<uint16_t>(peerAddress.family())) {
        return false;
    }

    if (peerAddress.isIPv4()) {
        sockaddr_in addr4{};
        if (!peerAddress.toSockAddrIn(addr4)
            || std::memcmp(&addr4.sin_addr, &tokenMeta.host_v4, sizeof(addr4.sin_addr)) != 0) {
            return false;
        }
    } else if (peerAddress.isIPv6()) {
        sockaddr_in6 addr6{};
        if (!peerAddress.toSockAddrIn6(addr6)
            || std::memcmp(&addr6.sin6_addr, &tokenMeta.host_v6, sizeof(addr6.sin6_addr)) != 0) {
            return false;
        }
    } else {
        return false;
    }

    const uint64_t nowSec = time::RealtimeMs() / 1000;
    if (nowSec < tokenMeta.timestamp) {
        return false;
    }

    uint32_t maxLifetime = m_config.zero_rtt_token_max_lifetime;
    if (maxLifetime == 0) {
        return false;
    }
    if (validityPeriod > 0) {
        maxLifetime = std::min<uint32_t>(maxLifetime, validityPeriod);
    }

    if ((nowSec - tokenMeta.timestamp) > maxLifetime) {
        return false;
    }

    if (!DecodeTokenEncryptionMode(tokenMeta.encryption_mode, encryptionMode)) {
        return false;
    }

    ticketCid = tokenMeta.cid;
    return true;
}

void ContextImpl::notePathValidationStarted()
{
    ++m_stat.path_validation_started;
}

void ContextImpl::notePathValidationSucceeded()
{
    ++m_stat.path_validation_succeeded;
}

void ContextImpl::notePathValidationFailed()
{
    ++m_stat.path_validation_failed;
}

void ContextImpl::noteZeroRttInvalidTicketRejected()
{
    ++m_stat.zero_rtt_invalid_ticket_rejected;
}

void ContextImpl::purgeZeroRttReplayCache(uint64_t nowMs)
{
    if (m_zeroRttReplayCache.empty()) {
        return;
    }

    for (auto it = m_zeroRttReplayCache.begin(); it != m_zeroRttReplayCache.end();) {
        if (it->second <= nowMs) {
            it = m_zeroRttReplayCache.erase(it);
        } else {
            ++it;
        }
    }
}

bool ContextImpl::rememberZeroRttNonce(uint32_t ticketCid, uint64_t nonce, uint64_t nowMs)
{
    if (ticketCid == 0) {
        return false;
    }

    if (nowMs == 0) {
        nowMs = time::RealtimeMs();
    }

    purgeZeroRttReplayCache(nowMs);

    ZeroRttReplayKey key;
    key.ticketCid = ticketCid;
    key.nonce = nonce;

    auto it = m_zeroRttReplayCache.find(key);
    if (it != m_zeroRttReplayCache.end()) {
        return false;
    }

    const uint32_t replayWindowS = std::max<uint32_t>(m_config.zero_rtt_replay_window, 1);
    const uint64_t expireAtMs = nowMs + static_cast<uint64_t>(replayWindowS) * 1000ULL;
    m_zeroRttReplayCache.emplace(key, expireAtMs);
    return true;
}

bool ContextImpl::allocLocalCid(uint32_t &cid)
{
    constexpr int32_t kMaxTry = 8;
    for (int32_t i = 0; i < kMaxTry; ++i) {
        const uint32_t candidate = Random<uint32_t>(1, UINT32_MAX);
        if (candidate == 0) {
            continue;
        }
        if (m_connections.find(candidate) != m_connections.end()) {
            continue;
        }
        if (m_pendingIncoming.find(candidate) != m_pendingIncoming.end()) {
            continue;
        }
        cid = candidate;
        return true;
    }

    cid = 0;
    return false;
}

void ContextImpl::removePendingIncoming(uint32_t localCid)
{
    auto it = m_pendingIncoming.find(localCid);
    if (it == m_pendingIncoming.end()) {
        return;
    }

    PeerIndexKey key;
    key.peerAddress = it->second.peerAddress;
    key.peerCid = it->second.peerCid;
    m_pendingIncomingPeerIndex.erase(key);
    m_waitHandshakeDone.erase(localCid);
    m_pendingIncomingQueue.remove(localCid);
    m_pendingIncoming.erase(it);
}

void ContextImpl::processPendingHandshakeTimeouts()
{
    if (m_waitHandshakeDone.empty()) {
        return;
    }

    const utp_time_t nowUs = time::MonotonicUs();
    std::vector<uint32_t> expired;
    expired.reserve(m_waitHandshakeDone.size());

    for (uint32_t localCid : m_waitHandshakeDone) {
        auto it = m_pendingIncoming.find(localCid);
        if (it == m_pendingIncoming.end()) {
            expired.push_back(localCid);
            continue;
        }

        PendingIncomingConnection &pending = it->second;
        if (!pending.handshakeSent || pending.lastHandshakeSentUs == 0) {
            continue;
        }

        const uint64_t retryDueUs = pendingHandshakeRetryDueUs(pending);
        if (retryDueUs == UINT64_MAX || static_cast<uint64_t>(nowUs) < retryDueUs) {
            continue;
        }

        if (pending.handshakeRetryCount < PendingHandshakeMaxRetries(m_config)) {
            if (sendPendingHandshake(pending) == UTP_ERR_OK) {
                pending.lastHandshakeSentUs = nowUs;
                ++pending.handshakeRetryCount;
                HandshakeTracePrint("server pending handshake retry advanced: peer=%s:%u local_cid=%u next_retry=%u",
                                    pending.peerIp.c_str(), pending.peerAddress.port(), pending.localCid,
                                    static_cast<unsigned>(pending.handshakeRetryCount));
            }
        } else {
            HandshakeTracePrint("server pending handshake timeout: peer=%s:%u local_cid=%u last_pn=%" PRIu64,
                                pending.peerIp.c_str(), pending.peerAddress.port(), pending.localCid,
                                pending.lastHandshakePacketNo);
            expired.push_back(localCid);
        }
    }

    for (uint32_t localCid : expired) {
        auto it = m_pendingIncoming.find(localCid);
        if (it == m_pendingIncoming.end()) {
            continue;
        }

        PendingIncomingConnection pending = it->second;
        (void)sendPendingConnectionClose(pending, UTP_ERR_TIMEOUT, "wait handshake done timeout");

        Context::ConnectAttemptInfo info;
        info.ip = pending.peerIp;
        info.port = pending.peerAddress.port();
        info.timeout = pendingHandshakeBaseTimeoutMs(pending);
        info.encrypted = pending.encrypted;
        info.type = Context::kConnectAttemptPassive;
        reportConnectError(UTP_ERR_TIMEOUT, "wait handshake done timeout", info);

        removePendingIncoming(localCid);
    }
}

void ContextImpl::refreshPendingHandshakeTimer()
{
    if (m_waitHandshakeDone.empty()) {
        m_pendingHandshakeTimer.stop();
        return;
    }

    const utp_time_t nowUs = time::MonotonicUs();
    uint64_t nextDueUs = UINT64_MAX;

    for (uint32_t localCid : m_waitHandshakeDone) {
        auto it = m_pendingIncoming.find(localCid);
        if (it == m_pendingIncoming.end()) {
            continue;
        }

        const PendingIncomingConnection &pending = it->second;
        if (!pending.handshakeSent || pending.lastHandshakeSentUs == 0) {
            continue;
        }

        const uint64_t retryDueUs = pendingHandshakeRetryDueUs(pending);
        if (retryDueUs < nextDueUs) {
            nextDueUs = retryDueUs;
        }
    }

    if (nextDueUs == UINT64_MAX) {
        m_pendingHandshakeTimer.stop();
        return;
    }

    uint32_t delayMs = 1;
    if (nextDueUs > static_cast<uint64_t>(nowUs)) {
        const uint64_t deltaUs = nextDueUs - static_cast<uint64_t>(nowUs);
        delayMs = static_cast<uint32_t>(std::max<uint64_t>(1, (deltaUs + 999ULL) / 1000ULL));
    }

    m_pendingHandshakeTimer.stop();
    m_pendingHandshakeTimer.start(delayMs);
}

uint32_t ContextImpl::pendingHandshakeBaseTimeoutMs(const PendingIncomingConnection &pending) const
{
    return PendingHandshakeBaseTimeoutMs(m_config, pending.peerTp);
}

uint64_t ContextImpl::pendingHandshakeRetryDueUs(const PendingIncomingConnection &pending) const
{
    return PendingHandshakeRetryDueUs(m_config, pending.peerTp, pending.lastHandshakeSentUs, pending.handshakeRetryCount);
}

void ContextImpl::onPendingHandshakeTimeout()
{
    processPendingHandshakeTimeouts();
    refreshPendingHandshakeTimer();
}

void ContextImpl::onReadEvent()
{
    processPendingHandshakeTimeouts();
    refreshPendingHandshakeTimer();

    while (true) {
        Status recvStatus;
        int32_t nread = m_udpSocket.recv(m_recvMsgScratch, recvStatus);
        if (nread <= 0) {
            return;
        }

        const utp_time_t nowUs = time::MonotonicUs();
        for (int32_t i = 0; i < nread; ++i) {
            const UdpSocket::MsgMetaInfo &msg = m_recvMsgScratch[static_cast<size_t>(i)];
            if (msg.data == nullptr || msg.len < UTP_HEADER_SIZE) {
                continue;
            }

            const uint8_t *offset = static_cast<const uint8_t *>(msg.data);
            size_t left = msg.len;
            uint32_t scid = 0;
            uint32_t dcid = 0;
            offset = eular::Serialize::DeserializeFrom(offset, left, scid);
            if (offset == nullptr) {
                continue;
            }

            offset = eular::Serialize::DeserializeFrom(offset, left, dcid);
            if (offset == nullptr) {
                continue;
            }

            uint64_t pn = 0;
            uint16_t payloadLen = 0;
            uint8_t packetType = UTP_TYPE_NONE;
            uint8_t reserve = 0;
            offset = eular::Serialize::DeserializeFrom(offset, left, pn);
            if (offset == nullptr) {
                continue;
            }
            offset = eular::Serialize::DeserializeFrom(offset, left, payloadLen);
            if (offset == nullptr) {
                continue;
            }
            offset = eular::Serialize::DeserializeFrom(offset, left, packetType);
            if (offset == nullptr) {
                continue;
            }
            offset = eular::Serialize::DeserializeFrom(offset, left, reserve);
            if (offset == nullptr) {
                continue;
            }

            auto it = m_connections.find(dcid);
            if (it != m_connections.end()) {
                it->second->onUdpPacket(msg, nowUs);

                handleConnectionState(it->second.get());
                continue;
            }

            if (packetType == UTP_TYPE_0RTT) {
                auto existingZeroRttConn = std::find_if(m_connections.begin(),
                                                        m_connections.end(),
                                                        [&] (const ConnectionMap::value_type &entry) {
                                                            if (!entry.second) {
                                                                return false;
                                                            }
                                                            const Connection::Description desc = entry.second->description();
                                                            const Context::ConnectInfo &peerInfo = entry.second->connectInfo();
                                                            return desc.dcid == scid
                                                                && peerInfo.port == msg.metaInfo.peerAddress.port()
                                                                && peerInfo.ip == msg.metaInfo.peerAddress.toIpString();
                                                        });
                if (existingZeroRttConn != m_connections.end()) {
                    existingZeroRttConn->second->onUdpPacket(msg);
                    handleConnectionState(existingZeroRttConn->second.get());
                    continue;
                }
            }

            auto pendingIt = m_pendingIncoming.find(dcid);
            UTP_LOGD_FMT("{} received packet with dcid {}, scid {}, pn {}, type {}, pending handshake: {}",
                      tag(), dcid, scid, pn, packetType, (pendingIt != m_pendingIncoming.end() ? pendingIt->second.handshakeSent : false));
            if (pendingIt != m_pendingIncoming.end() && pendingIt->second.handshakeSent) {
                auto packetReleaser = [this] (PacketIn *pkt) {
                    m_mm.releasePacketIn(pkt);
                };
                std::unique_ptr<PacketIn, decltype(packetReleaser)> pendingPacket(
                    m_mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
                if (!pendingPacket) {
                    continue;
                }
                bool handshakeDone = false;
                bool decodeOk = decodeIncomingPendingPacket(msg, pendingIt->second, *pendingPacket);
                if (decodeOk) {
                    size_t frameOffset = 0;
                    while (frameOffset < pendingPacket->payload_size) {
                        FrameType frameType = kFrameInvalid;
                        const uint8_t *frameData = nullptr;
                        size_t frameLen = 0;
                        Status nextSt;
                        if (pendingPacket->nextFrame(frameOffset, frameType, frameData, frameLen, nextSt) < 0) {
                            break;
                        }

                        parsePendingNegotiationFrame(pendingIt->second,
                                                     static_cast<uint8_t>(frameType),
                                                     frameData,
                                                     frameLen);

                        if (frameType == kFrameHandshakeDone) {
                            FrameHandshakeDone done;
                            Status st;
                            done.decode(frameData, frameLen, st);
                            if (st.ok()
                                && done.ack_handshake_pn == pendingIt->second.lastHandshakePacketNo) {
                                handshakeDone = true;
                            }
                            break;
                        }
                    }
                }
                UTP_LOGD_FMT("{} pending packet decode {}, handshake done: {}", tag(), (decodeOk ? "succeeded" : "failed"), handshakeDone);

                if (!handshakeDone) {
                    if (decodeOk
                        && msg.data != nullptr
                        && msg.len >= UTP_HEADER_SIZE
                        && pendingIt->second.bufferedBeforeHandshakeDone.size() < PendingPreHandshakeBufferMaxPackets(m_config)
                        && (pendingIt->second.bufferedBeforeHandshakeDoneBytes + static_cast<size_t>(msg.len)) <= PendingPreHandshakeBufferMaxBytes(m_config)) {
                        std::vector<uint8_t> cached(static_cast<size_t>(msg.len));
                        std::memcpy(cached.data(), msg.data, static_cast<size_t>(msg.len));
                        pendingIt->second.bufferedBeforeHandshakeDoneBytes += static_cast<size_t>(msg.len);
                        pendingIt->second.bufferedBeforeHandshakeDone.emplace_back(std::move(cached));
                    }
                    continue;
                }

                PendingIncomingConnection pending = std::move(pendingIt->second);

                Context::ConnectInfo info;
                info.ip = pending.peerIp;
                info.port = pending.peerAddress.port();
                info.timeout = m_config.handshake_timeout;
                info.encrypted = pending.encrypted;

                ConnectionImpl::SP conn = createAndInsertPassiveConnection(
                    pending.localCid,
                    info,
                    pending.peerAddress,
                    pending.peerCid,
                    pending.peerTp,
                    pending.hasPeerAckFrequency ? &pending.peerAckFrequency : nullptr,
                    pending.x25519,
                    pending.aesCtx,
                    "local cid collision while promoting passive connection");
                if (!conn) {
                    continue;
                }

                removePendingIncoming(dcid);
                if (m_onConnected) {
                    m_onConnected(conn);
                }
                replayBufferedPendingPackets(conn.get(), pending.bufferedBeforeHandshakeDone, msg);
                conn->onUdpPacket(msg);
                continue;
            }

            if (packetType == UTP_TYPE_0RTT) {
                auto packetReleaser = [this] (PacketIn *pkt) {
                    m_mm.releasePacketIn(pkt);
                };
                std::unique_ptr<PacketIn, decltype(packetReleaser)> packet(
                    m_mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
                if (!packet) {
                    continue;
                }
                if (!detail::DecodeUdpPacketWithOptionalAead(msg, m_mm, nullptr, *packet)) {
                    continue;
                }

                FrameSessionToken sessionToken;
                bool hasSessionToken = false;
                size_t frameOffset = 0;
                while (frameOffset < packet->payload_size) {
                    FrameType frameType = kFrameInvalid;
                    const uint8_t *frameData = nullptr;
                    size_t frameLen = 0;
                    Status nextSt;
                    if (packet->nextFrame(frameOffset, frameType, frameData, frameLen, nextSt) < 0) {
                        break;
                    }
                    if (frameType == kFrameSessionToken) {
                        Status st;
                        sessionToken.decode(frameData, frameLen, st);
                        if (st.ok()) {
                            hasSessionToken = true;
                        }
                        break;
                    }
                }

                if (!hasSessionToken) {
                    continue;
                }

                ++m_stat.zero_rtt_offered;

                uint32_t ticketCid = 0;
                Context::EncryptionMode ticketEncryptionMode = Context::kEncryptionNone;
                if (!validateZeroRttTicket(msg.metaInfo.peerAddress,
                                           sessionToken.token,
                                           sessionToken.token_validity_period,
                                           ticketCid,
                                           ticketEncryptionMode)) {
                    PendingIncomingConnection decision;
                    decision.localCid = 0;
                    decision.peerAddress = msg.metaInfo.peerAddress;
                    decision.peerIp = msg.metaInfo.peerAddress.toIpString();
                    decision.peerCid = scid;
                    decision.zeroRttAccepted = false;
                    ++m_stat.zero_rtt_rejected;
                    noteZeroRttInvalidTicketRejected();
                    reportZeroRttDecision(decision, false, "invalid_ticket");
                    (void)sendPendingConnectionClose(decision, UTP_ERR_INVALID_PARAM, "invalid_ticket");
                    continue;
                }

                if (!rememberZeroRttNonce(ticketCid, pn)) {
                    PendingIncomingConnection decision;
                    decision.localCid = 0;
                    decision.peerAddress = msg.metaInfo.peerAddress;
                    decision.peerIp = msg.metaInfo.peerAddress.toIpString();
                    decision.peerCid = scid;
                    decision.zeroRttAccepted = false;
                    ++m_stat.zero_rtt_rejected;
                    ++m_stat.zero_rtt_replay_rejected;
                    reportZeroRttDecision(decision, false, "replay");
                    (void)sendPendingConnectionClose(decision, UTP_ERR_CANCELLED, "replay");
                    continue;
                }

                PendingIncomingConnection decision;
                decision.localCid = 0;
                decision.peerAddress = msg.metaInfo.peerAddress;
                decision.peerIp = msg.metaInfo.peerAddress.toIpString();
                decision.peerCid = scid;
                decision.zeroRttAccepted = true;
                ++m_stat.zero_rtt_accepted;
                reportZeroRttDecision(decision, true, "accepted");

                Context::NewConnectionInfo newInfo;
                newInfo.remote_ip = msg.metaInfo.peerAddress.toIpString();
                newInfo.remote_port = msg.metaInfo.peerAddress.port();
                newInfo.local_cid = 0;
                newInfo.peer_cid = scid;
                newInfo.encrypted = ticketEncryptionMode;

                bool accepted = true;
                if (m_onNewConnection) {
                    accepted = m_onNewConnection(newInfo);
                }
                if (!accepted) {
                    PendingIncomingConnection decision;
                    decision.localCid = 0;
                    decision.peerAddress = msg.metaInfo.peerAddress;
                    decision.peerIp = msg.metaInfo.peerAddress.toIpString();
                    decision.peerCid = scid;
                    (void)sendPendingConnectionClose(decision, UTP_ERR_CANCELLED, "connection rejected");
                    continue;
                }

                uint32_t localCid = 0;
                if (!allocLocalCid(localCid)) {
                    continue;
                }

                Context::ConnectInfo info;
                info.ip = newInfo.remote_ip;
                info.port = newInfo.remote_port;
                info.timeout = m_config.handshake_timeout;
                info.encrypted = ticketEncryptionMode;

                ConnectionImpl::SP conn = createAndInsertPassiveConnection(
                    localCid,
                    info,
                    msg.metaInfo.peerAddress,
                    scid,
                    TransportParams{},
                    nullptr,
                    nullptr,
                    nullptr,
                    "local cid collision while creating 0-rtt connection",
                    static_cast<uint32_t>(sessionToken.token.size()));
                if (!conn) {
                    continue;
                }

                PendingIncomingConnection sendCtx;
                sendCtx.localCid = localCid;
                sendCtx.peerCid = scid;
                sendCtx.peerAddress = msg.metaInfo.peerAddress;
                sendCtx.peerIp = msg.metaInfo.peerAddress.toIpString();
                sendCtx.initialReceivedUs = time::MonotonicUs();
                sendCtx.encrypted = ticketEncryptionMode;
                (void)sendPendingHandshake(sendCtx);

                if (m_onConnected) {
                    m_onConnected(conn);
                }
                conn->onUdpPacket(msg);
                continue;
            }

            if (packetType != UTP_TYPE_INITIAL) {
                continue;
            }

            const std::string peerIp = msg.metaInfo.peerAddress.toIpString();
            const uint16_t peerPort = msg.metaInfo.peerAddress.port();
            bool knownPeerConnection = false;
            for (const auto &entry : m_connections) {
                if (!entry.second) {
                    continue;
                }

                const Connection::Description desc = entry.second->description();
                const Context::ConnectInfo &peerInfo = entry.second->connectInfo();
                if (desc.dcid == scid
                    && peerInfo.port == peerPort
                    && peerInfo.ip == peerIp) {
                    knownPeerConnection = true;
                    break;
                }
            }
            if (knownPeerConnection) {
                continue;
            }

            PeerIndexKey key;
            key.peerAddress = msg.metaInfo.peerAddress;
            key.peerCid = scid;
            if (m_pendingIncomingPeerIndex.find(key) != m_pendingIncomingPeerIndex.end()) {
                continue;
            }

            PacketIn initialPacket;
            if (!initialPacket.decode(msg.data, msg.len).ok()) {
                continue;
            }

            uint32_t localCid = 0;
            if (!allocLocalCid(localCid)) {
                continue;
            }

            PendingIncomingConnection pending;
            pending.localCid = localCid;
            pending.peerCid = scid;
            pending.peerAddress = msg.metaInfo.peerAddress;
            pending.peerIp = msg.metaInfo.peerAddress.toIpString();
            pending.initialReceivedUs = time::MonotonicUs();
            pending.encrypted = initialPacket.hasFrame(kFrameCrypto)
                             ? Context::kEncryptionAesGcm128
                             : Context::kEncryptionNone;

            size_t frameOffset = 0;
            while (frameOffset < initialPacket.payload_size) {
                FrameType frameType = kFrameInvalid;
                const uint8_t *frameData = nullptr;
                size_t frameLen = 0;
                Status nextSt;
                if (initialPacket.nextFrame(frameOffset, frameType, frameData, frameLen, nextSt) < 0) {
                    break;
                }

                parsePendingNegotiationFrame(pending,
                                             static_cast<uint8_t>(frameType),
                                             frameData,
                                             frameLen);

                if (frameType == kFrameCrypto && pending.aesCtx == nullptr) {
                    std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> peerPubKey{};
                    FrameCrypto crypto;
                    crypto.eph_pubkey = peerPubKey.data();
                    Status st;
                    crypto.decode(frameData, frameLen, st);
                    if (st.ok()) {
                        pending.encrypted = FrameCryptoTypeToEncryptionMode(crypto.crypto_type);
                        if (!pending.x25519) {
                            pending.x25519 = std::make_shared<X25519Wrapper>();
                        }

                        try {
                            X25519Wrapper::PublicKey peerPublicKey;
                            std::memcpy(peerPublicKey.data(), peerPubKey.data(), peerPublicKey.size());

                            pending.aesCtx = std::make_shared<AesGcmContext>();
                            const uint32_t noncePrefix = pending.localCid ^ pending.peerCid;
                            if (!InitAesContextByCryptoType(crypto.crypto_type,
                                                            peerPublicKey,
                                                            pending.x25519,
                                                            pending.aesCtx,
                                                            noncePrefix)) {
                                pending.aesCtx.reset();
                            }
                        } catch (const std::exception &) {
                            pending.aesCtx.reset();
                        }
                    }
                    continue;
                }

                if (frameType == kFrameSessionToken) {
                    FrameSessionToken sessionToken;
                    Status st;
                    sessionToken.decode(frameData, frameLen, st);
                    if (!st.ok()) {
                        continue;
                    }

                    pending.zeroRttOffered = true;
                    ++m_stat.zero_rtt_offered;
                    Context::EncryptionMode ticketEncryptionMode = Context::kEncryptionNone;
                    pending.zeroRttAccepted = validateZeroRttTicket(msg.metaInfo.peerAddress,
                                                                    sessionToken.token,
                                                                    sessionToken.token_validity_period,
                                                                    pending.zeroRttTokenCid,
                                                                    ticketEncryptionMode);
                    if (pending.zeroRttAccepted) {
                        if (pending.encrypted == Context::kEncryptionNone) {
                            pending.encrypted = ticketEncryptionMode;
                        } else if (pending.encrypted != ticketEncryptionMode) {
                            pending.zeroRttAccepted = false;
                        }
                    }
                    if (pending.zeroRttAccepted) {
                        ++m_stat.zero_rtt_accepted;
                        reportZeroRttDecision(pending, true, "accepted");
                    } else {
                        ++pending.zeroRttRejectedCount;
                        ++m_stat.zero_rtt_rejected;
                        noteZeroRttInvalidTicketRejected();
                        reportZeroRttDecision(pending, false, "invalid_ticket");
                    }
                }
            }

            m_pendingIncomingPeerIndex.emplace(key, localCid);
            m_pendingIncoming.emplace(localCid, pending);
            m_pendingIncomingQueue.push_back(localCid);

            Context::NewConnectionInfo info;
            info.remote_ip = pending.peerIp;
            info.remote_port = pending.peerAddress.port();
            info.local_cid = pending.localCid;
            info.peer_cid = pending.peerCid;
            info.encrypted = pending.encrypted;

            bool accepted = true;
            if (m_onNewConnection) {
                accepted = m_onNewConnection(info);
            }

            auto queuedPendingIt = m_pendingIncoming.find(localCid);
            const bool alreadyAccepted = queuedPendingIt != m_pendingIncoming.end()
                                      && queuedPendingIt->second.handshakeSent;
            if (!accepted && !alreadyAccepted) {
                if (queuedPendingIt != m_pendingIncoming.end()) {
                    (void)sendPendingConnectionClose(queuedPendingIt->second,
                                                     UTP_ERR_CANCELLED,
                                                     "connection rejected");
                }
                removePendingIncoming(localCid);
            }
        }
    }
}

void ContextImpl::onWriteEvent()
{
    m_inWriteDispatch = true;
    size_t roundBudget = m_wantWriteConns.size();
    const ConnectionSchedulerMode schedulerMode = ConnectionScheduler(m_config);
    const uint32_t quantum = ConnectionWdrQuantum(m_config);
    const uint32_t deficitCap = ConnectionWdrDeficitCap(m_config);

    while (roundBudget-- > 0 && !m_wantWriteConns.empty()) {
        ConnectionImpl *conn = m_wantWriteConns.front();
        m_wantWriteConns.pop_front();
        m_wantWriteConnSet.erase(conn);

        if (conn == nullptr) {
            continue;
        }

        ConnectionImpl::SP current;
        if (!findManagedConnection(conn, current)) {
            m_wdrrDeficit.erase(conn);
            continue;
        }

        const uint64_t txBefore = current->statistic().tx_bytes;

        if (schedulerMode == kConnectionSchedulerWdrr) {
            auto deficitIt = m_wdrrDeficit.find(conn);
            uint32_t deficit = (deficitIt == m_wdrrDeficit.end()) ? 0u : deficitIt->second;
            deficit = std::min<uint32_t>(deficit + quantum, deficitCap);
            m_wdrrDeficit[conn] = deficit;
        }

        current->onWrite();
        handleConnectionState(current.get());

        ConnectionImpl::SP aliveConn;
        if (!findManagedConnection(conn, aliveConn)) {
            m_wdrrDeficit.erase(conn);
            continue;
        }

        if (schedulerMode == kConnectionSchedulerWdrr) {
            const uint64_t txAfter = aliveConn->statistic().tx_bytes;
            const uint64_t sentBytes = (txAfter >= txBefore) ? (txAfter - txBefore) : 0;
            const uint32_t sent = static_cast<uint32_t>(std::min<uint64_t>(sentBytes, UINT32_MAX));
            uint32_t remain = m_wdrrDeficit[conn];
            remain = (sent >= remain) ? 0u : (remain - sent);
            m_wdrrDeficit[conn] = remain;
        } else {
            m_wdrrDeficit.erase(conn);
        }
    }

    m_inWriteDispatch = false;

    if (!m_wantWriteConns.empty()) {
        m_writeEvent.start();
    }
}

} // namespace utp
} // namespace eular
