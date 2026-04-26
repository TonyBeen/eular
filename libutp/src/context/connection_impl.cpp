/*************************************************************************
    > File Name: connection_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:40:15 PM CST
 ************************************************************************/

#include "context/connection_impl.h"
#include "context/context_impl.h"
#include "context/packet_decode_helper.h"
#include "context/send_ctl.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

#if defined(UTP_COMPILER_MSVC)
#include <intrin.h>
#endif

#include <utils/serialize.hpp>

#include "utp/errno.h"
#include "util/error.h"
#include "util/random.hpp"
#include "util/time.h"

#include "proto/proto.h"
#include "proto/frame.h"
#include "proto/packet_in.h"
#include "proto/packet_out.h"
#include "proto/frame/path.h"
#include "proto/frame/version.h"
#include "proto/frame/padding.h"
#include "proto/frame/ack_frequency.h"
#include "proto/frame/session_token.h"
#include "proto/frame/ack.h"
#include "proto/frame/stream.h"
#include "proto/frame/reset_stream.h"
#include "proto/frame/handshake_done.h"
#include "proto/frame/handshake_delay.h"
#include "proto/frame/handshake_helper.h"
#include "proto/frame/crypto.h"
#include "proto/frame/transport_params.h"
#include "proto/frame/connection_close.h"

#include "crypto/x25519_wrapper.h"
#include "crypto/aes_gcm_context.h"
#include "crypto/base64.h"
#include "crypto/resumption_state_codec.h"
#include "crypto/token.h"

#include "make_unique.hpp"
#include "logger/logger.h"
#include "connection_impl.h"

