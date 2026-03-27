/*************************************************************************
    > File Name: context_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:32 PM CST
 ************************************************************************/

#include "context/context_impl.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include "proto/frame/ack_frequency.h"
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
        return aesCtx->init(key256, noncePrefix);
    }

    auto sharedSecretShort = x25519->deriveSharedSecretShort(peerPublicKey);
    eular::utp::AesGcmContext::AesKey128 key128;
    std::copy(sharedSecretShort.begin(), sharedSecretShort.end(), key128.begin());
    return aesCtx->init(key128, noncePrefix);
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

    auto exists = std::find(m_wantWriteConns.begin(), m_wantWriteConns.end(), conn);
    if (exists != m_wantWriteConns.end()) {
        return;
    }

    m_wantWriteConns.push_back(conn);
    if (m_writeEvent.hasPending()) {
        return;
    }
    m_writeEvent.start();
}

void ContextImpl::handleConnectionState(ConnectionImpl *conn)
{
    if (conn == nullptr) {
        return;
    }

    auto it = std::find_if(m_connections.begin(), m_connections.end(), [conn] (const ConnectionMap::value_type &entry) {
        return entry.second.get() == conn;
    });
    if (it == m_connections.end()) {
        return;
    }

    ConnectionImpl::SP current = it->second;
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
            m_pendingConnections.erase(pendingIt);

            int32_t errorCode = current->lastErrorCode();
            if (errorCode == UTP_ERR_OK) {
                errorCode = UTP_ERR_CANCELLED;
            }

            std::string reason = current->lastErrorReason();
            if (reason.empty()) {
                reason = "connection closed during handshake";
            }

            if (m_onConnectError) {
                reportConnectError(errorCode, reason, current->connectAttemptInfo());
            }
        }
        return;
    }

    if (state != ConnectionImpl::kStateDisconnected) {
        return;
    }

    if (pendingIt != m_pendingConnections.end()) {
        m_pendingConnections.erase(pendingIt);

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
    }

    m_connections.erase(it);
}

int32_t ContextImpl::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    int32_t status = m_udpSocket.bind(ip, port, ifname);
    if (status != UTP_ERR_OK) {
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
    return UTP_ERR_OK;
}

int32_t ContextImpl::connect(const Context::ConnectInfo &info)
{
    return connectInternal(info, nullptr);
}

int32_t ContextImpl::connectInternal(const Context::ConnectInfo &info,
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
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} invalid connect info: {}:{}", tag(), info.ip, info.port);
        return -1;
    }

    if (!m_udpSocket.isValid()) {
        SetLastErrorV(UTP_ERR_SOCKET_NOT_BOUND, "{} UDP socket is not bound", tag());
        return -1;
    }

    for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
        const ConnectionImpl::SP &conn = it->second;
        if (conn->connectInfo() == info) {
            auto pendingIt = m_pendingConnections.find(conn.get());
            if (pendingIt != m_pendingConnections.end()) {
                SetLastErrorV(UTP_ERR_IN_PROGRESS, "{} connection to {}:{} is already in progress", tag(), info.ip, info.port);
                return -1;
            }

            SetLastErrorV(UTP_ERR_SOCKET_CONNECTED, "{} already connected to {}:{}", tag(), info.ip, info.port);
            return -1;
        }
    }

    uint32_t cid = 0;
    if (!allocLocalCid(cid)) {
        SetLastErrorV(UTP_ERR_NO_MEMORY, "{} allocate local cid failed", tag());
        return -1;
    }

    ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(this, &m_udpSocket, cid);
    int32_t status = conn->connect(info, zeroRtt);
    if (status != UTP_ERR_OK) {
        return status;
    }

    m_pendingConnections.insert(conn.get());
    m_connections[cid] = std::move(conn);
    return status;
}

