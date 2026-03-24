/*************************************************************************
    > File Name: connection_impl.h
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:40:12 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_CONNECTION_H__
#define __UTP_CONTEXT_CONNECTION_H__

#include <memory>
#include <map>
#include <unordered_map>
#include <vector>

#include <event/timer.h>

#include "utp/types.h"
#include "utp/connection.h"
#include "utp/utp.h"

#include "socket/udp.h"

#include "context/stream_impl.h"

#include "congestion/rtt.h"

#include "util/mm.h"
#include "util/malo.hpp"
#include "util/transport_param.h"
#include "util/network_path.h"
#include "util/receive_history.h"
#include "mtu/mtu.h"

namespace eular {
namespace utp {

class ContextImpl;
class X25519Wrapper;
class AesGcmContext;
class SendControl;
struct FrameAckFrequency;
struct FrameResetStream;

class ConnectionImpl : public Connection
{
public:
    using SP = std::shared_ptr<ConnectionImpl>;
    using WP = std::weak_ptr<ConnectionImpl>;

    struct ZeroRttConfig {
        enum Source : uint8_t {
            kSourceNone = 0,
            kSourceSessionToken,
            kSourceResumptionState,
        };

        std::vector<uint8_t> sessionTicket;
        std::vector<uint8_t> resumptionPsk;
        std::vector<uint8_t> earlyData;
        bool earlyFin{false};
        Source source{kSourceNone};
        uint64_t expiresAtSec{0};

        bool enabled() const { return !sessionTicket.empty(); }
    };

    enum State : uint8_t {
        kStateWaitSendInitial,      // 等待发送初始包
        kStateInitialSent,          // 已发送初始包
        kStateHandshakeSent,        // 已发送握手包
        kStateHandshakeReceived,    // 已收到握手包
        kStateConnected,            // 已连接
        kStateCloseSent,            // 已发送关闭包
        kStateCloseReceived,        // 收到关闭包
        kStatePtoTimedWait,         // PTO超时等待
        kStateDisconnected,         // 断连状态
    };

    enum AckFrequencyProfile : uint8_t {
        kAckProfileStable = 0,
        kAckProfileLatencySensitive,
        kAckProfileLossy,
    };

    ConnectionImpl(ContextImpl *ctx, UdpSocket *udpSocket, uint32_t cid);
    ~ConnectionImpl();

    int32_t connect(const Context::ConnectInfo &info, const ZeroRttConfig *zeroRtt = nullptr);
    int32_t initPassive(const Context::ConnectInfo &info,
                        const Address &peerAddress,
                        uint32_t peerConnectionID,
                        const TransportParams &peerTp,
                        const std::shared_ptr<X25519Wrapper> &x25519,
                        const std::shared_ptr<AesGcmContext> &aesCtx);

    void    onUdpPacket(const UdpSocket::MsgMetaInfo &msg);
    void    onWrite();

    // @brief 下一次调度时间(ms), send control触发
    void    nextScheduleTime(utp_time_t timeNext);

public:
    void        registerStreamCreated(const OnStreamCreated &cb) override;
    void        setOnSessionTokenReady(const OnSessionTokenReady &cb) override;
    int32_t     streamCount(StreamType streamType = kStreamTypeAll) const override;
    int32_t     creatableStreamCount(StreamType streamType) const override;
    Statistic   statistic() const override;
    Description description() const override;
    int32_t     exportSessionToken(std::vector<uint8_t> &outToken) override;
    int32_t     exportSessionResumptionState(std::string &outState) override;

    int32_t     createStream(StreamType streamType = kStreamTypeBidirectional) override;
    Stream*     getStream(uint32_t streamId) override;
    void        close() override;
    int32_t     ingestEarlyStreamFrame(uint32_t streamId,
                                       uint64_t streamOffset,
                                       const uint8_t *data,
                                       size_t len,
                                       bool fin);

public:
    uint64_t    packetNumber() { return m_packetNumber++; }
    uint32_t    cid() const { return m_localConnectionID; }
    const Context::ConnectInfo& connectInfo() const { return m_connectInfo; }
    const Context::ConnectAttemptInfo& connectAttemptInfo() const { return m_connectAttemptInfo; }
    State       state() const { return m_state; }
    int32_t     lastErrorCode() const { return m_lastErrorCode; }
    const std::string& lastErrorReason() const { return m_lastErrorReason; }

private:
    struct CachedResumptionState {
        Context::EncryptionMode encrypted{Context::kEncryptionNone};
        std::vector<uint8_t> sessionTicket;
        std::vector<uint8_t> resumptionPsk;
    };

    void cacheSessionResumptionState(const CachedResumptionState &info, uint64_t expiresAt);
    int32_t buildSessionResumptionState(const CachedResumptionState &info,
                                        uint64_t expiresAt,
                                        std::string &outState) const;

    void scheduleWrite();
    void collectClosedStreams();
    size_t activeStreamCount(StreamType streamType = kStreamTypeAll) const;
    size_t activeLocalStreamCount(StreamType streamType) const;
    uint32_t streamLimit(StreamType streamType, bool peerLimit) const;
    uint32_t streamIdSlot(StreamType streamType) const;
    int32_t validateIncomingStreamId(uint32_t streamId) const;
    int32_t ingestStreamFrame(const FrameStream &streamFrame);
    void flushPendingStreamWrites();
    int32_t sendConnectionCloseFrame();
    int32_t sendResetStreamFrame(uint32_t streamId, uint16_t errorCode, uint64_t finalSize);
    void    handleResetStreamFrame(const FrameResetStream &resetFrame);
    int32_t sendStreamFrame(uint32_t streamId,
                            uint64_t streamOffset,
                            const uint8_t *data,
                            size_t len,
                            bool fin);
    int32_t sendHandshakeDonePacket();
    int32_t maybeSendSessionTokenPacket();
    int32_t sendInitialPacket();
    int32_t sendHandshakePacket(bool encrypted);
    int32_t buildAckPayload(std::vector<uint8_t> &payload, utp_time_t nowUs) const;
    int32_t sendAckPacket(utp_time_t nowUs);
    void    noteAckElicitingPacket(utp_time_t nowUs);
    void    applyAckFrequency(const FrameAckFrequency &ackFreq, utp_time_t nowMs);
    void    maybeUpdateAckFrequency(utp_time_t nowUs);
    AckFrequencyProfile selectDesiredAckProfile(utp_time_t nowUs);
    utp_time_t ackProfileTransitionHoldUs(AckFrequencyProfile from, AckFrequencyProfile to) const;
    int32_t sendAckFrequencyUpdate(AckFrequencyProfile profile, utp_time_t nowUs);
    void    armAckTimer(uint32_t delayMs);
    void    stopAckTimer();
    void    onAckTimeout();
    int32_t sendPacket(uint8_t packetType,
                       const void *payload,
                       size_t payloadLen,
                       uint16_t packetFlags = 0,
                       utp_packno_t *outPacketNo = nullptr,
                       uint32_t frameTypeBitsOverride = 0,
                       const Address *targetAddress = nullptr);
    int32_t sendPacket(uint8_t packetType,
                       const void *payloadHead,
                       size_t payloadHeadLen,
                       const void *payloadBody,
                       size_t payloadBodyLen,
                       uint16_t packetFlags,
                       utp_packno_t *outPacketNo = nullptr,
                       uint32_t frameTypeBitsOverride = 0,
                       const Address *targetAddress = nullptr);
    bool    canSendOnCurrentPath(size_t packetLen, FrameType frameType) const;
    void    maybeSendPathChallenge();
    void    handlePathChallengeFrame(const uint8_t *frameData, size_t frameSize, const Address &fromAddress);
    void    handlePathResponseFrame(const uint8_t *frameData, size_t frameSize, const Address &fromAddress);
    void    onPathValidationTimeout();
    void    onHandshakeDoneTimeout();
    void    onKeepaliveTimeout();
    void    onCloseDrainTimeout();
    void    enterPtoTimedWait();
    utp_time_t closePtoUs() const;
    uint32_t keepaliveIntervalMs() const;
    void    armKeepaliveTimer(uint32_t delayMs);
    void    markPeerActivity(utp_time_t nowUs);
    void    beginCloseSent(uint16_t errorCode, const std::string &reason);
    void    armHandshakeDoneTimer();
    uint32_t handshakeDoneDelayMs() const;
    void    onConnTimeout();
    void    trySendZeroRttEarlyData();

private:
    friend class SendControl;
    friend class StreamImpl;

    ContextImpl*            m_ctx{};
    UdpSocket*              m_udpSocket{};
    State                   m_state{kStateDisconnected};
    TransportParams         m_loaclTP{};
    TransportParams         m_peerTP{};
    MemoryManager           m_mm;
    RttStats                m_rttStats;

    Context::ConnectInfo    m_connectInfo{};
    Context::ConnectAttemptInfo m_connectAttemptInfo{};
    ev::EventTimer          m_connTimer;
    ev::EventTimer          m_scheduleTimer;

    uint32_t                m_localConnectionID{};
    uint32_t                m_peerConnectionID{};
    uint64_t                m_packetNumber{1};
    Address                 m_peerAddress;
    NetworkPath             m_networkPath;

    using StreamMap = std::unordered_map<uint32_t, StreamImpl::SP>;
    uint32_t                m_streamId[STREAM_TYPES]{0};
    StreamMap               m_streams;
    std::shared_ptr<X25519Wrapper> m_x25519;
    std::shared_ptr<AesGcmContext> m_aesCtx;
    std::unique_ptr<SendControl>   m_sendCtl;

    OnStreamCreated         m_onStreamCreated;
    OnSessionTokenReady     m_onSessionTokenReady;

    uint64_t                m_bytesIn{};
    uint64_t                m_bytesOut{};
    ev::EventTimer          m_pathValidationTimer;
    ev::EventTimer          m_handshakeDoneTimer;
    ev::EventTimer          m_ackTimer;
    ev::EventTimer          m_keepaliveTimer;
    ev::EventTimer          m_closeDrainTimer;
    ReceiveHistory          m_receiveHistory;
    MtuDiscovery            m_mtuDiscovery;
    bool                    m_handshakeDonePending{false};
    bool                    m_handshakeDoneSent{false};
    bool                    m_closeFramePending{false};
    uint16_t                m_closeErrorCode{0};
    std::string             m_closeReason{"local close"};
    utp_time_t              m_closePtoUs{0};
    utp_time_t              m_closeDeadlineUs{0};
    utp_time_t              m_closeLastSendUs{0};
    utp_time_t              m_lastActivityUs{0};
    uint16_t                m_keepaliveMissedProbes{0};
    uint8_t                 m_closePeerResendCount{0};
    uint8_t                 m_ackElicitingThreshold{0};
    uint8_t                 m_ackReorderingThreshold{0};
    uint32_t                m_ackMaxDelayMs{0};
    uint32_t                m_ackElicitingSinceLastAck{0};
    utp_time_t              m_ackPendingSinceUs{0};
    utp_time_t              m_lastAckFrequencyApplyMs{0};
    AckFrequencyProfile     m_ackProfileCurrent{kAckProfileStable};
    AckFrequencyProfile     m_ackProfileCandidate{kAckProfileStable};
    utp_time_t              m_ackProfileCandidateSinceUs{0};
    utp_time_t              m_ackProfileLastSentMs{0};
    utp_time_t              m_ackProfileBaselineSrttUs{0};
    bool                    m_isClientInitiator{true};
    int32_t                 m_lastErrorCode{0};
    std::string             m_lastErrorReason;
    bool                    m_zeroRttEarlyDataSent{false};
    uint32_t                m_zeroRttEarlyStreamId{0};
    bool                    m_sessionTokenIssued{false};
    bool                    m_hasCachedResumptionState{false};
    CachedResumptionState   m_cachedResumptionInfo{};
    uint64_t                m_cachedResumptionExpiresAt{0};
    ZeroRttConfig           m_zeroRttConfig{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_CONNECTION_H__