namespace {

using eular::Serialize;
using eular::utp::Connection;
using eular::utp::FrameType;

constexpr uint64_t kPathValidationSendCredit = 256;
constexpr utp_time_t kMinPtoUs = 10000;
constexpr utp_time_t kDefaultPtoUs = 333333;
constexpr utp_time_t kMaxPtoUs = 60000000;
constexpr uint8_t kMaxCloseResendCount = 3;
constexpr utp_time_t kMinAckFrequencyApplyIntervalMs = 1000;
constexpr utp_time_t kMinAckFrequencySendIntervalMs = 2000;
constexpr utp_time_t kAckProfilePromoteHoldUs = 3000000;
constexpr utp_time_t kAckProfileRollbackHoldUs = 6000000;
constexpr utp_time_t kLossFrequentWindowUs = 2000000;
constexpr uint32_t kLossFrequentThreshold = 2;
constexpr size_t kMaxStreamPriorityLevels = 8;
constexpr size_t kMaxStreamSendBurstsPerFlush = 64;
constexpr utp_time_t kSchedulerStatsLogIntervalUs = 2000000;
constexpr size_t kAckPayloadScratchSize = 1500;
constexpr size_t kDefaultPayloadScratchCapacity = 2048;

struct AckProfileTuning {
    uint8_t ackThreshold;
    uint8_t reorderThreshold;
    uint32_t maxAckDelayMs;
};

AckProfileTuning TuningForProfile(eular::utp::ConnectionImpl::AckFrequencyProfile profile)
{
    switch (profile) {
    case eular::utp::ConnectionImpl::kAckProfileLossy:
        return AckProfileTuning{3, 1, 6};
    case eular::utp::ConnectionImpl::kAckProfileLatencySensitive:
        return AckProfileTuning{6, 2, 12};
    case eular::utp::ConnectionImpl::kAckProfileStable:
    default:
        return AckProfileTuning{10, 3, 25};
    }
}

FrameType FirstFrameTypeBit(uint32_t frameTypes)
{
    if (frameTypes == 0) {
        return eular::utp::kFrameInvalid;
    }

    uint32_t bitIndex = 0;
#if defined(UTP_COMPILER_MSVC)
    unsigned long firstSetBit = 0;
    if (_BitScanForward(&firstSetBit, frameTypes) == 0) {
        return eular::utp::kFrameInvalid;
    }
    bitIndex = static_cast<uint32_t>(firstSetBit);
#elif defined(UTP_COMPILER_GNU_LIKE)
    bitIndex = static_cast<uint32_t>(__builtin_ctz(frameTypes));
#else
    while (((frameTypes >> bitIndex) & 1u) == 0u) {
        ++bitIndex;
    }
#endif

    if (bitIndex >= static_cast<uint32_t>(eular::utp::kFrameMax)) {
        return eular::utp::kFrameInvalid;
    }

    return static_cast<FrameType>(bitIndex);
}

bool ResetScratchBuffer(std::vector<uint8_t> &buffer, size_t size)
{
    if (buffer.size() != size) {
        buffer.resize(size);
    }
    return true;
}

bool AppendRawBytes(std::vector<uint8_t> &buffer, const void *data, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (data == nullptr) {
        return false;
    }

    const size_t oldSize = buffer.size();
    buffer.resize(oldSize + len);
    std::memcpy(buffer.data() + oldSize, data, len);
    return true;
}

template <typename FrameT>
int32_t AppendEncodedFrame(std::vector<uint8_t> &payload,
                           const FrameT &frame,
                           size_t maxFrameSize)
{
    const size_t oldSize = payload.size();
    payload.resize(oldSize + maxFrameSize);
    const int32_t encoded = frame.encode(payload.data() + oldSize, maxFrameSize);
    if (encoded < 0) {
        payload.resize(oldSize);
        return -1;
    }

    payload.resize(oldSize + static_cast<size_t>(encoded));
    return UTP_ERR_OK;
}

bool IsSupportedStreamType(Connection::StreamType streamType)
{
    return streamType == Connection::kStreamTypeBidirectional
        || streamType == Connection::kStreamTypeUnidirectional;
}

Connection::StreamType StreamTypeFromStreamId(uint32_t streamId)
{
    return STREAM_ID_IS_UNI_DIR(streamId)
        ? Connection::kStreamTypeUnidirectional
        : Connection::kStreamTypeBidirectional;
}

bool StreamTypeMatchesId(Connection::StreamType streamType, uint32_t streamId)
{
    if (streamType == Connection::kStreamTypeAll) {
        return true;
    }

    if (streamType == Connection::kStreamTypeBidirectional) {
        return STREAM_ID_IS_BI_DIR(streamId);
    }

    if (streamType == Connection::kStreamTypeUnidirectional) {
        return STREAM_ID_IS_UNI_DIR(streamId);
    }

    return false;
}

bool IsLocalInitiatedStream(uint32_t streamId, bool isClientInitiator)
{
    return isClientInitiator ? STREAM_ID_IS_CLIENT(streamId) : STREAM_ID_IS_SERVER(streamId);
}

const char *StreamSchedulerModeToString(eular::utp::StreamSchedulerMode mode)
{
    switch (mode) {
    case eular::utp::kStreamSchedulerDisabled:
        return "DISABLED";
    case eular::utp::kStreamSchedulerDrr:
        return "DRR";
    case eular::utp::kStreamSchedulerStrict:
    default:
        return "STRICT";
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

bool FrameCryptoTypeToEncryptionContext(eular::utp::FrameCryptoType type,
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

int32_t AppendAckFrequencyFrame(const eular::utp::Config *config,
                                std::vector<uint8_t> &payload)
{
    eular::utp::FrameAckFrequency ackFreq;
    const uint16_t ackEveryN = config ? config->ack_every_n_packets : 10;
    ackFreq.ack_eliciting_threshold = static_cast<uint8_t>(std::min<uint16_t>(ackEveryN, UINT8_MAX));
    ackFreq.reordering_threshold = 3;
    ackFreq.max_ack_delay_ms = config ? config->ack_delay : 150;
    ackFreq.normalize();

    return AppendEncodedFrame(payload, ackFreq, FRAME_ACK_FREQUENCY_SIZE);
}

int32_t AppendTransportParamsFrame(const eular::utp::TransportParams &params,
                                   std::vector<uint8_t> &payload)
{
    eular::utp::TransportParams local = params;
    eular::utp::FrameTransportParams frame;
    frame.params = &local;

    return AppendEncodedFrame(payload, frame, FRAME_TRANSPORT_PARAMS_SIZE);
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
    payload.resize(targetPayloadSize);
    const int32_t encoded = padding.encode(payload.data() + oldSize, remain);
    if (encoded < 0 || static_cast<size_t>(encoded) != remain) {
        return -1;
    }

    return UTP_ERR_OK;
}

uint16_t ConfiguredMinPacketSize(const eular::utp::Config *config,
                                 eular::utp::Address::Family family)
{
    const uint16_t targetMtu = eular::utp::MtuDiscovery::NormalizeMtu(
        config == nullptr ? ETHERNET_MTU_MIN : config->mtu_min,
        family);
    return eular::utp::MtuDiscovery::PacketSizeFromMtu(targetMtu, family);
}

} // namespace

namespace eular {
namespace utp {
ConnectionImpl::ConnectionImpl(ContextImpl *ctx, UdpSocket *udpSocket, uint32_t cid) :
    m_ctx(ctx),
    m_udpSocket(udpSocket),
    m_localConnectionID(cid),
    m_networkPath(ctx && ctx->config() ? ctx->config()->keepalive_timeout : 1500,
                  ctx && ctx->config() ? static_cast<uint8_t>(ctx->config()->keepalive_probes) : 3)
{
    bootstrapLocalTransportParams();
    m_ackPayloadScratch.reserve(kAckPayloadScratchSize);
    m_payloadScratch.reserve(kDefaultPayloadScratchCapacity);
    m_bodyScratch.reserve(kDefaultPayloadScratchCapacity);
    m_sendMsgScratch.reserve(1);

    m_connTimer.reset(ctx->loop(), [this] () {
        onConnTimeout();
    });

    m_scheduleTimer.reset(ctx->loop(), [this] () {
        scheduleWrite();
    });

    m_pathValidationTimer.reset(ctx->loop(), [this] () {
        onPathValidationTimeout();
    });

    m_handshakeDoneTimer.reset(ctx->loop(), [this] () {
        onHandshakeDoneTimeout();
    });

    m_ackTimer.reset(ctx->loop(), [this] () {
        onAckTimeout();
    });

    m_keepaliveTimer.reset(ctx->loop(), [this] () {
        onKeepaliveTimeout();
    });

    m_closeDrainTimer.reset(ctx->loop(), [this] () {
        onCloseDrainTimeout();
    });

    m_sendCtl = std::make_unique<SendControl>(this, ctx);
    if (ctx != nullptr) {
        m_mtuDiscovery.init(ctx->config(), Address::IPv4);
    }

    FrameAckFrequency ackFreq;
    if (ctx != nullptr && ctx->config() != nullptr) {
        ackFreq.ack_eliciting_threshold = static_cast<uint8_t>(std::min<uint16_t>(ctx->config()->ack_every_n_packets, UINT8_MAX));
        ackFreq.reordering_threshold = 3;
        ackFreq.max_ack_delay_ms = ctx->config()->ack_delay;
    }
    ackFreq.normalize();
    m_ackElicitingThreshold = ackFreq.ack_eliciting_threshold;
    m_ackReorderingThreshold = ackFreq.reordering_threshold;
    m_ackMaxDelayMs = ackFreq.max_ack_delay_ms;
    if (m_sendCtl) {
        m_sendCtl->setReorderThreshold(m_ackReorderingThreshold);
    }
}

ConnectionImpl::~ConnectionImpl() = default;

void ConnectionImpl::bootstrapLocalTransportParams()
{
    m_loaclTP = TransportParams{};
    if (m_ctx == nullptr || m_ctx->config() == nullptr) {
        m_peerAckMaxDelayMs = FrameAckFrequency::kDefaultMaxAckDelayMs;
        return;
    }

    const Config *cfg = m_ctx->config();
    m_loaclTP.max_idle_timeout = cfg->max_idle_timeout;
    m_loaclTP.handshake_timeout = cfg->handshake_timeout;
    m_loaclTP.init_max_streams_bidi = cfg->init_max_streams_bidi;
    m_loaclTP.init_max_streams_uni = cfg->init_max_streams_uni;
    m_loaclTP.ack_delay_exponent = cfg->ack_delay_exponent;
    m_peerAckMaxDelayMs = cfg->ack_delay;
}

int32_t ConnectionImpl::connect(const Context::ConnectInfo &info, const ZeroRttConfig *zeroRtt)
{
    if (m_state != State::kStateDisconnected) {
        SetLastErrorV(UTP_ERR_INVALID_STATE, "{} connection is not in disconnected state", tag());
        return -1;
    }

    m_state = State::kStateWaitSendInitial;
    m_connectInfo = info;
    m_zeroRttConfig = zeroRtt ? *zeroRtt : ZeroRttConfig{};
    m_connectAttemptInfo.ip = info.ip;
    m_connectAttemptInfo.port = info.port;
    m_connectAttemptInfo.timeout = info.timeout;
    m_connectAttemptInfo.retries = info.retries;
    m_connectAttemptInfo.encrypted = info.encrypted;
    m_connectAttemptInfo.session_token_size = static_cast<uint32_t>(m_zeroRttConfig.sessionTicket.size());
    m_connectAttemptInfo.resumption_state_size = 0;
    m_connectAttemptInfo.early_data_size = static_cast<uint32_t>(m_zeroRttConfig.earlyData.size());
    m_connectAttemptInfo.early_fin = m_zeroRttConfig.earlyFin;
    switch (m_zeroRttConfig.source) {
    case ZeroRttConfig::kSourceSessionToken:
        m_connectAttemptInfo.type = Context::kConnectAttemptZeroRttToken;
        break;
    case ZeroRttConfig::kSourceResumptionState:
        m_connectAttemptInfo.type = Context::kConnectAttemptZeroRttState;
        break;
    case ZeroRttConfig::kSourceNone:
    default:
        m_connectAttemptInfo.type = Context::kConnectAttemptNormal;
        break;
    }

    Address peer(info.ip, info.port);
    if (!peer.isValid()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "invalid peer address {}:{}", info.ip, info.port);
        return -1;
    }

    m_peerAddress = peer;
    m_networkPath.bindPeerAddress(peer);
    m_mtuDiscovery.setAddressFamily(peer.family());
    m_lastErrorCode = UTP_ERR_OK;
    m_lastErrorReason.clear();
    m_closeByPeer = false;
    m_closedNotified = false;
    m_isClientInitiator = true;
    m_lastActivityUs = 0;
    m_keepaliveMissedProbes = 0;
    m_ackElicitingSinceLastAck = 0;
    m_ackPendingSinceUs = 0;
    m_ackProfileCurrent = kAckProfileStable;
    m_ackProfileCandidate = kAckProfileStable;
    m_ackProfileCandidateSinceUs = 0;
    m_ackProfileLastSentMs = 0;
    m_ackProfileBaselineSrttUs = 0;
    m_zeroRttEarlyDataSent = false;
    m_zeroRttEarlyStreamId = 0;
    m_sessionTokenIssued = false;
    m_hasCachedResumptionState = false;
    m_cachedResumptionInfo = CachedResumptionState{};
    m_cachedResumptionExpiresAt = 0;
    if (m_zeroRttConfig.enabled()) {
        CachedResumptionState cached;
        cached.encrypted = info.encrypted;
        cached.sessionTicket = m_zeroRttConfig.sessionTicket;
        cached.resumptionPsk = m_zeroRttConfig.resumptionPsk;

        uint64_t expiresAt = m_zeroRttConfig.expiresAtSec;
        if (expiresAt == 0 && m_ctx && m_ctx->config()) {
            const uint64_t nowSec = time::RealtimeMs() / 1000;
            const uint64_t lifetime = std::max<uint32_t>(m_ctx->config()->zero_rtt_token_max_lifetime, 1);
            expiresAt = nowSec + lifetime;
        }
        cacheSessionResumptionState(cached, expiresAt);
    }
    stopAckTimer();
    m_keepaliveTimer.stop();

    m_ctx->wantWrite(this);
    bool success = m_connTimer.start(info.timeout);
    if (!success) {
        SetLastErrorV(UTP_ERR_SOCKET_EVENT, "{} failed to start connection timer", tag());
        return -1;
    }
    return UTP_ERR_OK;
}

int32_t ConnectionImpl::initPassive(const Context::ConnectInfo &info,
                                   const Address &peerAddress,
                                   uint32_t peerConnectionID,
                                   const TransportParams &peerTp,
                                   const FrameAckFrequency *peerAckFrequency,
                                   const std::shared_ptr<X25519Wrapper> &x25519,
                                   const std::shared_ptr<AesGcmContext> &aesCtx)
{
    if (m_state != State::kStateDisconnected) {
        return UTP_ERR_INVALID_STATE;
    }

    if (!peerAddress.isValid() || peerConnectionID == 0) {
        return UTP_ERR_INVALID_PARAM;
    }

    m_connectInfo = info;
    m_connectAttemptInfo.ip = info.ip;
    m_connectAttemptInfo.port = info.port;
    m_connectAttemptInfo.timeout = info.timeout;
    m_connectAttemptInfo.retries = info.retries;
    m_connectAttemptInfo.encrypted = info.encrypted;
    m_connectAttemptInfo.type = Context::kConnectAttemptPassive;
    m_connectAttemptInfo.session_token_size = 0;
    m_connectAttemptInfo.resumption_state_size = 0;
    m_connectAttemptInfo.early_data_size = 0;
    m_connectAttemptInfo.early_fin = false;
    m_isClientInitiator = false;
    m_peerAddress = peerAddress;
    m_peerConnectionID = peerConnectionID;
    m_peerTP = peerTp;
    m_x25519 = x25519;
    m_aesCtx = aesCtx;
    m_zeroRttConfig = ZeroRttConfig{};

    m_networkPath.bindPeerAddress(peerAddress);
    m_mtuDiscovery.setAddressFamily(peerAddress.family());
    m_ackElicitingSinceLastAck = 0;
    m_ackPendingSinceUs = 0;
    m_ackProfileCurrent = kAckProfileStable;
    m_ackProfileCandidate = kAckProfileStable;
    m_ackProfileCandidateSinceUs = 0;
    m_ackProfileLastSentMs = 0;
    m_ackProfileBaselineSrttUs = 0;
    m_lastErrorCode = UTP_ERR_OK;
    m_lastErrorReason.clear();
    m_closeByPeer = false;
    m_closedNotified = false;
    m_sessionTokenIssued = false;
    m_hasCachedResumptionState = false;
    m_cachedResumptionInfo = CachedResumptionState{};
    m_cachedResumptionExpiresAt = 0;
    if (peerAckFrequency != nullptr) {
        FrameAckFrequency ackFreq = *peerAckFrequency;
        ackFreq.normalize();
        applyAckFrequency(ackFreq, 0);
    }
    stopAckTimer();

    m_state = State::kStateConnected;
    markPeerActivity(time::MonotonicUs());
    scheduleWrite();
    return UTP_ERR_OK;
}

void ConnectionImpl::onUdpPacket(const UdpSocket::MsgMetaInfo &msg)
{
    if (msg.data == nullptr || msg.len < UTP_HEADER_SIZE) {
        return;
    }

    const utp_time_t nowUs = time::MonotonicUs();

    auto packetReleaser = [this] (PacketIn *packet) {
        m_mm.putPacketIn(packet);
    };
    std::unique_ptr<PacketIn, decltype(packetReleaser)> packet(m_mm.getPacketIn(static_cast<uint32_t>(msg.len)), packetReleaser);
    if (!packet) {
        return;
    }

    if (!detail::DecodeUdpPacketWithOptionalAead(msg, m_mm, m_aesCtx, *packet)) {
        return;
    }

    const bool isPassiveInitial = (m_state == State::kStateDisconnected)
                               && packet->header.types == UTP_TYPE_INITIAL
                               && packet->header.dcid == 0;
    const bool isPassiveZeroRtt = (m_state == State::kStateConnected)
                               && packet->header.types == UTP_TYPE_0RTT
                               && packet->header.dcid == 0
                               && (m_peerConnectionID == 0 || packet->header.scid == m_peerConnectionID);
    if (packet->header.dcid != m_localConnectionID && !isPassiveInitial && !isPassiveZeroRtt) {
        return;
    }

    m_bytesIn += msg.len;
    const utp_packno_t largestBeforeInsert = m_receiveHistory.largest();
    const bool reorderedGap = largestBeforeInsert != 0
                           && packet->header.pn > largestBeforeInsert + 1
                           && (packet->header.pn - largestBeforeInsert - 1) >= m_ackReorderingThreshold;
    m_receiveHistory.insert(packet->header.pn, nowUs);
    m_peerConnectionID = packet->header.scid;

    const bool closingState = m_state == State::kStateCloseSent
                           || m_state == State::kStateCloseReceived;

    const Address packetPeerAddress = msg.metaInfo.peerAddress;
    bool fromActivePath = m_peerAddress.isValid() && (packetPeerAddress == m_peerAddress);

    // 保守策略：地址变化先进入 candidate 校验，业务路径保持 active 不切换。
    if (!fromActivePath && !closingState) {
        if (m_networkPath.detectPeerAddressChange(packetPeerAddress)) {
            if (m_ctx != nullptr) {
                m_ctx->notePathValidationStarted();
            }
            maybeSendPathChallenge();
        }
    }

    const bool fromCandidatePath = m_networkPath.needPathValidation()
                                && m_networkPath.peerAddress().isValid()
                                && (packetPeerAddress == m_networkPath.peerAddress());

    m_mtuDiscovery.setAddressFamily(packetPeerAddress.family());
    packet->meta = msg.metaInfo;

    if (m_state == State::kStateConnected) {
        markPeerActivity(nowUs);
    }

    if (m_state == State::kStatePtoTimedWait) {
        bool peerCloseInTimedWait = false;
        size_t timedWaitOffset = 0;
        while (timedWaitOffset < packet->payload_size) {
            FrameType frameType = kFrameInvalid;
            const uint8_t *frameData = nullptr;
            size_t frameLen = 0;
            if (packet->nextFrame(timedWaitOffset, frameType, frameData, frameLen) < 0) {
                break;
            }
            if (frameType == kFrameConnectionClose) {
                peerCloseInTimedWait = true;
                break;
            }
        }

        // 本端在进入 PTO timed-wait 之前已经发送过一次 CONNECTION_CLOSE。
        // 此时如果再收到对端回声式的 close，不再继续回包，避免额外多发一份 close。
        (void)peerCloseInTimedWait;
        return;
    }

    if (m_state == State::kStateDisconnected) {
        return;
    }

    size_t frameOffset = 0;
    bool peerCloseReceived = false;
    bool hasHandshakeDoneFrame = false;
    while (frameOffset < packet->payload_size) {
        FrameType frameType = kFrameInvalid;
        const uint8_t *frameData = nullptr;
        size_t frameLen = 0;
        if (packet->nextFrame(frameOffset, frameType, frameData, frameLen) < 0) {
            break;
        }

        // 进入关闭阶段后，只继续处理 ConnectionClose；其余帧直接忽略。
        if (closingState && frameType != kFrameConnectionClose) {
            continue;
        }

        // candidate 路径在验证成功前仅处理路径验证帧，业务数据继续走 active 路径。
        if (!fromActivePath && fromCandidatePath
            && frameType != kFramePathChallenge
            && frameType != kFramePathResponse
            && frameType != kFrameConnectionClose) {
            continue;
        }

        switch (frameType) {
        case kFrameStream: {
            FrameStream streamFrame;
            if (streamFrame.decode(frameData, frameLen) >= 0) {
                (void)ingestStreamFrame(streamFrame);
            }
            break;
        }
        case kFrameResetStream: {
            FrameResetStream resetFrame;
            if (resetFrame.decode(frameData, frameLen) >= 0) {
                handleResetStreamFrame(resetFrame);
            }
            break;
        }
        case kFramePathChallenge:
            handlePathChallengeFrame(frameData, frameLen, packetPeerAddress);
            break;
        case kFramePathResponse:
            handlePathResponseFrame(frameData, frameLen, packetPeerAddress);
            break;
        case kFrameAck: {
            AckInfo ackInfo;
            ackInfo.reset();

            FrameAck ack;
            ack._ackInfo = &ackInfo;
            ack._params = &m_peerTP;
            if (ack.decode(frameData, frameLen) >= 0 && m_sendCtl) {
                m_sendCtl->onAckReceived(ackInfo, nowUs);
                scheduleWrite();
            }
            break;
        }
        case kFrameAckFrequency: {
            FrameAckFrequency ackFreq;
            if (ackFreq.decode(frameData, frameLen) >= 0) {
                applyAckFrequency(ackFreq, nowUs / 1000);
            }
            break;
        }
        case kFrameTransportParams: {
            TransportParams peerTp;
            FrameTransportParams frameTp;
            frameTp.params = &peerTp;
            if (frameTp.decode(frameData, frameLen) >= 0) {
                m_peerTP = peerTp;
            }
            break;
        }
        case kFrameSessionToken: {
            FrameSessionToken sessionToken;
            if (sessionToken.decode(frameData, frameLen) >= 0 && m_ctx != nullptr && m_ctx->config() != nullptr) {
                if (sessionToken.token.size() == TOKEN_SIZE && sessionToken.token_validity_period > 0) {
                    CachedResumptionState cached;
                    cached.encrypted = Context::kEncryptionNone;
                    cached.sessionTicket = sessionToken.token;

                    uint32_t lifetime = m_ctx->config()->zero_rtt_token_max_lifetime;
                    lifetime = std::min<uint32_t>(lifetime, sessionToken.token_validity_period);
                    if (lifetime > 0) {
                        const uint64_t nowSec = time::RealtimeMs() / 1000;
                        cacheSessionResumptionState(cached, nowSec + lifetime);
                    }
                }
            }
            break;
        }
        case kFrameCrypto: {
            std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> peerPubkey{};

            FrameCrypto crypto;
            crypto.eph_pubkey = peerPubkey.data();
            if (crypto.decode(frameData, frameLen) >= 0) {
                if (!m_x25519) {
                    m_x25519 = std::make_shared<X25519Wrapper>();
                }

                try {
                    X25519Wrapper::PublicKey peerPublicKey;
                    std::memcpy(peerPublicKey.data(), peerPubkey.data(), peerPublicKey.size());

                    if (!m_aesCtx) {
                        m_aesCtx = std::make_shared<AesGcmContext>();
                    }

                    const uint32_t noncePrefix = m_localConnectionID ^ m_peerConnectionID;
                    if (!FrameCryptoTypeToEncryptionContext(crypto.crypto_type,
                                                            peerPublicKey,
                                                            m_x25519,
                                                            m_aesCtx,
                                                            noncePrefix)) {
                        SetLastErrorV(UTP_ERR_CRYPTO_INIT_FAILED,
                                      "init aes-gcm context failed after x25519 key exchange");
                    }
                } catch (const std::exception &e) {
                    SetLastErrorV(UTP_ERR_INTERNAL_ERROR, "x25519 key exchange failed: {}", e.what());
                }
            }
            break;
        }
        case kFrameConnectionClose:
            peerCloseReceived = true;
            m_state = kStateCloseReceived;
            m_closeByPeer = true;
            if (m_lastErrorCode == UTP_ERR_OK) {
                m_lastErrorCode = UTP_ERR_CANCELLED;
                m_lastErrorReason = "peer closed connection";
                notifyConnectionError(m_lastErrorCode, m_lastErrorReason, false);
            }
            break;
        case kFrameHandshakeDone:
            {
                FrameHandshakeDone done;
                if (done.decode(frameData, frameLen) >= 0) {
                    hasHandshakeDoneFrame = true;
                }
            }
            break;
        default:
            break;
        }
    }

    if (peerCloseReceived) {
        // 对端的 close 只回复一次；重复到达的 close 不再触发新的 close 发送。
        if (m_closePeerResendCount == 0) {
            m_closeErrorCode = UTP_ERR_CANCELLED;
            m_closeReason = "peer close ack";
            m_closeFramePending = true;
            m_closePtoUs = closePtoUs();
            m_closeDeadlineUs = 0;
            m_closeLastSendUs = 0;
            // 交给 onWrite() 统一发送，避免在收包路径里直接写 socket。
            scheduleWrite();
        }
        return;
    }

    if (closingState) {
        return;
    }

    maybeUpdateAckFrequency(nowUs);

    const uint32_t ackMask = (1u << static_cast<uint32_t>(kFrameAck));
    const bool ackOnly = packet->frame_types == ackMask;
    const bool handshakePacket = packet->header.types == UTP_TYPE_INITIAL
                              || packet->header.types == UTP_TYPE_HANDSHAKE;
    const bool closePacket = packet->header.types == UTP_TYPE_CONNECTION_CLOSE
                          || packet->hasFrame(kFrameConnectionClose);
    const bool suppressAck = handshakePacket || closePacket;

    if (!ackOnly && !suppressAck) {
        noteAckElicitingPacket(nowUs);
    }

    if (!suppressAck) {
        if (hasHandshakeDoneFrame && m_ackElicitingSinceLastAck > 0) {
            if (sendAckPacket(nowUs) != UTP_ERR_OK) {
                armAckTimer(1);
            }
        } else {
            const uint32_t ackThreshold = std::max<uint32_t>(1, m_ackElicitingThreshold);
            const bool ackCountReached = m_ackElicitingSinceLastAck >= ackThreshold;
            if ((ackCountReached || reorderedGap) && m_ackElicitingSinceLastAck > 0) {
                if (sendAckPacket(nowUs) != UTP_ERR_OK) {
                    armAckTimer(10);
                }
            } else if (m_ackElicitingSinceLastAck > 0) {
                armAckTimer(m_ackMaxDelayMs);
            }
        }
    }

    if (m_state == State::kStateInitialSent && packet->header.types == UTP_TYPE_HANDSHAKE) {
        m_state = State::kStateConnected;
        m_peerHandshakePacketNo = packet->header.pn;
        m_handshakeReceivedAtUs = nowUs;
        m_handshakeDonePending = true;
        m_handshakeDoneSent = false;
        m_handshakeDoneLastPacketNo = 0;
        armHandshakeDoneTimer();
        m_connTimer.stop();
        markPeerActivity(nowUs);
    }

    if (m_state == State::kStateConnected) {
        flushPendingStreamWrites();
    }
}

void ConnectionImpl::onWrite()
{
    if (m_state == State::kStatePtoTimedWait || m_state == State::kStateDisconnected) {
        return;
    }

    if (m_state == State::kStateCloseSent || m_state == State::kStateCloseReceived) {
        if (!m_closeFramePending) {
            return;
        }

        const int32_t closeStatus = sendConnectionCloseFrame();
        if (closeStatus == UTP_ERR_OK) {
            m_closeFramePending = false;
            m_closeLastSendUs = time::MonotonicUs();
            if (m_state == State::kStateCloseReceived && m_closePeerResendCount < kMaxCloseResendCount) {
                ++m_closePeerResendCount;
            }
            enterPtoTimedWait();
            return;
        }

        if (closeStatus == UTP_ERR_WOULD_BLOCK) {
            scheduleWrite();
            return;
        }

        m_closeFramePending = false;
        m_lastErrorCode = closeStatus;
        m_lastErrorReason = "send connection close failed";
        notifyConnectionError(m_lastErrorCode, m_lastErrorReason, true);
        m_state = State::kStateDisconnected;
        stopAckTimer();
        m_keepaliveTimer.stop();
        notifyConnectionClosed(m_lastErrorCode, m_lastErrorReason, m_closeByPeer);
        scheduleWrite();
        return;
    }

    if (m_state == State::kStateWaitSendInitial) {
        const bool zeroRttFirstPacket = m_zeroRttConfig.enabled();
        if (!zeroRttFirstPacket) {
            int32_t status = sendInitialPacket();
            if (status != UTP_ERR_OK) {
                int32_t err = utp_get_last_error();
                if (err <= 0) {
                    err = UTP_ERR_SOCKET_WRITE;
                }
                m_lastErrorCode = err;
                m_lastErrorReason = "send initial packet failed";
                notifyConnectionError(m_lastErrorCode, m_lastErrorReason, true);
                m_state = State::kStateDisconnected;
                m_connTimer.stop();
                m_handshakeDoneTimer.stop();
                m_pathValidationTimer.stop();
                stopAckTimer();
                m_keepaliveTimer.stop();
                notifyConnectionClosed(m_lastErrorCode, m_lastErrorReason, false);
                return;
            }
        }

        m_state = State::kStateInitialSent;
    }

    trySendZeroRttEarlyData();

    if (m_state == State::kStateConnected) {
        const int32_t tokenStatus = maybeSendSessionTokenPacket();
        if (tokenStatus == UTP_ERR_WOULD_BLOCK) {
            scheduleWrite();
        }
    }

    if (m_networkPath.needPathValidation() && !m_networkPath.hasInFlightChallenge()) {
        maybeSendPathChallenge();
    }

    if (m_sendCtl) {
        m_sendCtl->onCanWrite(time::MonotonicUs());
    }

    maybeUpdateAckFrequency(time::MonotonicUs());

    flushPendingStreamWrites();
}

void ConnectionImpl::trySendZeroRttEarlyData()
{
    const bool allowEarlyData = (m_state == State::kStateInitialSent)
                             && m_zeroRttConfig.enabled();
    if (!allowEarlyData || m_zeroRttEarlyDataSent || m_zeroRttConfig.earlyData.empty()) {
        return;
    }

    if (m_zeroRttEarlyStreamId == 0) {
        m_zeroRttEarlyStreamId = 0; // 双向流，客户端方向首个 stream id
    }

    const int32_t status = sendStreamFrame(m_zeroRttEarlyStreamId,
                                           0,
                                           m_zeroRttConfig.earlyData.data(),
                                           m_zeroRttConfig.earlyData.size(),
                                           m_zeroRttConfig.earlyFin);
    if (status == UTP_ERR_OK) {
        m_zeroRttEarlyDataSent = true;
    }
}

const Config* ConnectionImpl::config() const
{
    return m_ctx ? m_ctx->config() : nullptr;
}

void ConnectionImpl::nextScheduleTime(utp_time_t timeNext)
{
    m_scheduleTimer.start(timeNext);
}

void ConnectionImpl::scheduleWrite()
{
    if (m_ctx != nullptr) {
        m_ctx->wantWrite(this);
    }
}

int32_t ConnectionImpl::buildAckPayload(std::vector<uint8_t> &payload, utp_time_t nowUs) const
{
    if (m_receiveHistory.empty() || m_ctx == nullptr || m_peerConnectionID == 0) {
        return UTP_ERR_INVALID_STATE;
    }

    FrameAck ackFrame;
    ackFrame._history = &m_receiveHistory;
    ackFrame._params = const_cast<TransportParams *>(&m_loaclTP);
    ackFrame._config = m_ctx->config();
    ackFrame._now = nowUs;

    ResetScratchBuffer(payload, kAckPayloadScratchSize);
    const int32_t ackLen = ackFrame.encode(payload.data(), payload.size());
    if (ackLen <= 0) {
        payload.clear();
        return UTP_ERR_INTERNAL_ERROR;
    }

    payload.resize(static_cast<size_t>(ackLen));
    return UTP_ERR_OK;
}

int32_t ConnectionImpl::sendAckPacket(utp_time_t nowUs)
{
    if (m_ackElicitingSinceLastAck == 0) {
        stopAckTimer();
        return UTP_ERR_OK;
    }

    if (buildAckPayload(m_ackPayloadScratch, nowUs) != UTP_ERR_OK) {
        return UTP_ERR_INVALID_STATE;
    }

    const int32_t status = sendPacket(UTP_TYPE_CTRL,
                                      m_ackPayloadScratch.data(),
                                      m_ackPayloadScratch.size(),
                                      0,
                                      nullptr,
                                      (1u << static_cast<uint32_t>(kFrameAck)));
    if (status == UTP_ERR_OK) {
        m_ackElicitingSinceLastAck = 0;
        m_ackPendingSinceUs = 0;
        stopAckTimer();
    }

    return status;
}

void ConnectionImpl::noteAckElicitingPacket(utp_time_t nowUs)
{
    if (m_ackElicitingSinceLastAck == 0) {
        m_ackPendingSinceUs = nowUs;
    }
    ++m_ackElicitingSinceLastAck;
}

void ConnectionImpl::applyAckFrequency(const FrameAckFrequency &ackFreq, utp_time_t nowMs)
{
    if (m_lastAckFrequencyApplyMs != 0
        && nowMs > m_lastAckFrequencyApplyMs
        && (nowMs - m_lastAckFrequencyApplyMs) < kMinAckFrequencyApplyIntervalMs) {
        return;
    }

    m_ackElicitingThreshold = ackFreq.ack_eliciting_threshold;
    m_ackReorderingThreshold = ackFreq.reordering_threshold;
    m_ackMaxDelayMs = ackFreq.max_ack_delay_ms;
    m_lastAckFrequencyApplyMs = nowMs;
    if (m_sendCtl) {
        m_sendCtl->setReorderThreshold(m_ackReorderingThreshold);
    }
}

ConnectionImpl::AckFrequencyProfile ConnectionImpl::selectDesiredAckProfile(utp_time_t nowUs)
{
    const bool lossFrequent = m_sendCtl != nullptr
                           && m_sendCtl->isLossFrequent(nowUs, kLossFrequentWindowUs, kLossFrequentThreshold);
    if (lossFrequent) {
        return kAckProfileLossy;
    }

    const utp_time_t srttUs = m_rttStats.srtt();
    if (srttUs == 0) {
        return kAckProfileStable;
    }

    if (m_ackProfileBaselineSrttUs == 0) {
        m_ackProfileBaselineSrttUs = srttUs;
        return kAckProfileStable;
    }

    const utp_time_t rttDeltaThresholdUs = std::max<utp_time_t>(15000, m_ackProfileBaselineSrttUs / 4);
    if (srttUs > m_ackProfileBaselineSrttUs && (srttUs - m_ackProfileBaselineSrttUs) >= rttDeltaThresholdUs) {
        return kAckProfileLatencySensitive;
    }

    return kAckProfileStable;
}

utp_time_t ConnectionImpl::ackProfileTransitionHoldUs(AckFrequencyProfile from, AckFrequencyProfile to) const
{
    if (from == to) {
        return 0;
    }

    if (to > from) {
        return kAckProfilePromoteHoldUs;
    }

    return kAckProfileRollbackHoldUs;
}

int32_t ConnectionImpl::sendAckFrequencyUpdate(AckFrequencyProfile profile, utp_time_t nowUs)
{
    if (m_state != State::kStateConnected || m_peerConnectionID == 0) {
        return UTP_ERR_INVALID_STATE;
    }

    const AckProfileTuning tuning = TuningForProfile(profile);
    FrameAckFrequency ackFreq;
    ackFreq.ack_eliciting_threshold = tuning.ackThreshold;
    ackFreq.reordering_threshold = tuning.reorderThreshold;
    ackFreq.max_ack_delay_ms = tuning.maxAckDelayMs;
    ackFreq.normalize();

    std::array<uint8_t, FRAME_ACK_FREQUENCY_SIZE> payload{};
    const int32_t encoded = ackFreq.encode(payload.data(), payload.size());
    if (encoded <= 0) {
        return UTP_ERR_INTERNAL_ERROR;
    }

    const int32_t status = sendPacket(UTP_TYPE_CTRL,
                                      payload.data(),
                                      static_cast<size_t>(encoded),
                                      0,
                                      nullptr,
                                      (1u << static_cast<uint32_t>(kFrameAckFrequency)));
    if (status == UTP_ERR_OK) {
        m_ackProfileLastSentMs = nowUs / 1000;
        m_peerAckMaxDelayMs = ackFreq.max_ack_delay_ms;
    }

    return status;
}

void ConnectionImpl::maybeUpdateAckFrequency(utp_time_t nowUs)
{
    if (m_state != State::kStateConnected || m_peerConnectionID == 0) {
        return;
    }

    const AckFrequencyProfile desired = selectDesiredAckProfile(nowUs);
    if (desired != m_ackProfileCandidate) {
        m_ackProfileCandidate = desired;
        m_ackProfileCandidateSinceUs = nowUs;
    }

    const utp_time_t holdUs = ackProfileTransitionHoldUs(m_ackProfileCurrent, m_ackProfileCandidate);
    if (m_ackProfileCandidate != m_ackProfileCurrent) {
        if (m_ackProfileCandidateSinceUs == 0 || nowUs < m_ackProfileCandidateSinceUs) {
            return;
        }

        if ((nowUs - m_ackProfileCandidateSinceUs) < holdUs) {
            return;
        }

        const utp_time_t nowMs = nowUs / 1000;
        if (m_ackProfileLastSentMs != 0
            && nowMs > m_ackProfileLastSentMs
            && (nowMs - m_ackProfileLastSentMs) < kMinAckFrequencySendIntervalMs) {
            return;
        }

        if (sendAckFrequencyUpdate(m_ackProfileCandidate, nowUs) == UTP_ERR_OK) {
            m_ackProfileCurrent = m_ackProfileCandidate;
        }
    }

    if (m_ackProfileCurrent == kAckProfileStable) {
        const utp_time_t srttUs = m_rttStats.srtt();
        if (srttUs > 0) {
            if (m_ackProfileBaselineSrttUs == 0) {
                m_ackProfileBaselineSrttUs = srttUs;
            } else {
                m_ackProfileBaselineSrttUs = (m_ackProfileBaselineSrttUs * 7 + srttUs) / 8;
            }
        }
    }
}

void ConnectionImpl::armAckTimer(uint32_t delayMs)
{
    if (m_ackElicitingSinceLastAck == 0) {
        return;
    }

    if (m_state == State::kStateDisconnected
        || m_state == State::kStatePtoTimedWait
        || m_state == State::kStateCloseSent
        || m_state == State::kStateCloseReceived) {
        return;
    }

    m_ackTimer.stop();
    m_ackTimer.start(delayMs > 0 ? delayMs : 1);
}

void ConnectionImpl::stopAckTimer()
{
    m_ackTimer.stop();
}

void ConnectionImpl::onAckTimeout()
{
    if (m_ackElicitingSinceLastAck == 0) {
        return;
    }

    const int32_t status = sendAckPacket(time::MonotonicUs());
    if (status != UTP_ERR_OK) {
        armAckTimer(10);
    }
}

size_t ConnectionImpl::activeStreamCount(StreamType streamType) const
{
    size_t count = 0;
    for (const auto &entry : m_streams) {
        if (!entry.second || entry.second->state() == Stream::kStateClosed) {
            continue;
        }

        if (StreamTypeMatchesId(streamType, entry.first)) {
            ++count;
        }
    }
    return count;
}

size_t ConnectionImpl::activeLocalStreamCount(StreamType streamType) const
{
    size_t count = 0;
    for (const auto &entry : m_streams) {
        if (!entry.second || entry.second->state() == Stream::kStateClosed) {
            continue;
        }

        if (!StreamTypeMatchesId(streamType, entry.first)) {
            continue;
        }

        if (IsLocalInitiatedStream(entry.first, m_isClientInitiator)) {
            ++count;
        }
    }
    return count;
}

uint32_t ConnectionImpl::streamLimit(StreamType streamType, bool peerLimit) const
{
    if (!IsSupportedStreamType(streamType)) {
        return 0;
    }

    const TransportParams &tp = peerLimit ? m_peerTP : m_loaclTP;
    const uint32_t limit = streamType == kStreamTypeUnidirectional
                        ? tp.init_max_streams_uni
                        : tp.init_max_streams_bidi;
    return std::max<uint32_t>(1, limit);
}

uint32_t ConnectionImpl::streamIdSlot(StreamType streamType) const
{
    if (!IsSupportedStreamType(streamType)) {
        return STREAM_TYPES;
    }

    const uint32_t roleBase = m_isClientInitiator ? 0u : 1u;
    if (streamType == kStreamTypeUnidirectional) {
        return roleBase + 2u;
    }
    return roleBase;
}

void ConnectionImpl::collectClosedStreams()
{
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        if (!it->second) {
            it = m_streams.erase(it);
            continue;
        }

        if (it->second->state() == Stream::kStateClosed && !it->second->readable()) {
            it = m_streams.erase(it);
            continue;
        }

        ++it;
    }
}

int32_t ConnectionImpl::validateIncomingStreamId(uint32_t streamId) const
{
    const bool peerInitiated = m_isClientInitiator ? STREAM_ID_IS_SERVER(streamId)
                                                    : STREAM_ID_IS_CLIENT(streamId);
    if (!peerInitiated) {
        return UTP_ERR_STREAM_STATE_ERROR;
    }

    const StreamType streamType = StreamTypeFromStreamId(streamId);
    const uint32_t maxPeerStreams = streamLimit(streamType, false);
    const uint32_t streamOrdinal = (streamId / STREAM_TYPES) + 1;
    if (streamOrdinal > maxPeerStreams) {
        return UTP_ERR_STREAM_LIMIT_ERROR;
    }

    return UTP_ERR_OK;
}

int32_t ConnectionImpl::ingestStreamFrame(const FrameStream &streamFrame)
{
    auto it = m_streams.find(streamFrame.stream_id);
    if (it == m_streams.end()) {
        const int32_t validateStatus = validateIncomingStreamId(streamFrame.stream_id);
        if (validateStatus != UTP_ERR_OK) {
            return validateStatus;
        }

        StreamImpl::SP stream = std::make_shared<StreamImpl>(this,
                                                              streamFrame.stream_id,
                                                              defaultStreamPriority());
        auto pair = m_streams.emplace(streamFrame.stream_id, stream);
        if (!pair.second) {
            notifyConnectionError(UTP_ERR_INTERNAL_ERROR, "failed to insert new stream into map", false);
            return UTP_ERR_INTERNAL_ERROR;
        }
        if (m_onIncomingStream) {
            m_onIncomingStream(stream.get());
        }
        it = pair.first;
    }

    if (it == m_streams.end()) {
        return UTP_ERR_INTERNAL_ERROR;
    }

    const int32_t status = it->second->onFrame(streamFrame);
    collectClosedStreams();
    return status;
}

int32_t ConnectionImpl::ingestEarlyStreamFrame(uint32_t streamId,
                                               uint64_t streamOffset,
                                               const uint8_t *data,
                                               size_t len,
                                               bool fin)
{
    if (m_state != State::kStateConnected) {
        return UTP_ERR_INVALID_STATE;
    }

    if (len > UINT16_MAX) {
        return UTP_ERR_OVERFLOW;
    }

    FrameStream streamFrame;
    streamFrame.stream_flag = fin ? STREAM_SET_FIN(0) : 0;
    streamFrame.stream_data_length = static_cast<uint16_t>(len);
    streamFrame.stream_id = streamId;
    streamFrame.stream_offset = streamOffset;
    streamFrame.stream_data = const_cast<uint8_t *>(data);
    return ingestStreamFrame(streamFrame);
}

void ConnectionImpl::updateTag(const std::string& tag)
{
    m_tag = tag + " Connection [" + std::to_string(m_localConnectionID) + "]";
}

void ConnectionImpl::flushPendingStreamWrites() {
    const utp_time_t nowUs = time::MonotonicUs();
    const StreamSchedulerMode mode = streamSchedulerMode();
    noteSchedulerModeIfChanged(mode);

    auto armDeferredWakeup = [&](utp_time_t baseNowUs) {
        utp_time_t minRemainUs = 0;
        for (const auto &entry : m_streams) {
            if (!entry.second || !entry.second->hasPendingSendWork()) {
                continue;
            }

            if (!entry.second->shouldDeferSend(baseNowUs)) {
                continue;
            }

            const utp_time_t remainUs = entry.second->coalesceDelayRemainingUs(baseNowUs);
            if (remainUs == 0) {
                continue;
            }

            if (minRemainUs == 0 || remainUs < minRemainUs) {
                minRemainUs = remainUs;
            }
        }

        if (minRemainUs > 0) {
            const utp_time_t delayMs = std::max<utp_time_t>(1, (minRemainUs + 999) / 1000);
            nextScheduleTime(delayMs);
        }
    };

    size_t sentBursts = 0;
    while (sentBursts < kMaxStreamSendBurstsPerFlush) {
        StreamImpl::SP stream = pickNextWritableStream();
        if (!stream) {
            ++m_schedulerStats.emptyRounds;
            armDeferredWakeup(time::MonotonicUs());
            break;
        }

        const int32_t status = stream->onConnectionWritable();
        if (status == UTP_ERR_WOULD_BLOCK) {
            ++m_schedulerStats.wouldBlock;
            UTP_LOGD("connection %u scheduler backpressure: mode=%s stream=%u",
                     m_localConnectionID,
                     StreamSchedulerModeToString(mode),
                     stream->id());
            armDeferredWakeup(time::MonotonicUs());
            maybeEmitSchedulerStats(nowUs);
            return;
        }
        if (status != UTP_ERR_OK) {
            UTP_LOGW("connection %u scheduler stream write failed: mode=%s stream=%u status=%d",
                     m_localConnectionID,
                     StreamSchedulerModeToString(mode),
                     stream->id(),
                     status);
            break;
        }

        updateStrictAgingState(stream->id());
        ++sentBursts;
    }

    pruneSchedulerState();
    collectClosedStreams();
    maybeEmitSchedulerStats(nowUs);
}

size_t ConnectionImpl::streamPayloadBudgetHint() const
{
    size_t packetPayloadBudget = m_mtuDiscovery.currentMaxPacketSize();
    if (packetPayloadBudget <= UTP_HEADER_SIZE) {
        return 1;
    }

    packetPayloadBudget -= UTP_HEADER_SIZE;
    if (m_aesCtx != nullptr) {
        if (packetPayloadBudget <= AesGcmContext::GCM_TAG_SIZE) {
            return 1;
        }
        packetPayloadBudget -= AesGcmContext::GCM_TAG_SIZE;
    }

    if (packetPayloadBudget <= FRAME_STREAM_HDR_SIZE) {
        return 1;
    }
    return packetPayloadBudget - FRAME_STREAM_HDR_SIZE;
}

bool ConnectionImpl::canSendStreamUnackedBytes(size_t streamBytes) const
{
    if (streamBytes == 0) {
        return true;
    }

    const Config *cfg = config();
    const uint64_t limit = (cfg == nullptr) ? (256ull * 1024) : cfg->stream_unacked_data_limit;
    if (limit == 0) {
        return true;
    }

    return m_streamUnackedDataBytes + static_cast<uint64_t>(streamBytes) <= limit;
}

void ConnectionImpl::onStreamPacketUnackedAdded(const PacketOut *pkt)
{
    if (pkt == nullptr || pkt->stream_data_size == 0) {
        return;
    }

    m_streamUnackedDataBytes += pkt->stream_data_size;
}

void ConnectionImpl::onStreamPacketUnackedRemoved(const PacketOut *pkt)
{
    if (pkt == nullptr || pkt->stream_data_size == 0) {
        return;
    }

    if (m_streamUnackedDataBytes >= pkt->stream_data_size) {
        m_streamUnackedDataBytes -= pkt->stream_data_size;
    } else {
        m_streamUnackedDataBytes = 0;
    }
}

int32_t ConnectionImpl::sendStreamFrame(uint32_t streamId,
                                        uint64_t streamOffset,
                                        const uint8_t *data,
                                        size_t len,
                                        bool fin)
{
    if (m_state == State::kStateCloseSent
        || m_state == State::kStateCloseReceived
        || m_state == State::kStatePtoTimedWait) {
        SetLastErrorV(UTP_ERR_CONNECTION_CLOSING,
                      "connection {} is closing, stream {} send rejected",
                      m_localConnectionID,
                      streamId);
        return UTP_ERR_CONNECTION_CLOSING;
    }

    const bool allowEarlyData = (m_state == State::kStateInitialSent)
                             && m_zeroRttConfig.enabled();
    if (m_state != State::kStateConnected && !allowEarlyData) {
        return UTP_ERR_WOULD_BLOCK;
    }

    // 握手屏障：HandshakeDone 已发出但未确认前，阻断后续普通 Stream 外发，避免连锁丢包。
    if (!allowEarlyData
        && m_state == State::kStateConnected
        && m_handshakeDonePending
        && m_handshakeDoneSent
        && (len > 0 || fin)) {
        return UTP_ERR_WOULD_BLOCK;
    }

    if (len > UINT16_MAX) {
        return UTP_ERR_OVERFLOW;
    }

    FrameStream frame;
    frame.stream_flag = fin ? STREAM_SET_FIN(0) : 0;
    frame.stream_data_length = static_cast<uint16_t>(len);
    frame.stream_id = streamId;
    frame.stream_offset = streamOffset;
    frame.stream_data = const_cast<uint8_t *>(data);

    std::array<uint8_t, FRAME_STREAM_HDR_SIZE> header{};
    uint8_t *offset = header.data();
    size_t left = header.size();
    offset = Serialize::SerializeTo(offset, left, FrameType::kFrameStream);
    if (offset == nullptr) {
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, frame.stream_flag);
    if (offset == nullptr) {
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, frame.stream_data_length);
    if (offset == nullptr) {
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, frame.stream_id);
    if (offset == nullptr) {
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, frame.stream_offset);
    if (offset == nullptr) {
        return -1;
    }

    const bool piggybackHandshakeDone = (m_state == State::kStateConnected)
                                     && m_handshakeDonePending
                                     && len > 0;
    const uint8_t packetType = allowEarlyData ? UTP_TYPE_0RTT : UTP_TYPE_CTRL;
    std::array<uint8_t, FRAME_HANDSHAKE_DONE_SIZE + FRAME_HANDSHAKE_DELAY_SIZE> handshakeTrailer{};
    size_t handshakeTrailerSize = 0;
    const utp_time_t nowUs = (piggybackHandshakeDone || m_ackElicitingSinceLastAck > 0)
                           ? time::MonotonicUs()
                           : 0;

    if (piggybackHandshakeDone) {
        const utp_time_t baseUs = (m_handshakeReceivedAtUs > 0 && nowUs >= m_handshakeReceivedAtUs)
                                ? m_handshakeReceivedAtUs
                                : nowUs;
        if (BuildHandshakeTrailer(m_peerHandshakePacketNo,
                                  nowUs - baseUs,
                                  handshakeTrailer.data(),
                                  handshakeTrailer.size(),
                                  handshakeTrailerSize) != UTP_ERR_OK) {
            return -1;
        }
    }

    if (allowEarlyData) {
        FrameSessionToken sessionToken;
        sessionToken.token = m_zeroRttConfig.sessionTicket;
        sessionToken.token_size = static_cast<uint8_t>(sessionToken.token.size());

        const Config *cfg = (m_ctx != nullptr) ? m_ctx->config() : nullptr;
        const uint32_t lifetime = cfg ? cfg->zero_rtt_token_max_lifetime : 0;
        sessionToken.token_validity_period = static_cast<uint16_t>(std::min<uint32_t>(lifetime, UINT16_MAX));

        m_payloadScratch.clear();
        m_payloadScratch.resize(static_cast<size_t>(sessionToken.frameSize()));
        const int32_t tokenLen = sessionToken.encode(m_payloadScratch.data(), m_payloadScratch.size());
        if (tokenLen < 0) {
            return -1;
        }

        m_payloadScratch.resize(static_cast<size_t>(tokenLen));
        if (!AppendRawBytes(m_payloadScratch, header.data(), header.size())) {
            return -1;
        }
        if (!AppendRawBytes(m_payloadScratch, data, len)) {
            return -1;
        }

        const uint32_t frameBits = (1u << static_cast<uint32_t>(kFrameSessionToken))
                                | (1u << static_cast<uint32_t>(kFrameStream));
        return sendPacket(UTP_TYPE_0RTT,
                          m_payloadScratch.data(),
                          m_payloadScratch.size(),
                          0,
                          nullptr,
                          frameBits,
                          nullptr,
                          len);
    }

    if (m_ackElicitingSinceLastAck > 0) {
        if (buildAckPayload(m_ackPayloadScratch, nowUs) == UTP_ERR_OK) {
            const size_t combinedSize = m_ackPayloadScratch.size() + header.size() + len + handshakeTrailerSize;
            size_t packetPayloadBudget = m_mtuDiscovery.currentMaxPacketSize();
            if (packetPayloadBudget > UTP_HEADER_SIZE) {
                packetPayloadBudget -= UTP_HEADER_SIZE;
            } else {
                packetPayloadBudget = 0;
            }
            if (m_aesCtx != nullptr && packetPayloadBudget > AesGcmContext::GCM_TAG_SIZE) {
                packetPayloadBudget -= AesGcmContext::GCM_TAG_SIZE;
            }

            if (combinedSize <= UINT16_MAX && combinedSize <= packetPayloadBudget) {
                m_payloadScratch.clear();
                m_payloadScratch.reserve(combinedSize);
                m_payloadScratch.insert(m_payloadScratch.end(),
                                        m_ackPayloadScratch.begin(),
                                        m_ackPayloadScratch.end());
                if (!AppendRawBytes(m_payloadScratch, header.data(), header.size())) {
                    return -1;
                }
                if (!AppendRawBytes(m_payloadScratch, data, len)) {
                    return -1;
                }
                if (!AppendRawBytes(m_payloadScratch, handshakeTrailer.data(), handshakeTrailerSize)) {
                    return -1;
                }

                uint32_t frameBits = (1u << static_cast<uint32_t>(kFrameAck))
                                   | (1u << static_cast<uint32_t>(kFrameStream));
                if (piggybackHandshakeDone) {
                    frameBits |= (1u << static_cast<uint32_t>(kFrameHandshakeDone));
                    frameBits |= (1u << static_cast<uint32_t>(kFrameHandshakeDelay));
                }

                const int32_t status = sendPacket(packetType,
                                                  m_payloadScratch.data(),
                                                  m_payloadScratch.size(),
                                                  0,
                                                  piggybackHandshakeDone ? &m_handshakeDoneLastPacketNo : nullptr,
                                                  frameBits,
                                                  nullptr,
                                                  len);
                if (status == UTP_ERR_OK) {
                    m_ackElicitingSinceLastAck = 0;
                    m_ackPendingSinceUs = 0;
                    stopAckTimer();
                    if (piggybackHandshakeDone) {
                        m_handshakeDoneSent = true;
                        m_handshakeDonePending = true;
                        armHandshakeDoneTimer();
                    }
                }
                return status;
            }
        }

        const int32_t ackStatus = sendAckPacket(nowUs);
        if (ackStatus != UTP_ERR_OK && ackStatus != UTP_ERR_INVALID_STATE) {
            return ackStatus;
        }
    }

    if (piggybackHandshakeDone) {
        m_bodyScratch.clear();
        m_bodyScratch.reserve(len + handshakeTrailerSize);
        if (!AppendRawBytes(m_bodyScratch, data, len)) {
            return -1;
        }
        if (!AppendRawBytes(m_bodyScratch, handshakeTrailer.data(), handshakeTrailerSize)) {
            return -1;
        }

        const int32_t status = sendPacket(packetType,
                                          header.data(),
                                          header.size(),
                                          m_bodyScratch.data(),
                                          m_bodyScratch.size(),
                                          0,
                                          &m_handshakeDoneLastPacketNo,
                                          (1u << static_cast<uint32_t>(kFrameStream))
                                                                                | (1u << static_cast<uint32_t>(kFrameHandshakeDone))
                                                                                | (1u << static_cast<uint32_t>(kFrameHandshakeDelay)),
                                          nullptr,
                                          len);
        if (status == UTP_ERR_OK) {
            m_handshakeDoneSent = true;
            m_handshakeDonePending = true;
            armHandshakeDoneTimer();
        }
        return status;
    }

    return sendPacket(packetType,
                      header.data(),
                      header.size(),
                      data,
                      len,
                      0,
                      nullptr,
                      0,
                      nullptr,
                      len);
}

int32_t ConnectionImpl::sendConnectionCloseFrame()
{
    FrameConnectionClose closeFrame;
    closeFrame.error_code = m_closeErrorCode;
    closeFrame.reason_phrase = m_closeReason;
    closeFrame.reason_length = static_cast<uint16_t>(closeFrame.reason_phrase.size());

    std::array<uint8_t, 256> payload{};
    const int32_t frameLen = closeFrame.encode(payload.data(), payload.size());
    if (frameLen <= 0) {
        return UTP_ERR_INTERNAL_ERROR;
    }

    const int32_t status = sendPacket(UTP_TYPE_CONNECTION_CLOSE,
                                      payload.data(),
                                      static_cast<size_t>(frameLen));
    if (status == UTP_ERR_OK) {
        return UTP_ERR_OK;
    }

    int32_t err = utp_get_last_error();
    if (err <= 0) {
        err = UTP_ERR_SOCKET_WRITE;
    }
    return err;
}

int32_t ConnectionImpl::sendResetStreamFrame(uint32_t streamId, uint16_t errorCode, uint64_t finalSize)
{
    FrameResetStream resetFrame;
    resetFrame.error_code = errorCode;
    resetFrame.stream_id = streamId;
    resetFrame.final_size = finalSize;

    std::array<uint8_t, FRAME_RESET_STREAM_SIZE> payload{};
    const int32_t frameLen = resetFrame.encode(payload.data(), payload.size());
    if (frameLen <= 0) {
        return UTP_ERR_INTERNAL_ERROR;
    }

    const int32_t status = sendPacket(UTP_TYPE_CTRL,
                                      payload.data(),
                                      static_cast<size_t>(frameLen),
                                      0,
                                      nullptr,
                                      (1u << static_cast<uint32_t>(kFrameResetStream)));
    if (status == UTP_ERR_OK) {
        return UTP_ERR_OK;
    }

    int32_t err = utp_get_last_error();
    if (err <= 0) {
        err = UTP_ERR_SOCKET_WRITE;
    }
    return err;
}

void ConnectionImpl::handleResetStreamFrame(const FrameResetStream &resetFrame)
{
    auto it = m_streams.find(resetFrame.stream_id);
    if (it == m_streams.end()) {
        const int32_t validateStatus = validateIncomingStreamId(resetFrame.stream_id);
        if (validateStatus != UTP_ERR_OK) {
            return;
        }

        StreamImpl::SP stream = std::make_shared<StreamImpl>(this,
                                                              resetFrame.stream_id,
                                                              defaultStreamPriority());
        m_streams.emplace(resetFrame.stream_id, stream);
        if (m_onIncomingStream) {
            m_onIncomingStream(stream.get());
        }
        it = m_streams.find(resetFrame.stream_id);
    }

    if (it == m_streams.end() || !it->second) {
        return;
    }

    (void)it->second->onReset(resetFrame.error_code, true);
}

int32_t ConnectionImpl::sendHandshakeDonePacket()
{
    std::array<uint8_t, FRAME_HANDSHAKE_DONE_SIZE + FRAME_HANDSHAKE_DELAY_SIZE> payload{};
    size_t payloadSize = 0;

    const utp_time_t nowUs = time::MonotonicUs();
    const utp_time_t baseUs = (m_handshakeReceivedAtUs > 0 && nowUs >= m_handshakeReceivedAtUs)
                            ? m_handshakeReceivedAtUs
                            : nowUs;
    if (BuildHandshakeTrailer(m_peerHandshakePacketNo,
                              nowUs - baseUs,
                              payload.data(),
                              payload.size(),
                              payloadSize) != UTP_ERR_OK) {
        return -1;
    }

    utp_packno_t outPacketNo = 0;
    const int32_t status = sendPacket(UTP_TYPE_CTRL,
                                      payload.data(),
                                      payloadSize,
                                      0,
                                      &outPacketNo,
                                      (1u << static_cast<uint32_t>(kFrameHandshakeDone))
                                    | (1u << static_cast<uint32_t>(kFrameHandshakeDelay)));
    if (status == UTP_ERR_OK) {
        m_handshakeDoneSent = true;
        m_handshakeDonePending = true;
        m_handshakeDoneLastPacketNo = outPacketNo;
        armHandshakeDoneTimer();
    }
    return status;
}

int32_t ConnectionImpl::maybeSendSessionTokenPacket()
{
    if (m_sessionTokenIssued || m_isClientInitiator || m_state != State::kStateConnected) {
        return UTP_ERR_OK;
    }

    if (m_ctx == nullptr || m_peerConnectionID == 0 || !m_peerAddress.isValid()) {
        return UTP_ERR_WOULD_BLOCK;
    }

    uint16_t validityPeriod = 0;
    std::vector<uint8_t> token;
    if (!m_ctx->buildZeroRttSessionToken(m_peerAddress,
                                         m_localConnectionID,
                                         m_connectInfo.encrypted,
                                         validityPeriod,
                                         token)) {
        return UTP_ERR_OK;
    }

    CachedResumptionState cached;
    cached.encrypted = m_connectInfo.encrypted;
    cached.sessionTicket = token;
    const uint64_t nowSec = time::RealtimeMs() / 1000;
    cacheSessionResumptionState(cached, nowSec + validityPeriod);

    FrameSessionToken frame;
    frame.token = token;
    frame.token_size = static_cast<uint8_t>(frame.token.size());
    frame.token_validity_period = validityPeriod;

    m_payloadScratch.clear();
    m_payloadScratch.resize(static_cast<size_t>(frame.frameSize()));
    int32_t frameLen = frame.encode(m_payloadScratch.data(), m_payloadScratch.size());
    if (frameLen < 0) {
        return -1;
    }
    m_payloadScratch.resize(static_cast<size_t>(frameLen));

    const int32_t status = sendPacket(UTP_TYPE_CTRL,
                                      m_payloadScratch.data(),
                                      m_payloadScratch.size(),
                                      0,
                                      nullptr,
                                      (1u << static_cast<uint32_t>(kFrameSessionToken)));
    if (status == UTP_ERR_OK) {
        m_sessionTokenIssued = true;
    }
    return status;
}

int32_t ConnectionImpl::sendInitialPacket()
{
    m_payloadScratch.clear();
    size_t reserveSize = FRAME_VERSION_SIZE
                       + FRAME_TRANSPORT_PARAMS_SIZE
                       + FRAME_CRYPTO_SIZE
                       + FRAME_ACK_FREQUENCY_SIZE;
    if (m_zeroRttConfig.enabled()) {
        reserveSize += FRAME_SESSION_TOKEN_HDR_SIZE + m_zeroRttConfig.sessionTicket.size();
    }
    m_payloadScratch.reserve(reserveSize);

    FrameVersion version;
    version.version = UTP_PROTOCOL_VERSION;
    if (AppendEncodedFrame(m_payloadScratch, version, FRAME_VERSION_SIZE) != UTP_ERR_OK) {
        return -1;
    }

    if (AppendTransportParamsFrame(m_loaclTP, m_payloadScratch) != UTP_ERR_OK) {
        return -1;
    }

    if (m_connectInfo.encrypted != Context::kEncryptionNone) {
        if (!m_x25519) {
            m_x25519 = std::make_shared<X25519Wrapper>();
        }

        FrameCrypto crypto;
        crypto.crypto_type = EncryptionModeToFrameCryptoType(m_connectInfo.encrypted);
        crypto.eph_pubkey = const_cast<uint8_t *>(m_x25519->publicKey().data());

        if (AppendEncodedFrame(m_payloadScratch, crypto, FRAME_CRYPTO_SIZE) != UTP_ERR_OK) {
            return -1;
        }
    }

    if (m_zeroRttConfig.enabled()) {
        FrameSessionToken sessionToken;
        sessionToken.token = m_zeroRttConfig.sessionTicket;
        sessionToken.token_size = static_cast<uint8_t>(sessionToken.token.size());

        const Config *cfg = (m_ctx != nullptr) ? m_ctx->config() : nullptr;
        const uint32_t lifetime = cfg ? cfg->zero_rtt_token_max_lifetime : 0;
        sessionToken.token_validity_period = static_cast<uint16_t>(std::min<uint32_t>(lifetime, UINT16_MAX));

        const size_t oldSize = m_payloadScratch.size();
        m_payloadScratch.resize(oldSize + static_cast<size_t>(sessionToken.frameSize()));
        const int32_t tokenLen = sessionToken.encode(m_payloadScratch.data() + oldSize,
                                                     m_payloadScratch.size() - oldSize);
        if (tokenLen < 0) {
            return -1;
        }
        m_payloadScratch.resize(oldSize + static_cast<size_t>(tokenLen));
    }

    if (AppendAckFrequencyFrame(m_ctx ? m_ctx->config() : nullptr,
                                m_payloadScratch) != UTP_ERR_OK) {
        return -1;
    }

    const Address::Family family = m_peerAddress.isValid() ? m_peerAddress.family() : Address::IPv4;
    const uint16_t targetPacketSize = ConfiguredMinPacketSize(config(), family);
    if (targetPacketSize > UTP_HEADER_SIZE) {
        const size_t targetPayloadSize = static_cast<size_t>(targetPacketSize - UTP_HEADER_SIZE);
        if (AppendPaddingToTargetPayloadSize(targetPayloadSize, m_payloadScratch) != UTP_ERR_OK) {
            return -1;
        }
    }

    return sendPacket(UTP_TYPE_INITIAL,
                      m_payloadScratch.data(),
                      m_payloadScratch.size(),
                      PacketOutFlags::kPoHello);
}

int32_t ConnectionImpl::sendHandshakePacket(bool encrypted)
{
    m_payloadScratch.clear();
    m_payloadScratch.reserve(FRAME_VERSION_SIZE
                           + FRAME_TRANSPORT_PARAMS_SIZE
                           + FRAME_CRYPTO_SIZE
                           + FRAME_ACK_FREQUENCY_SIZE);

    FrameVersion version;
    version.version = UTP_PROTOCOL_VERSION;
    if (AppendEncodedFrame(m_payloadScratch, version, FRAME_VERSION_SIZE) != UTP_ERR_OK) {
        return -1;
    }

    if (AppendTransportParamsFrame(m_loaclTP, m_payloadScratch) != UTP_ERR_OK) {
        return -1;
    }

    if (encrypted) {
        if (!m_x25519) {
            m_x25519 = std::make_shared<X25519Wrapper>();
        }

        FrameCrypto crypto;
        crypto.crypto_type = EncryptionModeToFrameCryptoType(m_connectInfo.encrypted);
        crypto.eph_pubkey = const_cast<uint8_t *>(m_x25519->publicKey().data());

        if (AppendEncodedFrame(m_payloadScratch, crypto, FRAME_CRYPTO_SIZE) != UTP_ERR_OK) {
            return -1;
        }
    }

    if (AppendAckFrequencyFrame(m_ctx ? m_ctx->config() : nullptr,
                                m_payloadScratch) != UTP_ERR_OK) {
        return -1;
    }

    const Address::Family family = m_peerAddress.isValid() ? m_peerAddress.family() : Address::IPv4;
    const uint16_t targetPacketSize = ConfiguredMinPacketSize(config(), family);
    if (targetPacketSize > UTP_HEADER_SIZE) {
        const size_t targetPayloadSize = static_cast<size_t>(targetPacketSize - UTP_HEADER_SIZE);
        if (AppendPaddingToTargetPayloadSize(targetPayloadSize, m_payloadScratch) != UTP_ERR_OK) {
            return -1;
        }
    }

    return sendPacket(UTP_TYPE_HANDSHAKE,
                      m_payloadScratch.data(),
                      m_payloadScratch.size(),
                      PacketOutFlags::kPoHello);
}

int32_t ConnectionImpl::sendPacket(uint8_t packetType,
                                   const void *payload,
                                   size_t payloadLen,
                                   uint16_t packetFlags,
                                   utp_packno_t *outPacketNo,
                                   uint32_t frameTypeBitsOverride,
                                   const Address *targetAddress,
                                   size_t streamDataBytes)
{
    return sendPacket(packetType,
                      payload,
                      payloadLen,
                      nullptr,
                      0,
                      packetFlags,
                      outPacketNo,
                      frameTypeBitsOverride,
                      targetAddress,
                      streamDataBytes);
}

int32_t ConnectionImpl::sendPacket(uint8_t packetType,
                                   const void *payloadHead,
                                   size_t payloadHeadLen,
                                   const void *payloadBody,
                                   size_t payloadBodyLen,
                                   uint16_t packetFlags,
                                   utp_packno_t *outPacketNo,
                                   uint32_t frameTypeBitsOverride,
                                   const Address *targetAddress,
                                   size_t streamDataBytes)
{
    if (m_udpSocket == nullptr) {
        SetLastErrorV(UTP_ERR_SOCKET_WRITE, "udp socket is null");
        return -1;
    }

    const Address &sendAddress = (targetAddress != nullptr) ? *targetAddress : m_peerAddress;
    if (!sendAddress.isValid()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "peer address is invalid");
        return -1;
    }

    const size_t payloadLen = payloadHeadLen + payloadBodyLen;
    if (payloadLen > UINT16_MAX) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "payload length {} exceeds uint16 max", payloadLen);
        return -1;
    }