int32_t ContextImpl::connect0Rtt(const Context::Connect0RttInfo &info)
{
    const Context::ConnectAttemptInfo attempt = MakeConnectAttemptInfo(info);
    if (info.ip.empty() || info.port == 0 || info.session_ticket.empty()) {
        return -UTP_ERR_INVALID_PARAM;
    }

    size_t payloadSize = FRAME_SESSION_TOKEN_HDR_SIZE + info.session_ticket.size();
    if (!info.early_data.empty()) {
        payloadSize += FRAME_STREAM_HDR_SIZE + info.early_data.size();
    }
    if (UTP_HEADER_SIZE + payloadSize > 1280) {
        return -UTP_ERR_OVERFLOW;
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

int32_t ContextImpl::connect0RttWithState(const Context::Connect0RttWithStateInfo &info,
                                          const std::string &state)
{
    Context::ConnectAttemptInfo attempt = MakeConnectAttemptInfo(info, state.size());
    if (info.ip.empty() || info.port == 0 || state.empty()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} invalid connect0RttWithState input: {}:{} state_size={}",
                      tag(), info.ip, info.port, state.size());
        return -1;
    }

    Context::ConnectInfo base;
    CachedResumptionState parsed;
    uint64_t expiresAt = 0;
    int32_t status = parseSessionResumptionState(state, parsed, expiresAt);
    if (status != UTP_ERR_OK) {
        return -1;
    }

    Context::ConnectInfo base;
    CachedResumptionState parsed;
    uint64_t expiresAt = 0;
    int32_t status = parseSessionResumptionState(state, parsed, expiresAt);
    if (status != UTP_ERR_OK) {
        return -1;
    }
    attempt.encrypted = parsed.encrypted;

    const uint64_t nowSec = time::RealtimeMs() / 1000;
    if (expiresAt > 0 && nowSec > expiresAt) {
        SetLastErrorV(UTP_ERR_TIMEOUT, "{} session resumption state expired: now={}, expires_at={}",
                      tag(), nowSec, expiresAt);
        return -1;
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


int32_t ContextImpl::parseSessionResumptionState(const std::string &state,
                                                 CachedResumptionState &outInfo,
                                                 uint64_t &expiresAt) const
{
    std::vector<uint8_t> sealed;
    if (!Base64::DecodeStd(state, sealed)) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} invalid standard base64 session resumption state", tag());
        return -1;
    }

    std::vector<uint8_t> plain;
    const auto key = activeResumptionSecret();
    if (!ResumptionStateCodec::Open(key, sealed, plain)) {
        SetLastErrorV(UTP_ERR_CRYPTO_DECRYPTION, "{} failed to decrypt session resumption state", tag());
        return -1;
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
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} malformed session resumption state(ticket)", tag());
        return -1;
    }

    outInfo.sessionTicket.assign(offset, offset + ticketSize);
    offset += ticketSize;
    left -= ticketSize;

    offset = Serialize::DeserializeFrom(offset, left, pskSize);
    if (offset == nullptr || left < pskSize) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} malformed session resumption state(psk)", tag());
        return -1;
    }

    outInfo.resumptionPsk.assign(offset, offset + pskSize);
    Context::EncryptionMode mode = Context::kEncryptionNone;
    if (!DecodeTokenEncryptionMode(modeRaw, mode)) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} invalid encryption_mode in resumption state", tag());
        return -1;
    }

    outInfo.encrypted = mode;
    if (outInfo.encrypted != Context::kEncryptionNone && outInfo.resumptionPsk.size() != ResumptionStateCodec::KEY_SIZE) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} invalid resumption_psk size in encrypted state: {}",
                      tag(), outInfo.resumptionPsk.size());
        return -1;
    }

    (void)version;
    return UTP_ERR_OK;
}

