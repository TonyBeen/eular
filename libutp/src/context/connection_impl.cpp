/*************************************************************************
    > File Name: connection_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:40:15 PM CST
 ************************************************************************/

#include "context/connection_impl.h"
#include "context/context_impl.h"
#include "context/send_ctl.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

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
#include "proto/frame/ack.h"
#include "proto/frame/stream.h"
#include "proto/frame/crypto.h"
#include "proto/frame/connection_close.h"

#include "crypto/x25519_wrapper.h"
#include "crypto/aes_gcm_context.h"

#include "make_unique.hpp"

namespace {

using eular::Serialize;
using eular::utp::FrameType;

constexpr uint64_t kPathValidationSendCredit = 256;
constexpr utp_time_t kMinPtoUs = 10000;
constexpr utp_time_t kDefaultPtoUs = 333333;
constexpr utp_time_t kMaxPtoUs = 60000000;
constexpr uint8_t kMaxCloseResendCount = 3;

int32_t BuildAckFrequencyFrame(const eular::utp::Config *config,
                               uint8_t *buffer,
                               size_t size,
                               size_t &outSize)
{
    eular::utp::FrameAckFrequency ackFreq;
    const uint16_t ackEveryN = config ? config->ack_every_n_packets : 10;
    ackFreq.ack_eliciting_threshold = static_cast<uint8_t>(std::min<uint16_t>(ackEveryN, UINT8_MAX));
    ackFreq.reordering_threshold = 3;
    ackFreq.max_ack_delay_ms = config ? config->ack_delay : 150;
    ackFreq.timestamp = eular::utp::time::MonotonicMs();

    const int32_t encoded = ackFreq.encode(buffer, size);
    if (encoded < 0) {
        return -1;
    }

    outSize = static_cast<size_t>(encoded);
    return UTP_ERR_OK;
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
    const int32_t encoded = padding.encode(payload.data() + oldSize, remain);
    if (encoded < 0 || static_cast<size_t>(encoded) != remain) {
        return -1;
    }

    return UTP_ERR_OK;
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
    m_connTimer.reset(ctx->loop(), [this] () {
        onConnTimeout();
    });

    m_scheduleTimer.reset(ctx->loop(), [this] () {
        onWrite();
    });

    m_pathValidationTimer.reset(ctx->loop(), [this] () {
        onPathValidationTimeout();
    });

    m_handshakeDoneTimer.reset(ctx->loop(), [this] () {
        onHandshakeDoneTimeout();
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
}

ConnectionImpl::~ConnectionImpl() = default;

int32_t ConnectionImpl::connect(const Context::ConnectInfo &info)
{
    if (m_state != State::kStateDisconnected) {
        return UTP_ERR_INVALID_STATE;
    }

    m_state = State::kStateWaitSendInitial;
    m_connectInfo = info;

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
    m_lastActivityUs = 0;
    m_keepaliveMissedProbes = 0;
    m_keepaliveTimer.stop();

    m_ctx->wantWrite(this);
    m_connTimer.start(info.timeout);
    return UTP_ERR_OK;
}

int32_t ConnectionImpl::initPassive(const Context::ConnectInfo &info,
                                   const Address &peerAddress,
                                   uint32_t peerConnectionID,
                                   const TransportParams &peerTp,
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
    m_peerAddress = peerAddress;
    m_peerConnectionID = peerConnectionID;
    m_peerTP = peerTp;
    m_x25519 = x25519;
    m_aesCtx = aesCtx;

    m_networkPath.bindPeerAddress(peerAddress);
    m_mtuDiscovery.setAddressFamily(peerAddress.family());

    m_state = State::kStateConnected;
    markPeerActivity(time::MonotonicUs());
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

    std::memcpy(const_cast<uint8_t *>(packet->raw_data), msg.data, msg.len);
    packet->raw_size = msg.len;

    if (packet->decode(packet->raw_data, packet->raw_size) < 0) {
        if (!m_aesCtx) {
            return;
        }

        std::unique_ptr<PacketIn, decltype(packetReleaser)> encryptedPacket(
            m_mm.getPacketIn(static_cast<uint32_t>(msg.len)),
            packetReleaser);
        if (!encryptedPacket) {
            return;
        }

        std::memcpy(const_cast<uint8_t *>(encryptedPacket->raw_data), msg.data, msg.len);
        encryptedPacket->raw_size = msg.len;

        const uint8_t *offset = encryptedPacket->raw_data;
        size_t left = encryptedPacket->raw_size;
        offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.scid);
        if (offset == nullptr) {
            return;
        }
        offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.dcid);
        if (offset == nullptr) {
            return;
        }
        offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.pn);
        if (offset == nullptr) {
            return;
        }
        offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.payload_length);
        if (offset == nullptr) {
            return;
        }
        offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.types);
        if (offset == nullptr) {
            return;
        }
        offset = Serialize::DeserializeFrom(offset, left, encryptedPacket->header.reserve);
        if (offset == nullptr) {
            return;
        }

        if (left < encryptedPacket->header.payload_length) {
            return;
        }

        encryptedPacket->payload = offset;
        encryptedPacket->payload_size = encryptedPacket->header.payload_length;
        if (m_aesCtx->decrypt(encryptedPacket.get()) != UTP_ERR_OK) {
            return;
        }

        if (packet->decode(encryptedPacket->raw_data, encryptedPacket->raw_size) < 0) {
            return;
        }
    }

    const bool isPassiveInitial = (m_state == State::kStateDisconnected)
                               && packet->header.types == UTP_TYPE_INITIAL
                               && packet->header.dcid == 0;
    if (packet->header.dcid != m_localConnectionID && !isPassiveInitial) {
        return;
    }

    m_bytesIn += msg.len;
    m_receiveHistory.insert(packet->header.pn, nowUs);
    m_peerConnectionID = packet->header.scid;
    m_peerAddress = msg.metaInfo.peerAddress;
    m_mtuDiscovery.setAddressFamily(m_peerAddress.family());
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

        if (peerCloseInTimedWait && m_closePeerResendCount < kMaxCloseResendCount) {
            const utp_time_t nowTimedWaitUs = time::MonotonicUs();
            const utp_time_t resendGuardUs = std::max<utp_time_t>(m_closePtoUs / 2, 1000);
            if (m_closeLastSendUs == 0 || nowTimedWaitUs - m_closeLastSendUs >= resendGuardUs) {
                if (sendConnectionCloseFrame() == UTP_ERR_OK) {
                    m_closeLastSendUs = nowTimedWaitUs;
                    ++m_closePeerResendCount;
                }
            }
        }
        return;
    }

    if (m_state == State::kStateDisconnected) {
        return;
    }

    const bool closingState = m_state == State::kStateCloseSent
                           || m_state == State::kStateCloseReceived;

    if (!closingState && m_networkPath.detectPeerAddressChange(msg.metaInfo.peerAddress)) {
        maybeSendPathChallenge();
    }

    size_t frameOffset = 0;
    bool peerCloseReceived = false;
    while (frameOffset < packet->payload_size) {
        FrameType frameType = kFrameInvalid;
        const uint8_t *frameData = nullptr;
        size_t frameLen = 0;
        if (packet->nextFrame(frameOffset, frameType, frameData, frameLen) < 0) {
            break;
        }

        if (closingState && frameType != kFrameConnectionClose) {
            continue;
        }

        switch (frameType) {
        case kFrameStream: {
            FrameStream streamFrame;
            if (streamFrame.decode(frameData, frameLen) >= 0) {
                auto it = m_streams.find(streamFrame.stream_id);
                if (it == m_streams.end()) {
                    StreamImpl::SP stream = std::make_shared<StreamImpl>(this, streamFrame.stream_id);
                    m_streams.emplace(streamFrame.stream_id, stream);
                    if (m_onStreamCreated) {
                        m_onStreamCreated(stream.get());
                    }
                    it = m_streams.find(streamFrame.stream_id);
                }

                if (it != m_streams.end()) {
                    (void)it->second->onFrame(streamFrame);
                }
            }
            break;
        }
        case kFramePathChallenge:
            handlePathChallengeFrame(frameData, frameLen);
            break;
        case kFramePathResponse:
            handlePathResponseFrame(frameData, frameLen);
            break;
        case kFrameAck: {
            AckInfo ackInfo;
            ackInfo.reset();

            FrameAck ack;
            ack._ackInfo = &ackInfo;
            ack._params = &m_peerTP;
            if (ack.decode(frameData, frameLen) >= 0 && m_sendCtl) {
                m_sendCtl->onAckReceived(ackInfo, nowUs);
            }
            break;
        }
        case kFrameCrypto: {
            TransportParams peerTp;
            std::array<uint8_t, FRAME_CRYPTO_EPH_PUBKEY_SIZE> peerPubkey{};

            FrameCrypto crypto;
            crypto.tp = &peerTp;
            crypto.eph_pubkey = peerPubkey.data();
            if (crypto.decode(frameData, frameLen) >= 0) {
                m_peerTP = peerTp;
                if (!m_x25519) {
                    m_x25519 = std::make_shared<X25519Wrapper>();
                }

                try {
                    X25519Wrapper::PublicKey peerPublicKey;
                    std::memcpy(peerPublicKey.data(), peerPubkey.data(), peerPublicKey.size());

                    auto sharedSecretShort = m_x25519->deriveSharedSecretShort(peerPublicKey);
                    AesGcmContext::AesKey128 key;
                    std::copy(sharedSecretShort.begin(), sharedSecretShort.end(), key.begin());

                    if (!m_aesCtx) {
                        m_aesCtx = std::make_shared<AesGcmContext>();
                    }

                    const uint32_t noncePrefix = m_localConnectionID ^ m_peerConnectionID;
                    if (!m_aesCtx->init(key, noncePrefix)) {
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
            if (m_lastErrorCode == UTP_ERR_OK) {
                m_lastErrorCode = UTP_ERR_CANCELLED;
                m_lastErrorReason = "peer closed connection";
            }
            break;
        default:
            break;
        }
    }

    if (peerCloseReceived) {
        m_closeErrorCode = UTP_ERR_CANCELLED;
        m_closeReason = "peer close ack";
        m_closeFramePending = true;
        m_closePtoUs = closePtoUs();
        m_closeDeadlineUs = 0;
        m_closeLastSendUs = 0;
        m_closePeerResendCount = 0;
        scheduleWrite();
        return;
    }

    if (closingState) {
        return;
    }

    const uint32_t ackMask = (1u << static_cast<uint32_t>(kFrameAck));
    const bool ackOnly = packet->frame_types == ackMask;
    if (!ackOnly && !m_receiveHistory.empty() && m_ctx != nullptr && m_peerConnectionID != 0) {
        FrameAck ackFrame;
        ackFrame._history = &m_receiveHistory;
        ackFrame._params = &m_loaclTP;
        ackFrame._config = m_ctx->config();
        ackFrame._now = nowUs;

        std::array<uint8_t, 1500> ackPayload{};
        int32_t ackLen = ackFrame.encode(ackPayload.data(), ackPayload.size());
        if (ackLen > 0) {
            (void)sendPacket(UTP_TYPE_CTRL, ackPayload.data(), static_cast<size_t>(ackLen));
        }
    }

    if (m_state == State::kStateDisconnected && packet->header.types == UTP_TYPE_INITIAL) {
        const bool encryptedHandshake = (m_aesCtx != nullptr);
        if (sendHandshakePacket(encryptedHandshake) == UTP_ERR_OK) {
            m_state = State::kStateConnected;
            markPeerActivity(nowUs);
        }
        return;
    }

    if (m_state == State::kStateInitialSent && packet->header.types == UTP_TYPE_HANDSHAKE) {
        m_state = State::kStateConnected;
        m_handshakeDonePending = true;
        m_handshakeDoneSent = false;
        armHandshakeDoneTimer();
        m_connTimer.stop();
        markPeerActivity(nowUs);
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
        m_state = State::kStateDisconnected;
        m_keepaliveTimer.stop();
        scheduleWrite();
        return;
    }

    if (m_state == State::kStateWaitSendInitial) {
        int32_t status = sendInitialPacket();
        if (status != UTP_ERR_OK) {
            int32_t err = GetLastError();
            if (err <= 0) {
                err = UTP_ERR_SOCKET_WRITE;
            }
            m_lastErrorCode = err;
            m_lastErrorReason = "send initial packet failed";
            m_state = State::kStateDisconnected;
            m_connTimer.stop();
            m_handshakeDoneTimer.stop();
            m_pathValidationTimer.stop();
            m_keepaliveTimer.stop();
            return;
        }

        m_state = State::kStateInitialSent;
    }

    if (m_networkPath.needPathValidation() && !m_networkPath.hasInFlightChallenge()) {
        maybeSendPathChallenge();
    }

    if (m_sendCtl) {
        m_sendCtl->onCanWrite(time::MonotonicUs());
    }

    flushPendingStreamWrites();
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

void ConnectionImpl::flushPendingStreamWrites()
{
    for (auto &entry : m_streams) {
        if (entry.second) {
            const int32_t status = entry.second->flushPendingSends();
            if (status == UTP_ERR_WOULD_BLOCK) {
                return;
            }
        }
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

    if (m_state != State::kStateConnected) {
        return UTP_ERR_INVALID_STATE;
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

    const bool piggybackHandshakeDone = m_handshakeDonePending && !m_handshakeDoneSent && len > 0;
    if (piggybackHandshakeDone) {
        std::vector<uint8_t> streamBody;
        streamBody.reserve(len + 1);
        streamBody.insert(streamBody.end(), data, data + len);
        streamBody.push_back(static_cast<uint8_t>(kFrameHandshakeDone));

        const int32_t status = sendPacket(UTP_TYPE_CTRL,
                                          header.data(),
                                          header.size(),
                                          streamBody.data(),
                                          streamBody.size(),
                                          0);
        if (status == UTP_ERR_OK) {
            m_handshakeDoneSent = true;
            m_handshakeDonePending = false;
            m_handshakeDoneTimer.stop();
        }
        return status;
    }

    return sendPacket(UTP_TYPE_CTRL,
                      header.data(),
                      header.size(),
                      data,
                      len,
                      0);
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

    int32_t err = GetLastError();
    if (err <= 0) {
        err = UTP_ERR_SOCKET_WRITE;
    }
    return err;
}

int32_t ConnectionImpl::sendHandshakeDonePacket()
{
    const uint8_t payload[1] = {static_cast<uint8_t>(kFrameHandshakeDone)};
    const int32_t status = sendPacket(UTP_TYPE_CTRL, payload, sizeof(payload));
    if (status == UTP_ERR_OK) {
        m_handshakeDoneSent = true;
        m_handshakeDonePending = false;
        m_handshakeDoneTimer.stop();
    }
    return status;
}

int32_t ConnectionImpl::sendInitialPacket()
{
    std::vector<uint8_t> payload;

    if (m_connectInfo.encrypted) {
        if (!m_x25519) {
            m_x25519 = std::make_shared<X25519Wrapper>();
        }

        FrameCrypto crypto;
        crypto.crypto_type = kFrameCryptoAESGCM128;
        crypto.tp_size = static_cast<uint8_t>(TransportParams::kMaxNumeric);
        crypto.tp = &m_loaclTP;
        crypto.eph_pubkey = const_cast<uint8_t *>(m_x25519->publicKey().data());

        std::array<uint8_t, FRAME_CRYPTO_SIZE> cryptoPayload{};
        int32_t frameLen = crypto.encode(cryptoPayload.data(), cryptoPayload.size());
        if (frameLen < 0) {
            return -1;
        }
        payload.insert(payload.end(), cryptoPayload.begin(), cryptoPayload.begin() + frameLen);
    } else {
        FrameVersion version;
        version.version = 1;
        std::array<uint8_t, FRAME_VERSION_SIZE> versionPayload{};
        int32_t frameLen = version.encode(versionPayload.data(), versionPayload.size());
        if (frameLen < 0) {
            return -1;
        }
        payload.insert(payload.end(), versionPayload.begin(), versionPayload.begin() + frameLen);
    }

    std::array<uint8_t, FRAME_ACK_FREQUENCY_SIZE> ackFreqPayload{};
    size_t ackFreqSize = 0;
    if (BuildAckFrequencyFrame(m_ctx ? m_ctx->config() : nullptr,
                               ackFreqPayload.data(),
                               ackFreqPayload.size(),
                               ackFreqSize) != UTP_ERR_OK) {
        return -1;
    }
    payload.insert(payload.end(), ackFreqPayload.begin(), ackFreqPayload.begin() + ackFreqSize);

    const Address::Family family = m_peerAddress.isValid() ? m_peerAddress.family() : Address::IPv4;
    const uint16_t targetPacketSize = MtuDiscovery::PacketSizeFromMtu(ETHERNET_MTU_MIN, family);
    if (targetPacketSize > UTP_HEADER_SIZE) {
        const size_t targetPayloadSize = static_cast<size_t>(targetPacketSize - UTP_HEADER_SIZE);
        if (AppendPaddingToTargetPayloadSize(targetPayloadSize, payload) != UTP_ERR_OK) {
            return -1;
        }
    }

    return sendPacket(UTP_TYPE_INITIAL,
                      payload.data(),
                      payload.size(),
                      PacketOutFlags::kPoHello);
}

int32_t ConnectionImpl::sendHandshakePacket(bool encrypted)
{
    std::vector<uint8_t> payload;

    if (encrypted) {
        if (!m_x25519) {
            m_x25519 = std::make_shared<X25519Wrapper>();
        }

        FrameCrypto crypto;
        crypto.crypto_type = kFrameCryptoAESGCM128;
        crypto.tp_size = static_cast<uint8_t>(TransportParams::kMaxNumeric);
        crypto.tp = &m_loaclTP;
        crypto.eph_pubkey = const_cast<uint8_t *>(m_x25519->publicKey().data());

        std::array<uint8_t, FRAME_CRYPTO_SIZE> cryptoPayload{};
        int32_t frameLen = crypto.encode(cryptoPayload.data(), cryptoPayload.size());
        if (frameLen < 0) {
            return -1;
        }
        payload.insert(payload.end(), cryptoPayload.begin(), cryptoPayload.begin() + frameLen);
    } else {
        FrameVersion version;
        version.version = 1;
        std::array<uint8_t, FRAME_VERSION_SIZE> versionPayload{};
        int32_t frameLen = version.encode(versionPayload.data(), versionPayload.size());
        if (frameLen < 0) {
            return -1;
        }
        payload.insert(payload.end(), versionPayload.begin(), versionPayload.begin() + frameLen);
    }

    std::array<uint8_t, FRAME_ACK_FREQUENCY_SIZE> ackFreqPayload{};
    size_t ackFreqSize = 0;
    if (BuildAckFrequencyFrame(m_ctx ? m_ctx->config() : nullptr,
                               ackFreqPayload.data(),
                               ackFreqPayload.size(),
                               ackFreqSize) != UTP_ERR_OK) {
        return -1;
    }
    payload.insert(payload.end(), ackFreqPayload.begin(), ackFreqPayload.begin() + ackFreqSize);

    return sendPacket(UTP_TYPE_HANDSHAKE,
                      payload.data(),
                      payload.size(),
                      PacketOutFlags::kPoHello);
}

int32_t ConnectionImpl::sendPacket(uint8_t packetType,
                                   const void *payload,
                                   size_t payloadLen,
                                   uint16_t packetFlags,
                                   utp_packno_t *outPacketNo)
{
    return sendPacket(packetType,
                      payload,
                      payloadLen,
                      nullptr,
                      0,
                      packetFlags,
                      outPacketNo);
}

int32_t ConnectionImpl::sendPacket(uint8_t packetType,
                                   const void *payloadHead,
                                   size_t payloadHeadLen,
                                   const void *payloadBody,
                                   size_t payloadBodyLen,
                                   uint16_t packetFlags,
                                   utp_packno_t *outPacketNo)
{
    if (m_udpSocket == nullptr) {
        SetLastErrorV(UTP_ERR_SOCKET_WRITE, "udp socket is null");
        return -1;
    }

    if (!m_peerAddress.isValid()) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "peer address is invalid");
        return -1;
    }

    const size_t payloadLen = payloadHeadLen + payloadBodyLen;
    if (payloadLen > UINT16_MAX) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "payload length {} exceeds uint16 max", payloadLen);
        return -1;
    }

    if (m_networkPath.state() == NetworkPath::kPathFailed) {
        SetLastErrorV(UTP_ERR_INVALID_STATE, "network path validation failed");
        return -1;
    }

    FrameType frameType = kFrameInvalid;
    if (payloadHead != nullptr && payloadHeadLen > 0) {
        frameType = static_cast<FrameType>(*(static_cast<const uint8_t *>(payloadHead)));
    } else if (payloadBody != nullptr && payloadBodyLen > 0) {
        frameType = static_cast<FrameType>(*(static_cast<const uint8_t *>(payloadBody)));
    }

    const bool shouldEncrypt = (m_aesCtx != nullptr)
                            && ((packetFlags & PacketOutFlags::kPoNoEncrypt) == 0)
                            && packetType != UTP_TYPE_INITIAL
                            && packetType != UTP_TYPE_HANDSHAKE
                            && frameType != kFrameCrypto
                            && frameType != kFrameVersion;
    const size_t encryptOverhead = shouldEncrypt ? static_cast<size_t>(AesGcmContext::GCM_TAG_SIZE) : 0;

    size_t packetLen = UTP_HEADER_SIZE + payloadLen;
    const size_t wirePacketLen = packetLen + encryptOverhead;
    if (wirePacketLen > UINT16_MAX) {
        SetLastErrorV(UTP_ERR_OVERFLOW, "packet length {} exceeds uint16 max", wirePacketLen);
        return -1;
    }

    const bool isMtuProbePacket = (packetFlags & PacketOutFlags::kPoMtuProbe) != 0;
    if (!isMtuProbePacket && wirePacketLen > m_mtuDiscovery.currentMaxPacketSize()) {
        SetLastErrorV(UTP_ERR_WOULD_BLOCK,
                      "packet length {} exceeds current path MTU budget {}",
                      wirePacketLen,
                      m_mtuDiscovery.currentMaxPacketSize());
        return -1;
    }

    if (isMtuProbePacket && wirePacketLen > m_mtuDiscovery.absoluteMaxPacketSize()) {
        SetLastErrorV(UTP_ERR_WOULD_BLOCK,
                      "mtu probe packet length {} exceeds probe ceiling {}",
                      wirePacketLen,
                      m_mtuDiscovery.absoluteMaxPacketSize());
        return -1;
    }

    if (!canSendOnCurrentPath(wirePacketLen, frameType)) {
        uint32_t frameBits = 0;
        if (frameType < kFrameMax) {
            frameBits = (1u << static_cast<uint32_t>(frameType));
        }
        SetLastErrorV(UTP_ERR_WOULD_BLOCK,
                      "anti-amplification limit reached while path validating, frame={}",
                      frameBits != 0 ? FrameTypeToString(frameBits) : "Unknown");
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
    if (frameType < kFrameMax) {
        packet->frame_types = (1u << static_cast<uint32_t>(frameType));
    }

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
    msg.metaInfo.peerAddress = m_peerAddress;

    std::vector<UdpSocket::MsgMetaInfo> msgVec(1, msg);
    int32_t sent = m_udpSocket->send(msgVec);
    if (sent <= 0) {
        m_mm.putPacketOut(packet);
        return -1;
    }

    m_bytesOut += packet->data_size;

    const bool shouldTrackPacket = frameType != kFrameAck;
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

    if (sendPacket(UTP_TYPE_CTRL, payload, static_cast<size_t>(frameLen)) != UTP_ERR_OK) {
        return;
    }

    utp_time_t nowMs = time::MonotonicMs();
    utp_time_t deadlineMs = m_networkPath.challengeDeadlineMs();
    utp_time_t timeoutMs = deadlineMs > nowMs ? (deadlineMs - nowMs) : 1;
    m_pathValidationTimer.start(timeoutMs);
}

void ConnectionImpl::handlePathChallengeFrame(const uint8_t *frameData, size_t frameSize)
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

    sendPacket(UTP_TYPE_CTRL, payload, static_cast<size_t>(frameLen));
}

void ConnectionImpl::handlePathResponseFrame(const uint8_t *frameData, size_t frameSize)
{
    FramePathResponse response;
    if (response.decode(frameData, frameSize) < 0) {
        return;
    }

    if (m_networkPath.onPathResponse(response)) {
        m_pathValidationTimer.stop();
    }
}

void ConnectionImpl::onPathValidationTimeout()
{
    if (m_networkPath.onTimeout(time::MonotonicMs())) {
        if (m_networkPath.state() == NetworkPath::kPathFailed) {
            m_lastErrorCode = UTP_ERR_TIMEOUT;
            m_lastErrorReason = "path validation timeout";
            m_state = kStateDisconnected;
            m_pathValidationTimer.stop();
            m_keepaliveTimer.stop();
            scheduleWrite();
        }
        return;
    }

    if (m_networkPath.needPathValidation() && !m_networkPath.hasInFlightChallenge()) {
        maybeSendPathChallenge();
    }
}

void ConnectionImpl::onHandshakeDoneTimeout()
{
    if (!m_handshakeDonePending || m_handshakeDoneSent || m_state != State::kStateConnected) {
        return;
    }

    const int32_t status = sendHandshakeDonePacket();
    if (status != UTP_ERR_OK) {
        scheduleWrite();
        m_handshakeDoneTimer.start(10);
    }
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
    if (m_ctx != nullptr && m_ctx->config() != nullptr && m_ctx->config()->handshake_timeout > 0) {
        handshakeTimeoutMs = m_ctx->config()->handshake_timeout;
    } else if (m_connectInfo.timeout > 0) {
        handshakeTimeoutMs = m_connectInfo.timeout;
    } else {
        handshakeTimeoutMs = 1000;
    }

    const uint32_t delayMs = handshakeTimeoutMs / 2;
    return delayMs > 0 ? delayMs : 1;
}

void ConnectionImpl::onConnTimeout()
{
    if (m_state == State::kStateConnected
        || m_state == State::kStateDisconnected
        || m_state == State::kStateCloseSent
        || m_state == State::kStatePtoTimedWait) {
        return;
    }

    m_lastErrorCode = UTP_ERR_TIMEOUT;
    m_lastErrorReason = "connect timeout";
    m_state = State::kStateDisconnected;
    m_handshakeDoneTimer.stop();
    m_keepaliveTimer.stop();
    scheduleWrite();
}

uint32_t ConnectionImpl::keepaliveIntervalMs() const
{
    if (m_ctx == nullptr || m_ctx->config() == nullptr) {
        return 1000;
    }

    const Config *cfg = m_ctx->config();
    if (cfg->keepalive_interval > 0) {
        return cfg->keepalive_interval;
    }

    const uint32_t srttMs = static_cast<uint32_t>(m_rttStats.srtt() / 1000);
    const uint32_t guardMs = 3 * srttMs;
    if (cfg->max_idle_timeout > guardMs + 1) {
        return cfg->max_idle_timeout - guardMs;
    }

    return std::max<uint32_t>(cfg->max_idle_timeout / 2, 1);
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
        m_state = State::kStateDisconnected;
        m_keepaliveTimer.stop();
        scheduleWrite();
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

    utp_time_t maxAckDelayUs = static_cast<utp_time_t>(m_peerTP.max_ack_delay) * 1000;
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
    m_keepaliveTimer.stop();
    scheduleWrite();
}

void ConnectionImpl::registerStreamCanCreate(const OnStreamCanCreate &cb)
{
    m_onStreamCanCreate = cb;
}

void ConnectionImpl::registerStreamCreated(const OnStreamCreated &cb)
{
    m_onStreamCreated = cb;
}

int32_t ConnectionImpl::streamCount() const
{
    return static_cast<int32_t>(m_streams.size());
}

Connection::Statistic ConnectionImpl::statistic() const
{
    Connection::Statistic stat;
    std::memset(&stat, 0, sizeof(stat));
    stat.pmtu = m_mtuDiscovery.pathMtu();
    stat.rtt = static_cast<uint32_t>(m_rttStats.srtt());
    stat.rttvar = static_cast<uint32_t>(m_rttStats.rttVar());
    stat.rx_bytes = m_bytesIn;
    stat.tx_bytes = m_bytesOut;
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

int32_t ConnectionImpl::createStream()
{
    if (m_state != State::kStateConnected) {
        return UTP_ERR_INVALID_STATE;
    }

    const uint32_t maxStreams = std::max<uint32_t>(1, m_peerTP.init_max_streams_bidi);
    if (m_streams.size() >= maxStreams) {
        return UTP_ERR_STREAM_LIMIT_ERROR;
    }

    const bool isClientInitiator = !m_connectInfo.ip.empty();
    uint32_t &nextStreamId = m_streamId[0];
    if (nextStreamId == 0) {
        nextStreamId = isClientInitiator ? 0u : 1u;
    }

    const uint32_t streamId = nextStreamId;
    nextStreamId += 4;

    if (m_streams.find(streamId) != m_streams.end()) {
        return UTP_ERR_STREAM_ID_EXHAUSTED;
    }

    StreamImpl::SP stream = std::make_shared<StreamImpl>(this, streamId);
    m_streams.emplace(streamId, stream);

    if (m_onStreamCreated) {
        m_onStreamCreated(stream.get());
    }

    if (m_onStreamCanCreate && (m_streams.size() < maxStreams)) {
        m_onStreamCanCreate();
    }

    return static_cast<int32_t>(streamId);
}

void ConnectionImpl::close()
{
    if (m_state == State::kStateDisconnected) {
        return;
    }

    m_state = State::kStateCloseSent;
    m_connTimer.stop();
    m_handshakeDoneTimer.stop();
    m_pathValidationTimer.stop();
    m_keepaliveTimer.stop();

    m_closeErrorCode = UTP_ERR_CANCELLED;
    m_closeReason = "local close";
    m_closeFramePending = true;
    m_closePeerResendCount = 0;

    scheduleWrite();
}

} // namespace utp
} // namespace eular