    uint32_t frameTypeBits = frameTypeBitsOverride;
    if (frameTypeBits == 0) {
        FrameType frameType = kFrameInvalid;
        if (payloadHead != nullptr && payloadHeadLen > 0) {
            frameType = static_cast<FrameType>(*(static_cast<const uint8_t *>(payloadHead)));
        } else if (payloadBody != nullptr && payloadBodyLen > 0) {
            frameType = static_cast<FrameType>(*(static_cast<const uint8_t *>(payloadBody)));
        }

        if (frameType < kFrameMax) {
            frameTypeBits = (1u << static_cast<uint32_t>(frameType));
        }
    }

    const FrameType effectiveFrameType = frameTypeBits != 0
                                       ? FirstFrameTypeBit(frameTypeBits)
                                       : kFrameInvalid;
    const bool isClosePacket = packetType == UTP_TYPE_CONNECTION_CLOSE
                            || (frameTypeBits & (1u << static_cast<uint32_t>(kFrameConnectionClose))) != 0;
    if (m_networkPath.state() == NetworkPath::kPathFailed && !isClosePacket) {
        SetLastErrorV(UTP_ERR_INVALID_STATE, "network path validation failed");
        return -1;
    }

    const bool shouldEncrypt = (m_aesCtx != nullptr)
                            && ((packetFlags & PacketOutFlags::kPoNoEncrypt) == 0)
                            && packetType != UTP_TYPE_INITIAL
                            && packetType != UTP_TYPE_HANDSHAKE
                            && effectiveFrameType != kFrameCrypto
                            && effectiveFrameType != kFrameVersion;
    const size_t encryptOverhead = shouldEncrypt ? static_cast<size_t>(AesGcmContext::GCM_TAG_SIZE) : 0;