int32_t ContextImpl::accept()
{
    if (m_pendingIncomingQueue.empty()) {
        return UTP_ERR_WOULD_BLOCK;
    }
    const uint32_t localCid = m_pendingIncomingQueue.front();
    m_pendingIncomingQueue.pop_front();

    auto it = m_pendingIncoming.find(localCid);
    if (it == m_pendingIncoming.end()) {
        return UTP_ERR_INVALID_STATE;
    }

    PendingIncomingConnection &pending = it->second;
    if (pending.handshakeSent) {
        return UTP_ERR_IN_PROGRESS;
    }

    if (sendPendingHandshake(pending) != UTP_ERR_OK) {
        sendPendingConnectionClose(pending, UTP_ERR_INTERNAL_ERROR, "send handshake failed");

        Context::ConnectAttemptInfo info;
        info.ip = pending.peerIp;
        info.port = pending.peerAddress.port();
        info.timeout = m_config.handshake_timeout;
        info.encrypted = pending.encrypted;
        info.type = Context::kConnectAttemptPassive;
        reportConnectError(UTP_ERR_INTERNAL_ERROR, "send passive handshake failed", info);

        removePendingIncoming(localCid);
        return UTP_ERR_INTERNAL_ERROR;
    }

    pending.handshakeSent = true;
    pending.acceptStartUs = time::MonotonicUs();
    m_waitHandshakeDone.insert(localCid);
    m_pendingHandshakeTimer.stop();
    m_pendingHandshakeTimer.start(m_config.handshake_timeout);

    return UTP_ERR_OK;
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

std::string ContextImpl::PeerKey(const Address &peerAddress, uint32_t peerCid)
{
    return peerAddress.toString() + "#" + std::to_string(peerCid);
}

int32_t ContextImpl::sendPendingPacket(PendingIncomingConnection &pending,
                                       uint8_t packetType,
                                       const void *payload,
                                       size_t payloadLen)
{
    if (!pending.peerAddress.isValid() || payloadLen > UINT16_MAX) {
        return -1;
    }

    std::vector<uint8_t> wire(UTP_HEADER_SIZE + payloadLen, 0);
    uint8_t *offset = wire.data();
    size_t left = wire.size();

    offset = Serialize::SerializeTo(offset, left, pending.localCid);
    if (offset == nullptr) return -1;
    offset = Serialize::SerializeTo(offset, left, pending.peerCid);
    if (offset == nullptr) return -1;
    offset = Serialize::SerializeTo(offset, left, pending.packetNumber++);
    if (offset == nullptr) return -1;
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(payloadLen));
    if (offset == nullptr) return -1;
    offset = Serialize::SerializeTo(offset, left, packetType);
    if (offset == nullptr) return -1;
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    if (offset == nullptr) return -1;

    if (payloadLen > 0 && payload != nullptr) {
        std::memcpy(offset, payload, payloadLen);
    }

    UdpSocket::MsgMetaInfo msg{};
    msg.data = wire.data();
    msg.len = wire.size();
    msg.metaInfo.peerAddress = pending.peerAddress;

    std::vector<UdpSocket::MsgMetaInfo> msgVec(1, msg);
    int32_t sent = m_udpSocket.send(msgVec);
    return (sent > 0) ? UTP_ERR_OK : -1;
}