    size_t packetLen = UTP_HEADER_SIZE + payloadLen;
    const size_t wirePacketLen = packetLen + encryptOverhead;
    if (wirePacketLen > UINT16_MAX) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "packet length {} exceeds uint16 max", wirePacketLen);
        return -1;
    }

    const bool isMtuProbePacket = (packetFlags & PacketOutFlags::kPoMtuProbe) != 0;
    const size_t currentMaxPacketSize = m_mtuDiscovery.currentMaxPacketSize();
    if (!isMtuProbePacket && wirePacketLen > currentMaxPacketSize) {
        SetLastErrorV(UTP_ERR_WOULD_BLOCK,
                      "packet length {} exceeds current path MTU budget {}",
                      wirePacketLen,
                      currentMaxPacketSize);
        return -1;
    }

    if (isMtuProbePacket && wirePacketLen > m_mtuDiscovery.absoluteMaxPacketSize()) {
        SetLastErrorV(UTP_ERR_WOULD_BLOCK, "mtu probe packet length {} exceeds probe ceiling {}",
                      wirePacketLen, m_mtuDiscovery.absoluteMaxPacketSize());
        return -1;
    }

    if (!canSendOnCurrentPath(wirePacketLen, effectiveFrameType)) {
        SetLastErrorV(UTP_ERR_WOULD_BLOCK,
                      "anti-amplification limit reached while path validating, frame={}",
                      FrameTypeToString(effectiveFrameType));
        return -1;
    }

    if (!canSendStreamUnackedBytes(streamDataBytes)) {
        SetLastErrorV(UTP_ERR_WOULD_BLOCK,
                      "stream unacked data limit reached: pending={}, this={}, limit={}",
                      m_streamUnackedDataBytes,
                      streamDataBytes,
                      config() ? config()->stream_unacked_data_limit : (256u * 1024u));
        return -1;
    }

    PacketOut *packet = m_mm.getPacketOut(static_cast<uint32_t>(packetLen));
    if (packet == nullptr || packet->raw_data == nullptr) {
        if (packet != nullptr) {
            m_mm.putPacketOut(packet);
        }
        SetLastErrorV(UTP_ERR_NO_MEMORY, "allocate packet out failed, size={}", packetLen);
        return -1;
    }

    packet->packno = packetNumber();
    if (outPacketNo != nullptr) {
        *outPacketNo = packet->packno;
    }
    packet->sent_time = time::MonotonicUs();
    packet->data_size = static_cast<uint16_t>(packetLen);
    packet->po_flags |= packetFlags;
    packet->slice_count = 0;
    packet->frame_types = frameTypeBits;
    packet->stream_data_size = static_cast<uint32_t>(std::min<size_t>(streamDataBytes, UINT32_MAX));

    uint8_t *offset = packet->raw_data;
    size_t left = packet->alloc_size;
    offset = Serialize::SerializeTo(offset, left, m_localConnectionID);
    if (offset == nullptr) {
        m_mm.putPacketOut(packet);
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, m_peerConnectionID);
    if (offset == nullptr) {
        m_mm.putPacketOut(packet);
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, packet->packno);
    if (offset == nullptr) {
        m_mm.putPacketOut(packet);
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, static_cast<uint16_t>(payloadLen));
    if (offset == nullptr) {
        m_mm.putPacketOut(packet);
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, packetType);
    if (offset == nullptr) {
        m_mm.putPacketOut(packet);
        return -1;
    }
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(0));
    if (offset == nullptr) {
        m_mm.putPacketOut(packet);
        return -1;
    }

    packet->slices[packet->slice_count++] = PacketOutSlice{0, static_cast<uint16_t>(UTP_HEADER_SIZE)};

    uint16_t payloadOffset = static_cast<uint16_t>(UTP_HEADER_SIZE);

    if (payloadHeadLen > 0 && payloadHead != nullptr) {
        std::memcpy(offset, payloadHead, payloadHeadLen);
        if (packet->slice_count < PACKET_OUT_MAX_SLICES) {
            packet->slices[packet->slice_count++] = PacketOutSlice{payloadOffset, static_cast<uint16_t>(payloadHeadLen)};
        }
        offset += payloadHeadLen;
        payloadOffset = static_cast<uint16_t>(payloadOffset + payloadHeadLen);
    }
    if (payloadBodyLen > 0 && payloadBody != nullptr) {
        std::memcpy(offset, payloadBody, payloadBodyLen);
        if (packet->slice_count < PACKET_OUT_MAX_SLICES) {
            packet->slices[packet->slice_count++] = PacketOutSlice{payloadOffset, static_cast<uint16_t>(payloadBodyLen)};
        }
    }

    if (shouldEncrypt) {
        if (m_aesCtx->encrypt(packet) != UTP_ERR_OK) {
            m_mm.putPacketOut(packet);
            return -1;
        }
        packet->data_size = packet->encrypt_data_size;
    }

    UdpSocket::MsgMetaInfo msg;
    std::memset(&msg, 0, sizeof(msg));
    if (shouldEncrypt && packet->encrypt_data != nullptr) {
        msg.data = packet->encrypt_data;
        msg.len = packet->data_size;
    } else {
        msg.data = packet->raw_data;
        msg.len = packet->data_size;
        msg.slice_count = packet->slice_count;
        for (uint8_t i = 0; i < packet->slice_count && i < UdpSocket::kMaxMsgSlices; ++i) {
            msg.slices[i].data = packet->raw_data + packet->slices[i].offset;
            msg.slices[i].len = packet->slices[i].length;
        }
    }
    msg.metaInfo.peerAddress = sendAddress;

    m_sendMsgScratch.resize(1);
    m_sendMsgScratch[0] = msg;
    int32_t sent = m_udpSocket->send(m_sendMsgScratch);
    if (sent <= 0) {
        m_mm.putPacketOut(packet);
        return -1;
    }

    m_bytesOut += packet->data_size;

    const bool isAckOnlyPacket = frameTypeBits == (1u << static_cast<uint32_t>(kFrameAck));
    const bool allowSendCtlRetrans = (m_state == State::kStateConnected)
                                  || (m_state == State::kStateCloseSent)
                                  || (m_state == State::kStateCloseReceived)
                                  || isClosePacket;
    const bool shouldTrackPacket = !isAckOnlyPacket && allowSendCtlRetrans;
    if (m_sendCtl && shouldTrackPacket) {
        if (m_sendCtl->packetSent(packet) != UTP_ERR_OK) {
            m_mm.putPacketOut(packet);
            return -1;
        }
    } else {
        m_mm.putPacketOut(packet);
    }

    return UTP_ERR_OK;
}

bool ConnectionImpl::canSendOnCurrentPath(size_t packetLen, FrameType frameType) const
{
    // TODO (path-migration): 预留激进策略入口。
    // 当前版本即使 Config.path_migration_mode = kPathMigrationAggressive，
    // 也强制按保守策略执行：未验证路径仅允许受控发送，不提前切业务数据到新路径。
    // 后续在双路径状态机完成后再启用激进行为。
    if (!m_networkPath.needPathValidation()) {
        return true;
    }

    switch (frameType) {
    case kFramePathChallenge:
    case kFramePathResponse:
    case kFramePing:
    case kFrameAck:
    case kFrameHandshakeDone:
    case kFrameConnectionClose:
        return true;
    default:
        break;
    }

    uint64_t outBytesNext = m_bytesOut + packetLen;
    uint64_t limit = m_bytesIn * 3 + kPathValidationSendCredit;
    return outBytesNext <= limit;
}

void ConnectionImpl::maybeSendPathChallenge()
{
    if (!m_networkPath.needPathValidation()) {
        return;
    }

    FramePathChallenge challenge;
    int32_t status = m_networkPath.makePathChallenge(challenge, time::MonotonicMs());
    if (status != UTP_ERR_OK) {
        return;
    }

    uint8_t payload[FRAME_PATH_FRAME_SIZE] = {0};
    int32_t frameLen = challenge.encode(payload, sizeof(payload));
    if (frameLen < 0) {
        return;
    }

    const Address candidateAddress = m_networkPath.peerAddress();
    if (!candidateAddress.isValid()) {
        return;
    }

    if (sendPacket(UTP_TYPE_CTRL,
                   payload,
                   static_cast<size_t>(frameLen),
                   0,
                   nullptr,
                   0,
                   &candidateAddress) != UTP_ERR_OK) {
        return;
    }

    utp_time_t nowMs = time::MonotonicMs();
    utp_time_t deadlineMs = m_networkPath.challengeDeadlineMs();
    utp_time_t timeoutMs = deadlineMs > nowMs ? (deadlineMs - nowMs) : 1;
    m_pathValidationTimer.start(timeoutMs);
}