int32_t ContextImpl::sendPendingHandshake(PendingIncomingConnection &pending)
{
    auto appendAckFrequency = [this](std::vector<uint8_t> &payload) -> int32_t {
        FrameAckFrequency ackFreq;
        ackFreq.ack_eliciting_threshold = static_cast<uint8_t>(std::min<uint16_t>(m_config.ack_every_n_packets, UINT8_MAX));
        ackFreq.reordering_threshold = 3;
        ackFreq.max_ack_delay_ms = m_config.ack_delay;
        ackFreq.timestamp = time::MonotonicMs();

        const size_t oldSize = payload.size();
        payload.resize(oldSize + FRAME_ACK_FREQUENCY_SIZE, 0);
        const int32_t encoded = ackFreq.encode(payload.data() + oldSize, FRAME_ACK_FREQUENCY_SIZE);
        if (encoded < 0) {
            return -1;
        }
        payload.resize(oldSize + static_cast<size_t>(encoded));
        return UTP_ERR_OK;
    };

    if (pending.encrypted != Context::kEncryptionNone) {
        if (!pending.x25519) {
            pending.x25519 = std::make_shared<X25519Wrapper>();
        }

        TransportParams localTp;
        localTp.handshake_timeout = m_config.handshake_timeout;
        localTp.init_max_streams_bidi = m_config.init_max_streams_bidi;
        localTp.init_max_streams_uni = m_config.init_max_streams_uni;
        localTp.ack_delay_exponent = m_config.ack_delay_exponent;
        localTp.max_ack_delay = m_config.ack_delay;

        FrameCrypto crypto;
        crypto.crypto_type = EncryptionModeToFrameCryptoType(pending.encrypted);
        crypto.tp_size = static_cast<uint8_t>(TransportParams::kMaxNumeric);
        crypto.tp = &localTp;
        crypto.eph_pubkey = const_cast<uint8_t *>(pending.x25519->publicKey().data());

        std::vector<uint8_t> payload(FRAME_CRYPTO_SIZE, 0);
        int32_t frameLen = crypto.encode(payload.data(), payload.size());
        if (frameLen < 0) {
            return -1;
        }
        payload.resize(static_cast<size_t>(frameLen));

        if (appendAckFrequency(payload) != UTP_ERR_OK) {
            return -1;
        }

        return sendPendingPacket(pending, UTP_TYPE_HANDSHAKE, payload.data(), payload.size());
    }

    FrameVersion version;
    version.version = 1;
    std::vector<uint8_t> payload(FRAME_VERSION_SIZE, 0);
    int32_t frameLen = version.encode(payload.data(), payload.size());
    if (frameLen < 0) {
        return -1;
    }
    payload.resize(static_cast<size_t>(frameLen));

    if (appendAckFrequency(payload) != UTP_ERR_OK) {
        return -1;
    }

    return sendPendingPacket(pending, UTP_TYPE_HANDSHAKE, payload.data(), payload.size());
}

int32_t ContextImpl::sendPendingConnectionClose(PendingIncomingConnection &pending,
                                                uint16_t errorCode,
                                                const std::string &reason)
{
    FrameConnectionClose close;
    close.error_code = errorCode;
    close.reason_phrase = reason;
    close.reason_length = static_cast<uint16_t>(reason.size());

    std::vector<uint8_t> payload(static_cast<size_t>(close.frameSize()));
    int32_t frameLen = close.encode(payload.data(), payload.size());
    if (frameLen < 0) {
        return -1;
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
    if (msg.data == nullptr || msg.len < UTP_HEADER_SIZE) {
        return false;
    }

    std::memcpy(const_cast<uint8_t *>(packet.raw_data), msg.data, msg.len);
    packet.raw_size = msg.len;
    if (packet.decode(packet.raw_data, packet.raw_size) == UTP_ERR_OK) {
        return true;
    }

    if (!pending.aesCtx) {
        return false;
    }

    auto packetReleaser = [this] (PacketIn *pkt) {
        m_mm.putPacketIn(pkt);
    };
    std::unique_ptr<PacketIn, decltype(packetReleaser)> encryptedPacket(
        m_mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
    if (!encryptedPacket) {
        return false;
    }

    std::memcpy(const_cast<uint8_t *>(encryptedPacket->raw_data), msg.data, msg.len);
    encryptedPacket->raw_size = msg.len;

    const uint8_t *offset = encryptedPacket->raw_data;
    size_t left = encryptedPacket->raw_size;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.scid);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.dcid);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.pn);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.payload_length);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.types);
    if (offset == nullptr) return false;
    offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.reserve);
    if (offset == nullptr) return false;

    if (left < encryptedPacket->header.payload_length) {
        return false;
    }

    encryptedPacket->payload = offset;
    encryptedPacket->payload_size = encryptedPacket->header.payload_length;
    if (pending.aesCtx->decrypt(encryptedPacket.get()) != UTP_ERR_OK) {
        return false;
    }

    // PacketIn::decode overwrites raw_data/raw_size with the input buffer.
    // Keep ownership single by decoding from packet's own storage.
    std::memcpy(const_cast<uint8_t *>(packet.raw_data),
                encryptedPacket->raw_data,
                encryptedPacket->raw_size);
    packet.raw_size = encryptedPacket->raw_size;

    return packet.decode(packet.raw_data, packet.raw_size) == UTP_ERR_OK;
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

    const std::string key = PeerKey(it->second.peerAddress, it->second.peerCid);
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
    const uint64_t timeoutUs = static_cast<uint64_t>(m_config.handshake_timeout) * 1000ULL;

    std::vector<uint32_t> expired;
    expired.reserve(m_waitHandshakeDone.size());

    for (uint32_t localCid : m_waitHandshakeDone) {
        auto it = m_pendingIncoming.find(localCid);
        if (it == m_pendingIncoming.end()) {
            expired.push_back(localCid);
            continue;
        }

        const PendingIncomingConnection &pending = it->second;
        if (pending.acceptStartUs == 0) {
            continue;
        }

        if (nowUs >= pending.acceptStartUs && (nowUs - pending.acceptStartUs) >= timeoutUs) {
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
        info.timeout = m_config.handshake_timeout;
        info.encrypted = pending.encrypted;
        info.type = Context::kConnectAttemptPassive;
        reportConnectError(UTP_ERR_TIMEOUT, "wait handshake done timeout", info);

        removePendingIncoming(localCid);
    }
}