void ConnectionImpl::handlePathChallengeFrame(const uint8_t *frameData,
                                              size_t frameSize,
                                              const Address &fromAddress)
{
    FramePathChallenge challenge;
    if (challenge.decode(frameData, frameSize) < 0) {
        return;
    }

    FramePathResponse response;
    m_networkPath.makePathResponse(challenge, response);
    uint8_t payload[FRAME_PATH_FRAME_SIZE] = {0};
    int32_t frameLen = response.encode(payload, sizeof(payload));
    if (frameLen < 0) {
        return;
    }

    sendPacket(UTP_TYPE_CTRL,
               payload,
               static_cast<size_t>(frameLen),
               0,
               nullptr,
               0,
               &fromAddress);
}

void ConnectionImpl::handlePathResponseFrame(const uint8_t *frameData,
                                             size_t frameSize,
                                             const Address &fromAddress)
{
    if (!m_networkPath.needPathValidation() || fromAddress != m_networkPath.peerAddress()) {
        return;
    }

    FramePathResponse response;
    if (response.decode(frameData, frameSize) < 0) {
        return;
    }

    if (m_networkPath.onPathResponse(response)) {
        // 校验成功后才切换 active 路径到 candidate。
        m_peerAddress = m_networkPath.peerAddress();
        m_mtuDiscovery.onPathValidated(time::MonotonicMs());
        m_pathValidationTimer.stop();
        if (m_ctx != nullptr) {
            m_ctx->notePathValidationSucceeded();
        }
    }
}

void ConnectionImpl::onPathValidationTimeout()
{
    if (m_networkPath.onTimeout(time::MonotonicMs())) {
        if (m_networkPath.state() == NetworkPath::kPathFailed) {
            // 保守策略：candidate 失败不关闭连接，回退/保持 active 路径。
            if (m_ctx != nullptr) {
                m_ctx->notePathValidationFailed();
            }
            m_networkPath.bindPeerAddress(m_peerAddress);
            m_pathValidationTimer.stop();
        }
        return;
    }

    if (m_networkPath.needPathValidation() && !m_networkPath.hasInFlightChallenge()) {
        maybeSendPathChallenge();
    }
}

void ConnectionImpl::onHandshakeDoneTimeout()
{
    if (!m_handshakeDonePending || m_state != State::kStateConnected) {
        return;
    }

    const int32_t status = sendHandshakeDonePacket();
    if (status != UTP_ERR_OK) {
        scheduleWrite();
        m_handshakeDoneTimer.start(10);
    }
}

void ConnectionImpl::onHandshakeDoneFrameAcked()
{
    if (!m_handshakeDonePending) {
        return;
    }

    m_handshakeDonePending = false;
    m_handshakeDoneTimer.stop();
}

void ConnectionImpl::armHandshakeDoneTimer()
{
    const uint32_t delayMs = handshakeDoneDelayMs();
    m_handshakeDoneTimer.stop();
    m_handshakeDoneTimer.start(delayMs);
}

uint32_t ConnectionImpl::handshakeDoneDelayMs() const
{
    uint32_t handshakeTimeoutMs = 0;
    if ((m_peerTP.flags & TransportParams::kHandshakeTimeout) != 0 && m_peerTP.handshake_timeout > 0) {
        handshakeTimeoutMs = m_peerTP.handshake_timeout;
    } else if (m_connectInfo.timeout > 0) {
        handshakeTimeoutMs = m_connectInfo.timeout;
    } else if (m_ctx != nullptr && m_ctx->config() != nullptr && m_ctx->config()->handshake_timeout > 0) {
        handshakeTimeoutMs = m_ctx->config()->handshake_timeout;
    } else {
        handshakeTimeoutMs = 1000;
    }

    const uint32_t delayMs = handshakeTimeoutMs / 3;
    return delayMs > 0 ? delayMs : 1;
}

void ConnectionImpl::onConnTimeout()
{
    if (m_state == State::kStateConnected || m_state == State::kStateDisconnected ||
        m_state == State::kStateCloseSent || m_state == State::kStatePtoTimedWait) {
        return;
    }

    m_lastErrorCode = UTP_ERR_TIMEOUT;
    m_lastErrorReason = "connect timeout";
    notifyConnectionError(m_lastErrorCode, m_lastErrorReason, true);
    m_state = State::kStateDisconnected;
    m_handshakeDoneTimer.stop();
    stopAckTimer();
    m_keepaliveTimer.stop();
    notifyConnectionClosed(m_lastErrorCode, m_lastErrorReason, false);
    scheduleWrite();
}

uint32_t ConnectionImpl::keepaliveIntervalMs() const
{
    if (m_ctx == nullptr || m_ctx->config() == nullptr) {
        return 1000;
    }

    const Config *cfg = m_ctx->config();
    const uint32_t localInterval = cfg->keepalive_interval > 0
                                 ? cfg->keepalive_interval
                                 : std::max<uint32_t>(cfg->max_idle_timeout, 1);

    const uint32_t peerIdleTimeout = std::max<uint32_t>(m_peerTP.max_idle_timeout, 1);
    const uint32_t srttMs = static_cast<uint32_t>(m_rttStats.srtt() / 1000);
    const uint32_t guardMs = std::max<uint32_t>(3 * srttMs, 50);
    const uint32_t peerSafeInterval = peerIdleTimeout > guardMs + 1
                                    ? (peerIdleTimeout - guardMs)
                                    : 1;

    return std::max<uint32_t>(std::min(localInterval, peerSafeInterval), 1);
}

void ConnectionImpl::armKeepaliveTimer(uint32_t delayMs)
{
    if (m_state != State::kStateConnected || m_ctx == nullptr || m_ctx->config() == nullptr) {
        return;
    }
    if (!m_ctx->config()->enable_keepalive) {
        return;
    }

    m_keepaliveTimer.stop();
    m_keepaliveTimer.start(delayMs > 0 ? delayMs : 1);
}

void ConnectionImpl::markPeerActivity(utp_time_t nowUs)
{
    m_lastActivityUs = nowUs;
    m_keepaliveMissedProbes = 0;
    armKeepaliveTimer(keepaliveIntervalMs());
}

void ConnectionImpl::beginCloseSent(uint16_t errorCode, const std::string &reason)
{
    if (m_state == State::kStateDisconnected || m_state == State::kStatePtoTimedWait) {
        return;
    }

    m_state = State::kStateCloseSent;
    m_connTimer.stop();
    m_handshakeDoneTimer.stop();
    m_pathValidationTimer.stop();
    stopAckTimer();
    m_keepaliveTimer.stop();

    m_closeErrorCode = errorCode;
    m_closeReason = reason.empty() ? "local close" : reason;
    m_closeByPeer = false;
    m_closeFramePending = true;
    m_closePeerResendCount = 0;
    m_closePtoUs = closePtoUs();
    m_closeDeadlineUs = 0;
    m_closeLastSendUs = 0;

    scheduleWrite();
}

void ConnectionImpl::onKeepaliveTimeout()
{
    if (m_state != State::kStateConnected || m_ctx == nullptr || m_ctx->config() == nullptr) {
        return;
    }

    const Config *cfg = m_ctx->config();
    if (!cfg->enable_keepalive) {
        return;
    }

    const utp_time_t nowUs = time::MonotonicUs();
    if (m_lastActivityUs == 0) {
        m_lastActivityUs = nowUs;
    }

    const uint32_t intervalMs = keepaliveIntervalMs();
    const utp_time_t intervalUs = static_cast<utp_time_t>(intervalMs) * 1000;
    if (nowUs < m_lastActivityUs + intervalUs) {
        const utp_time_t remainUs = m_lastActivityUs + intervalUs - nowUs;
        armKeepaliveTimer(static_cast<uint32_t>(std::max<utp_time_t>(remainUs / 1000, 1)));
        return;
    }

    const uint16_t maxProbes = std::max<uint16_t>(cfg->keepalive_probes, 1);
    if (m_keepaliveMissedProbes >= maxProbes) {
        m_lastErrorCode = UTP_ERR_TIMEOUT;
        m_lastErrorReason = "keepalive timeout";
        notifyConnectionError(m_lastErrorCode, m_lastErrorReason, true);
        beginCloseSent(static_cast<uint16_t>(m_lastErrorCode), m_lastErrorReason);
        return;
    }

    const uint8_t payload[1] = {static_cast<uint8_t>(kFramePing)};
    const int32_t status = sendPacket(UTP_TYPE_CTRL, payload, sizeof(payload));
    if (status == UTP_ERR_OK) {
        ++m_keepaliveMissedProbes;
        const uint32_t timeoutMs = cfg->keepalive_timeout > 0 ? cfg->keepalive_timeout : intervalMs;
        armKeepaliveTimer(timeoutMs);
        return;
    }

    armKeepaliveTimer(10);
}

utp_time_t ConnectionImpl::closePtoUs() const
{
    utp_time_t srtt = m_rttStats.srtt();
    utp_time_t rttVar = m_rttStats.rttVar();
    if (srtt == 0) {
        srtt = kDefaultPtoUs;
    }

    utp_time_t granularityUs = 1000;
    if (m_ctx != nullptr && m_ctx->config() != nullptr) {
        granularityUs = std::max<utp_time_t>(1000, m_ctx->config()->clock_granularity_us);
    }

    utp_time_t maxAckDelayUs = static_cast<utp_time_t>(m_peerAckMaxDelayMs) * 1000;
    utp_time_t ptoUs = srtt + std::max<utp_time_t>(4 * rttVar, granularityUs) + maxAckDelayUs;
    ptoUs = std::max<utp_time_t>(ptoUs, kMinPtoUs);
    ptoUs = std::min<utp_time_t>(ptoUs, kMaxPtoUs);

    return ptoUs;
}

void ConnectionImpl::enterPtoTimedWait()
{
    const utp_time_t nowUs = time::MonotonicUs();
    m_connTimer.stop();
    m_handshakeDoneTimer.stop();
    m_pathValidationTimer.stop();
    stopAckTimer();
    m_keepaliveTimer.stop();
    if (m_state != State::kStateCloseReceived) {
        m_state = State::kStatePtoTimedWait;
    }

    if (m_closePtoUs == 0) {
        m_closePtoUs = closePtoUs();
    }
    if (m_closeDeadlineUs == 0 || m_closeDeadlineUs < nowUs) {
        m_closeDeadlineUs = nowUs + m_closePtoUs * 3;
    }

    m_closeDrainTimer.stop();
    utp_time_t remainUs = (m_closeDeadlineUs > nowUs) ? (m_closeDeadlineUs - nowUs) : 0;
    uint32_t waitMs = static_cast<uint32_t>(remainUs / 1000);
    m_closeDrainTimer.start(waitMs > 0 ? waitMs : 1);
}

void ConnectionImpl::onCloseDrainTimeout()
{
    if (m_state != State::kStateCloseSent && m_state != State::kStatePtoTimedWait && m_state != State::kStateCloseReceived) {
        return;
    }

    m_state = State::kStateDisconnected;
    stopAckTimer();
    m_keepaliveTimer.stop();
    const int32_t closeCode = m_closeErrorCode == 0 ? UTP_ERR_CANCELLED : static_cast<int32_t>(m_closeErrorCode);
    notifyConnectionClosed(closeCode, m_closeReason, m_closeByPeer);
    scheduleWrite();
}

void ConnectionImpl::setOnIncomingStream(const OnIncomingStream &cb)
{
    m_onIncomingStream = cb;
}

void ConnectionImpl::setOnSessionTokenReady(const OnSessionTokenReady &cb)
{
    m_onSessionTokenReady = cb;
}

void ConnectionImpl::setOnError(const OnError &cb)
{
    m_onError = cb;
}

void ConnectionImpl::setOnClosed(const OnClosed &cb)
{
    m_onClosed = cb;
}

void ConnectionImpl::notifyConnectionError(int32_t errorCode, const std::string &reason, bool fatal)
{
    if (errorCode == UTP_ERR_OK) {
        return;
    }

    if (m_onError) {
        ConnectionErrorInfo info;
        info.error_code = errorCode;
        info.error_reason = reason;
        info.fatal = fatal;
        m_onError(info);
    }
}

void ConnectionImpl::notifyConnectionClosed(int32_t errorCode, const std::string &reason, bool byPeer)
{
    if (m_closedNotified) {
        return;
    }

    m_closedNotified = true;
    if (!m_onClosed) {
        return;
    }

    ConnectionCloseInfo info;
    info.error_code = errorCode;
    info.error_reason = reason;
    info.by_peer = byPeer;
    m_onClosed(info);
}

int32_t ConnectionImpl::streamCount(StreamType streamType) const
{
    return static_cast<int32_t>(activeStreamCount(streamType));
}

int32_t ConnectionImpl::creatableStreamCount(StreamType streamType) const
{
    if (!IsSupportedStreamType(streamType)) {
        return 0;
    }

    const uint32_t maxStreams = streamLimit(streamType, true);
    const uint32_t usedStreams = static_cast<uint32_t>(activeLocalStreamCount(streamType));
    if (usedStreams >= maxStreams) {
        return 0;
    }
    return static_cast<int32_t>(maxStreams - usedStreams);
}

Connection::Statistic ConnectionImpl::statistic() const
{
    Connection::Statistic stat;
    std::memset(&stat, 0, sizeof(stat));
    stat.pmtu = m_mtuDiscovery.pathMtu();
    stat.rtt = static_cast<uint32_t>(std::min<uint64_t>(m_obsRttUs, UINT32_MAX));
    stat.rttvar = static_cast<uint32_t>(std::min<uint64_t>(m_obsRttVarUs, UINT32_MAX));
    const uint64_t bwEstimate = (m_sendCtl != nullptr) ? m_sendCtl->bandwidthEstimate() : 0;
    stat.bw_estimate = static_cast<uint32_t>(std::min<uint64_t>(bwEstimate, UINT32_MAX));
    stat.rx_bytes = m_bytesIn;
    stat.tx_bytes = m_bytesOut;
    stat.rtx_bytes = m_bytesRetrans;
    stat.scheduler_select_total = m_schedulerStats.selectTotal;
    stat.scheduler_select_disabled = m_schedulerStats.selectDisabled;
    stat.scheduler_select_strict = m_schedulerStats.selectStrict;
    stat.scheduler_select_drr = m_schedulerStats.selectDrr;
    stat.scheduler_strict_aging_promoted = m_schedulerStats.strictAgingPromoted;
    stat.scheduler_would_block = m_schedulerStats.wouldBlock;
    stat.scheduler_empty_rounds = m_schedulerStats.emptyRounds;
    stat.scheduler_mode_switches = m_schedulerStats.modeSwitches;
    stat.scheduler_drr_refills = m_schedulerStats.drrDeficitRefills;
    stat.scheduler_drr_consumes = m_schedulerStats.drrDeficitConsumes;
    return stat;
}

Connection::Description ConnectionImpl::description() const
{
    Connection::Description desc;
    desc.scid = m_localConnectionID;
    desc.dcid = m_peerConnectionID;
    desc.remoteHost = m_connectInfo.ip;
    desc.remotePort = m_connectInfo.port;
    return desc;
}

int32_t ConnectionImpl::exportSessionToken(std::vector<uint8_t> &outToken)
{
    if (!m_hasCachedResumptionState || m_cachedResumptionInfo.sessionTicket.empty()) {
        SetLastErrorV(UTP_ERR_INVALID_STATE, "connection {} has no cached session token", m_localConnectionID);
        return -1;
    }

    outToken = m_cachedResumptionInfo.sessionTicket;
    return UTP_ERR_OK;
}

int32_t ConnectionImpl::exportSessionResumptionState(std::string &outState)
{
    if (!m_hasCachedResumptionState) {
        SetLastErrorV(UTP_ERR_INVALID_STATE, "connection {} has no cached resumption state", m_localConnectionID);
        return -1;
    }

    return buildSessionResumptionState(m_cachedResumptionInfo, m_cachedResumptionExpiresAt, outState);
}

void ConnectionImpl::cacheSessionResumptionState(const CachedResumptionState &info, uint64_t expiresAt)
{
    if (info.sessionTicket.empty()) {
        return;
    }
    if (info.encrypted != Context::kEncryptionNone && info.resumptionPsk.size() != ResumptionStateCodec::KEY_SIZE) {
        return;
    }

    m_cachedResumptionInfo = info;
    m_cachedResumptionExpiresAt = expiresAt;
    m_hasCachedResumptionState = true;
    if (m_onSessionTokenReady) {
        m_onSessionTokenReady();
    }
}

int32_t ConnectionImpl::buildSessionResumptionState(const CachedResumptionState &info,
                                                    uint64_t expiresAt,
                                                    std::string &outState) const
{
    if (m_ctx == nullptr) {
        SetLastErrorV(UTP_ERR_INVALID_STATE, "connection {} has no context", m_localConnectionID);
        return -1;
    }
    if (info.sessionTicket.empty()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "connection {} cannot build state without session ticket", m_localConnectionID);
        return -1;
    }
    if (info.encrypted != Context::kEncryptionNone && info.resumptionPsk.size() != ResumptionStateCodec::KEY_SIZE) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "connection {} encrypted state requires {}-byte psk, got {}",
                      m_localConnectionID,
                      ResumptionStateCodec::KEY_SIZE,
                      info.resumptionPsk.size());
        return -1;
    }

    const uint16_t ticketSize = static_cast<uint16_t>(std::min<size_t>(info.sessionTicket.size(), UINT16_MAX));
    const uint8_t pskSize = static_cast<uint8_t>(std::min<size_t>(info.resumptionPsk.size(), UINT8_MAX));
    std::vector<uint8_t> plain(4 + 1 + 8 + 2 + ticketSize + 1 + pskSize, 0);

    uint8_t *offset = plain.data();
    size_t left = plain.size();
    const uint32_t utpVersion = UTP_PROTOCOL_VERSION;
    offset = Serialize::SerializeTo(offset, left, utpVersion);
    offset = Serialize::SerializeTo(offset, left, static_cast<uint8_t>(info.encrypted));
    offset = Serialize::SerializeTo(offset, left, expiresAt);
    offset = Serialize::SerializeTo(offset, left, ticketSize);
    if (offset == nullptr || left < ticketSize) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "connection {} build state overflow on session ticket", m_localConnectionID);
        return -1;
    }
    std::memcpy(offset, info.sessionTicket.data(), ticketSize);
    offset += ticketSize;
    left -= ticketSize;

    offset = Serialize::SerializeTo(offset, left, pskSize);
    if (offset == nullptr || left < pskSize) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "connection {} build state overflow on resumption psk", m_localConnectionID);
        return -1;
    }
    if (pskSize > 0) {
        std::memcpy(offset, info.resumptionPsk.data(), pskSize);
    }

    std::vector<uint8_t> sealed;
    const auto key = m_ctx->resumptionSecret();
    if (!ResumptionStateCodec::Seal(key, plain, sealed)) {
        SetLastErrorV(UTP_ERR_CRYPTO_ENCRYPTION, "connection {} failed to seal resumption state", m_localConnectionID);
        return -1;
    }
    if (!Base64::EncodeStd(sealed, outState)) {
        SetLastErrorV(UTP_ERR_CRYPTO_ENCRYPTION, "connection {} failed to encode resumption state", m_localConnectionID);
        return -1;
    }
    return UTP_ERR_OK;
}