void ContextImpl::onPendingHandshakeTimeout()
{
    processPendingHandshakeTimeouts();
    if (!m_waitHandshakeDone.empty()) {
        m_pendingHandshakeTimer.start(m_config.handshake_timeout);
    }
}

void ContextImpl::onReadEvent()
{
    while (true) {
        processPendingHandshakeTimeouts();

        std::vector<UdpSocket::MsgMetaInfo> msgVec;
        int32_t nread = m_udpSocket.recv(msgVec);
        if (nread <= 0) {
            return;
        }

        for (const UdpSocket::MsgMetaInfo &msg : msgVec) {
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
                it->second->onUdpPacket(msg);

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
                    m_mm.putPacketIn(pkt);
                };
                std::unique_ptr<PacketIn, decltype(packetReleaser)> pendingPacket(
                    m_mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
                if (!pendingPacket) {
                    continue;
                }
                bool handshakeDone = false;
                bool decodeOk = decodeIncomingPendingPacket(msg, pendingIt->second, *pendingPacket);
                if (decodeOk) {
                    handshakeDone = pendingPacket->hasFrame(kFrameHandshakeDone);
                }
                UTP_LOGD_FMT("{} pending packet decode {}, handshake done: {}", tag(), (decodeOk ? "succeeded" : "failed"), handshakeDone);

                if (!handshakeDone) {
                    continue;
                }

                PendingIncomingConnection pending = pendingIt->second;

                Context::ConnectInfo info;
                info.ip = pending.peerIp;
                info.port = pending.peerAddress.port();
                info.timeout = m_config.handshake_timeout;
                info.encrypted = pending.encrypted;

                ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(this, &m_udpSocket, pending.localCid);
                if (conn->initPassive(info,
                                      pending.peerAddress,
                                      pending.peerCid,
                                      pending.peerTp,
                                      pending.x25519,
                                      pending.aesCtx) != UTP_ERR_OK) {
                    continue;
                }

                auto inserted = m_connections.emplace(pending.localCid, conn);
                if (!inserted.second) {
                    Context::ConnectAttemptInfo attempt;
                    attempt.ip = info.ip;
                    attempt.port = info.port;
                    attempt.timeout = info.timeout;
                    attempt.encrypted = info.encrypted;
                    attempt.type = Context::kConnectAttemptPassive;
                    reportConnectError(UTP_ERR_CID_CONFLICT,
                                       "local cid collision while promoting passive connection",
                                       attempt);
                    continue;
                }

                removePendingIncoming(dcid);
                if (m_onConnected) {
                    m_onConnected(conn);
                }
                conn->onUdpPacket(msg);
                continue;
            }

            if (packetType == UTP_TYPE_0RTT) {
                auto packetReleaser = [this] (PacketIn *pkt) {
                    m_mm.putPacketIn(pkt);
                };
                std::unique_ptr<PacketIn, decltype(packetReleaser)> packet(
                    m_mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
                if (!packet) {
                    continue;
                }
                std::memcpy(const_cast<uint8_t *>(packet->raw_data), msg.data, msg.len);
                packet->raw_size = msg.len;
                if (packet->decode(packet->raw_data, packet->raw_size) < 0) {
                    continue;
                }

                FrameSessionToken sessionToken;
                bool hasSessionToken = false;
                size_t frameOffset = 0;
                while (frameOffset < packet->payload_size) {
                    FrameType frameType = kFrameInvalid;
                    const uint8_t *frameData = nullptr;
                    size_t frameLen = 0;
                    if (packet->nextFrame(frameOffset, frameType, frameData, frameLen) < 0) {
                        break;
                    }
                    if (frameType == kFrameSessionToken) {
                        if (sessionToken.decode(frameData, frameLen) >= 0) {
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

                ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(this, &m_udpSocket, localCid);
                if (conn->initPassive(info,
                                      msg.metaInfo.peerAddress,
                                      scid,
                                      TransportParams{},
                                      nullptr,
                                      nullptr) != UTP_ERR_OK) {
                    continue;
                }

                auto inserted = m_connections.emplace(localCid, conn);
                if (!inserted.second) {
                    Context::ConnectAttemptInfo attempt;
                    attempt.ip = info.ip;
                    attempt.port = info.port;
                    attempt.timeout = info.timeout;
                    attempt.encrypted = info.encrypted;
                    attempt.type = Context::kConnectAttemptPassive;
                    attempt.session_token_size = static_cast<uint32_t>(sessionToken.token.size());
                    reportConnectError(UTP_ERR_CID_CONFLICT,
                                       "local cid collision while creating 0-rtt connection",
                                       attempt);
                    continue;
                }

                PendingIncomingConnection sendCtx;
                sendCtx.localCid = localCid;
                sendCtx.peerCid = scid;
                sendCtx.peerAddress = msg.metaInfo.peerAddress;
                sendCtx.peerIp = msg.metaInfo.peerAddress.toIpString();
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

            const std::string key = PeerKey(msg.metaInfo.peerAddress, scid);
            if (m_pendingIncomingPeerIndex.find(key) != m_pendingIncomingPeerIndex.end()) {
                continue;
            }

            PacketIn initialPacket;
            if (initialPacket.decode(msg.data, msg.len) < 0) {
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
            pending.encrypted = initialPacket.hasFrame(kFrameCrypto)
                             ? Context::kEncryptionAesGcm128
                             : Context::kEncryptionNone;

            size_t frameOffset = 0;
            while (frameOffset < initialPacket.payload_size) {
                FrameType frameType = kFrameInvalid;
                const uint8_t *frameData = nullptr;
                size_t frameLen = 0;
                if (initialPacket.nextFrame(frameOffset, frameType, frameData, frameLen) < 0) {
                    break;
                }

                if (frameType == kFrameCrypto && pending.aesCtx == nullptr) {
                    TransportParams peerTp;
                    std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> peerPubKey{};
                    FrameCrypto crypto;
                    crypto.tp = &peerTp;
                    crypto.eph_pubkey = peerPubKey.data();
                    if (crypto.decode(frameData, frameLen) >= 0) {
                        pending.encrypted = FrameCryptoTypeToEncryptionMode(crypto.crypto_type);
                        pending.peerTp = peerTp;
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
                    if (sessionToken.decode(frameData, frameLen) < 0) {
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
    std::list<ConnectionImpl *> conns;
    conns.swap(m_wantWriteConns);

    std::unordered_set<ConnectionImpl *> seen;

    for (ConnectionImpl *conn : conns) {
        if (conn == nullptr) {
            continue;
        }
        if (!seen.insert(conn).second) {
            continue;
        }

        auto it = std::find_if(m_connections.begin(), m_connections.end(), [conn] (const ConnectionMap::value_type &entry) {
            return entry.second.get() == conn;
        });
        if (it == m_connections.end()) {
            continue;
        }

        ConnectionImpl::SP current = it->second;
        current->onWrite();
        handleConnectionState(current.get());
    }

    if (!m_wantWriteConns.empty()) {
        m_writeEvent.start();
    }
}

} // namespace utp
} // namespace eular