int32_t ConnectionImpl::createStream(StreamType streamType)
{
    if (!IsSupportedStreamType(streamType)) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "connection {} unsupported stream type {}",
                      m_localConnectionID,
                      static_cast<uint32_t>(streamType));
        return -1;
    }

    const bool allowEarlyData = (m_state == State::kStateInitialSent)
                             && m_zeroRttConfig.enabled();
    if (m_state != State::kStateConnected && !allowEarlyData) {
        SetLastErrorV(UTP_ERR_INVALID_STATE,
                      "connection {} cannot create stream in state {}",
                      m_localConnectionID,
                      static_cast<uint32_t>(m_state));
        return -1;
    }

    collectClosedStreams();

    const uint32_t maxStreams = streamLimit(streamType, true);
    if (activeLocalStreamCount(streamType) >= maxStreams) {
        SetLastErrorV(UTP_ERR_STREAM_LIMIT_ERROR,
                      "connection {} stream limit reached for type {}",
                      m_localConnectionID,
                      static_cast<uint32_t>(streamType));
        return -1;
    }

    const uint32_t slot = streamIdSlot(streamType);
    if (slot >= STREAM_TYPES) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM,
                      "connection {} invalid stream slot {} for type {}",
                      m_localConnectionID,
                      slot,
                      static_cast<uint32_t>(streamType));
        return -1;
    }

    uint32_t &nextStreamId = m_streamId[slot];
    if (nextStreamId == 0) {
        const uint32_t roleBase = m_isClientInitiator ? 0u : 1u;
        const uint32_t dirBit = streamType == kStreamTypeUnidirectional ? 2u : 0u;
        nextStreamId = roleBase + dirBit;
    }

    const uint32_t streamId = nextStreamId;
    nextStreamId += STREAM_TYPES;

    if (m_streams.find(streamId) != m_streams.end()) {
        SetLastErrorV(UTP_ERR_STREAM_ID_EXHAUSTED,
                      "connection {} stream id exhausted: {}",
                      m_localConnectionID,
                      streamId);
        return -1;
    }

    StreamImpl::SP stream = std::make_shared<StreamImpl>(this,
                                                          streamId,
                                                          defaultStreamPriority());
    m_streams.emplace(streamId, stream);

    return static_cast<int32_t>(streamId);
}

uint8_t ConnectionImpl::defaultStreamPriority() const
{
    if (m_ctx == nullptr || m_ctx->config() == nullptr) {
        return Stream::kPriorityDefault;
    }

    return std::min<uint8_t>(m_ctx->config()->stream_default_priority, Stream::kPriorityLowest);
}

StreamSchedulerMode ConnectionImpl::streamSchedulerMode() const
{
    if (m_ctx == nullptr || m_ctx->config() == nullptr) {
        return kStreamSchedulerStrict;
    }

    return m_ctx->config()->stream_scheduler_mode;
}

StreamImpl::SP ConnectionImpl::pickRoundRobinStream(const std::vector<StreamImpl::SP> &candidates,
                                                    uint32_t &cursor)
{
    if (candidates.empty()) {
        return nullptr;
    }

    std::vector<uint32_t> ids;
    ids.reserve(candidates.size());
    for (const auto &stream : candidates) {
        if (stream) {
            ids.push_back(stream->id());
        }
    }
    if (ids.empty()) {
        return nullptr;
    }

    std::sort(ids.begin(), ids.end());
    uint32_t selectedId = ids.front();
    for (uint32_t id : ids) {
        if (id > cursor) {
            selectedId = id;
            break;
        }
    }
    cursor = selectedId;

    auto it = m_streams.find(selectedId);
    if (it == m_streams.end()) {
        return nullptr;
    }
    return it->second;
}

StreamImpl::SP ConnectionImpl::pickNextWritableStreamDisabled()
{
    const utp_time_t nowUs = time::MonotonicUs();
    std::vector<StreamImpl::SP> candidates;
    candidates.reserve(m_streams.size());
    for (const auto &entry : m_streams) {
        if (entry.second && entry.second->hasPendingSendWork() && !entry.second->shouldDeferSend(nowUs)) {
            candidates.push_back(entry.second);
        }
    }

    StreamImpl::SP selected = pickRoundRobinStream(candidates, m_disabledRrCursor);
    if (selected) {
        onSchedulerStreamSelected(selected,
                                  kStreamSchedulerDisabled,
                                  false,
                                  selected->m_priority,
                                  0,
                                  0,
                                  0);
        UTP_LOGD("connection %u scheduler selected: mode=%s stream=%u prio=%u",
                 m_localConnectionID,
                 StreamSchedulerModeToString(kStreamSchedulerDisabled),
                 selected->id(),
                 static_cast<uint32_t>(selected->m_priority));
    }

    return selected;
}

StreamImpl::SP ConnectionImpl::pickNextWritableStreamStrict()
{
    const utp_time_t nowUs = time::MonotonicUs();
    std::array<std::vector<StreamImpl::SP>, kMaxStreamPriorityLevels> byPriority;
    for (const auto &entry : m_streams) {
        if (!entry.second || !entry.second->hasPendingSendWork() || entry.second->shouldDeferSend(nowUs)) {
            continue;
        }

        const uint8_t basePriority = std::min<uint8_t>(entry.second->m_priority, Stream::kPriorityLowest);
        const Config *cfg = (m_ctx != nullptr) ? m_ctx->config() : nullptr;
        const uint16_t agingThreshold = (cfg != nullptr && cfg->stream_aging_threshold > 0)
                                     ? cfg->stream_aging_threshold
                                     : 1;
        const uint8_t agingStep = (cfg != nullptr && cfg->stream_aging_step > 0)
                               ? cfg->stream_aging_step
                               : 1;
        const uint32_t promotions = (entry.second->m_schedWaitRounds / agingThreshold) * agingStep;
        const uint8_t effectivePriority = (promotions >= basePriority)
                                       ? Stream::kPriorityHighest
                                       : static_cast<uint8_t>(basePriority - promotions);
        byPriority[effectivePriority].push_back(entry.second);
    }

    for (size_t prio = 0; prio < kMaxStreamPriorityLevels; ++prio) {
        StreamImpl::SP selected = pickRoundRobinStream(byPriority[prio], m_strictRrCursor[prio]);
        if (selected) {
            const bool agingPromoted = selected->m_schedWaitRounds > 0 && prio < selected->m_priority;
            onSchedulerStreamSelected(selected,
                                      kStreamSchedulerStrict,
                                      agingPromoted,
                                      static_cast<uint8_t>(prio),
                                      0,
                                      0,
                                      0);
            UTP_LOGD("connection %u scheduler selected: mode=%s stream=%u base_prio=%u effective_prio=%u wait_rounds=%u",
                     m_localConnectionID,
                     StreamSchedulerModeToString(kStreamSchedulerStrict),
                     selected->id(),
                     static_cast<uint32_t>(selected->m_priority),
                     static_cast<uint32_t>(prio),
                     selected->m_schedWaitRounds);
            return selected;
        }
    }

    return nullptr;
}

StreamImpl::SP ConnectionImpl::pickNextWritableStreamDrr()
{
    const utp_time_t nowUs = time::MonotonicUs();
    std::vector<StreamImpl::SP> candidates;
    candidates.reserve(m_streams.size());
    for (const auto &entry : m_streams) {
        if (entry.second && entry.second->hasPendingSendWork() && !entry.second->shouldDeferSend(nowUs)) {
            candidates.push_back(entry.second);
        }
    }

    if (candidates.empty()) {
        return nullptr;
    }

    std::sort(candidates.begin(), candidates.end(), [] (const StreamImpl::SP &a, const StreamImpl::SP &b) {
        return a->id() < b->id();
    });

    const Config *cfg = (m_ctx != nullptr) ? m_ctx->config() : nullptr;
    const uint32_t baseQuantum = (cfg != nullptr && cfg->stream_drr_quantum > 0)
                              ? cfg->stream_drr_quantum
                              : 1200;
    const uint32_t deficitCap = (cfg != nullptr && cfg->stream_drr_deficit_cap > 0)
                             ? cfg->stream_drr_deficit_cap
                             : 64 * 1024;

    const size_t size = candidates.size();
    size_t startIndex = 0;
    for (size_t i = 0; i < size; ++i) {
        if (candidates[i]->id() > m_drrCursor) {
            startIndex = i;
            break;
        }
    }

    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t n = 0; n < size; ++n) {
            const size_t idx = (startIndex + n) % size;
            const StreamImpl::SP &stream = candidates[idx];
            const uint32_t priorityWeight = static_cast<uint32_t>(Stream::kPriorityLowest - std::min<uint8_t>(stream->m_priority, Stream::kPriorityLowest) + 1);
            const uint32_t quantum = baseQuantum * priorityWeight;

            uint32_t &deficit = m_drrDeficit[stream->id()];
            const uint32_t before = deficit;
            if (deficit + quantum >= deficitCap) {
                deficit = deficitCap;
            } else {
                deficit += quantum;
            }
            ++m_schedulerStats.drrDeficitRefills;

            const uint32_t need = stream->m_sendQueuedBytes > 0
                                ? static_cast<uint32_t>(std::min<size_t>(stream->m_sendQueuedBytes, UINT16_MAX))
                                : 1u;
            if (deficit >= need) {
                const uint32_t afterRefill = deficit;
                deficit -= need;
                ++m_schedulerStats.drrDeficitConsumes;
                m_drrCursor = stream->id();
                onSchedulerStreamSelected(stream,
                                          kStreamSchedulerDrr,
                                          false,
                                          stream->m_priority,
                                          need,
                                          before,
                                          deficit);
                UTP_LOGD("connection %u scheduler selected: mode=%s stream=%u prio=%u need=%u deficit_before=%u deficit_after_refill=%u deficit_after=%u",
                         m_localConnectionID,
                         StreamSchedulerModeToString(kStreamSchedulerDrr),
                         stream->id(),
                         static_cast<uint32_t>(stream->m_priority),
                         need,
                         before,
                         afterRefill,
                         deficit);
                return stream;
            }
        }
    }

    return nullptr;
}

StreamImpl::SP ConnectionImpl::pickNextWritableStream()
{
    switch (streamSchedulerMode()) {
    case kStreamSchedulerDisabled:
        return pickNextWritableStreamDisabled();
    case kStreamSchedulerDrr:
        return pickNextWritableStreamDrr();
    case kStreamSchedulerStrict:
    default:
        return pickNextWritableStreamStrict();
    }
}

void ConnectionImpl::updateStrictAgingState(uint32_t selectedStreamId)
{
    if (streamSchedulerMode() != kStreamSchedulerStrict) {
        return;
    }

    for (auto &entry : m_streams) {
        if (!entry.second || !entry.second->hasPendingSendWork()) {
            if (entry.second) {
                entry.second->m_schedWaitRounds = 0;
            }
            continue;
        }

        if (entry.first == selectedStreamId) {
            entry.second->m_schedWaitRounds = 0;
            continue;
        }

        if (entry.second->m_schedWaitRounds < UINT32_MAX) {
            ++entry.second->m_schedWaitRounds;
        }
    }
}

void ConnectionImpl::pruneSchedulerState()
{
    for (auto it = m_drrDeficit.begin(); it != m_drrDeficit.end();) {
        auto streamIt = m_streams.find(it->first);
        if (streamIt == m_streams.end() || !streamIt->second || !streamIt->second->hasPendingSendWork()) {
            it = m_drrDeficit.erase(it);
            continue;
        }
        ++it;
    }
}

void ConnectionImpl::onSchedulerStreamSelected(const StreamImpl::SP &stream,
                                               StreamSchedulerMode mode,
                                               bool agingPromoted,
                                               uint8_t effectivePriority,
                                               uint32_t drrNeed,
                                               uint32_t drrDeficitBefore,
                                               uint32_t drrDeficitAfter)
{
    if (!stream) {
        return;
    }

    ++m_schedulerStats.selectTotal;
    switch (mode) {
    case kStreamSchedulerDisabled:
        ++m_schedulerStats.selectDisabled;
        break;
    case kStreamSchedulerDrr:
        ++m_schedulerStats.selectDrr;
        break;
    case kStreamSchedulerStrict:
    default:
        ++m_schedulerStats.selectStrict;
        break;
    }

    if (agingPromoted) {
        ++m_schedulerStats.strictAgingPromoted;
    }

    (void)effectivePriority;
    (void)drrNeed;
    (void)drrDeficitBefore;
    (void)drrDeficitAfter;
}

void ConnectionImpl::noteSchedulerModeIfChanged(StreamSchedulerMode mode)
{
    if (m_schedulerStats.lastMode == mode) {
        return;
    }

    ++m_schedulerStats.modeSwitches;
    UTP_LOGD("connection %u scheduler mode switch: %s -> %s",
             m_localConnectionID,
             StreamSchedulerModeToString(m_schedulerStats.lastMode),
             StreamSchedulerModeToString(mode));
    m_schedulerStats.lastMode = mode;
}

void ConnectionImpl::maybeEmitSchedulerStats(utp_time_t nowUs)
{
    if (m_schedulerStats.lastReportUs != 0
        && nowUs - m_schedulerStats.lastReportUs < kSchedulerStatsLogIntervalUs) {
        return;
    }

    m_schedulerStats.lastReportUs = nowUs;
    UTP_LOGD("connection %u scheduler stats: total=%u disabled=%u strict=%u drr=%u aging_promoted=%u would_block=%u empty_rounds=%u mode_switches=%u drr_refill=%u drr_consume=%u",
             m_localConnectionID,
             static_cast<uint32_t>(m_schedulerStats.selectTotal),
             static_cast<uint32_t>(m_schedulerStats.selectDisabled),
             static_cast<uint32_t>(m_schedulerStats.selectStrict),
             static_cast<uint32_t>(m_schedulerStats.selectDrr),
             static_cast<uint32_t>(m_schedulerStats.strictAgingPromoted),
             static_cast<uint32_t>(m_schedulerStats.wouldBlock),
             static_cast<uint32_t>(m_schedulerStats.emptyRounds),
             static_cast<uint32_t>(m_schedulerStats.modeSwitches),
             static_cast<uint32_t>(m_schedulerStats.drrDeficitRefills),
             static_cast<uint32_t>(m_schedulerStats.drrDeficitConsumes));
}

Stream* ConnectionImpl::getStream(uint32_t streamId)
{
    collectClosedStreams();

    auto it = m_streams.find(streamId);
    if (it == m_streams.end() || !it->second) {
        return nullptr;
    }
    return it->second.get();
}

void ConnectionImpl::close()
{
    if (m_state == State::kStateDisconnected) {
        return;
    }

    beginCloseSent(UTP_ERR_CANCELLED, "local close");
}

} // namespace utp
} // namespace eular
