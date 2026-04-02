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
#include <array>
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
struct PacketOut;
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
    void        setOnIncomingStream(const OnIncomingStream &cb) override;
    void        setOnSessionTokenReady(const OnSessionTokenReady &cb) override;
    void        setOnError(const OnError &cb) override;
    void        setOnClosed(const OnClosed &cb) override;
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
    void        updateTag(const std::string &tag);
    const char* tag() const { return m_tag.c_str(); }

public:
    uint64_t    packetNumber() { return m_packetNumber++; }
    uint32_t    cid() const { return m_localConnectionID; }
    const Config* config() const;
    const Context::ConnectInfo& connectInfo() const { return m_connectInfo; }
    const Context::ConnectAttemptInfo& connectAttemptInfo() const { return m_connectAttemptInfo; }
    State       state() const { return m_state; }
    int32_t     lastErrorCode() const { return m_lastErrorCode; }
    const std::string& lastErrorReason() const { return m_lastErrorReason; }
    size_t      streamPayloadBudgetHint() const;
    bool        canSendStreamUnackedBytes(size_t streamBytes) const;
    void        onStreamPacketUnackedAdded(const PacketOut *pkt);
    void        onStreamPacketUnackedRemoved(const PacketOut *pkt);

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
                       const Address *targetAddress = nullptr,
                       size_t streamDataBytes = 0);
    int32_t sendPacket(uint8_t packetType,
                       const void *payloadHead,
                       size_t payloadHeadLen,
                       const void *payloadBody,
                       size_t payloadBodyLen,
                       uint16_t packetFlags,
                       utp_packno_t *outPacketNo = nullptr,
                       uint32_t frameTypeBitsOverride = 0,
                       const Address *targetAddress = nullptr,
                       size_t streamDataBytes = 0);
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
    void    onHandshakeDoneFrameAcked();
    void    bootstrapLocalTransportParams();
    void    onConnTimeout();
    void    trySendZeroRttEarlyData();
    void    notifyConnectionError(int32_t errorCode, const std::string &reason, bool fatal);
    void    notifyConnectionClosed(int32_t errorCode, const std::string &reason, bool byPeer);

    /// @brief 读取默认 stream 优先级（0最高，7最低）
    uint8_t defaultStreamPriority() const;
    /// @brief 读取当前调度策略（支持运行时热切换）
    StreamSchedulerMode streamSchedulerMode() const;
    /// @brief 对候选 stream 做基于 stream_id 的 RR 选取
    StreamImpl::SP pickRoundRobinStream(const std::vector<StreamImpl::SP> &candidates,
                                        uint32_t &cursor);
    /// @brief DISABLED 模式：按 stream_id RR 选流
    StreamImpl::SP pickNextWritableStreamDisabled();
    /// @brief STRICT+Aging 模式选流
    StreamImpl::SP pickNextWritableStreamStrict();
    /// @brief DRR 模式选流
    StreamImpl::SP pickNextWritableStreamDrr();
    /// @brief 按当前模式统一选流入口
    StreamImpl::SP pickNextWritableStream();
    /// @brief STRICT 模式下更新 aging 等待轮次
    void    updateStrictAgingState(uint32_t selectedStreamId);
    /// @brief 清理无效调度状态（已关闭流/空队列）
    void    pruneSchedulerState();
    /// @brief 记录一次选流结果并累计指标
    void    onSchedulerStreamSelected(const StreamImpl::SP &stream,
                                      StreamSchedulerMode mode,
                                      bool agingPromoted,
                                      uint8_t effectivePriority,
                                      uint32_t drrNeed,
                                      uint32_t drrDeficitBefore,
                                      uint32_t drrDeficitAfter);
    /// @brief 记录一次 mode 热切换
    void    noteSchedulerModeIfChanged(StreamSchedulerMode mode);
    /// @brief 周期打印调度统计指标
    void    maybeEmitSchedulerStats(utp_time_t nowUs);

private:
    friend class SendControl;
    friend class StreamImpl;

    std::string             m_tag;
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

    OnIncomingStream        m_onIncomingStream;
    OnSessionTokenReady     m_onSessionTokenReady;
    OnError                 m_onError;
    OnClosed                m_onClosed;

    uint64_t                m_bytesIn{};
    uint64_t                m_bytesOut{};
    uint64_t                m_bytesRetrans{};
    uint64_t                m_obsRttUs{0};            // 统计用 SRTT(单位:us)，仅用于 Connection::Statistic 导出
    uint64_t                m_obsRttVarUs{0};         // 统计用 RTTVAR(单位:us)，仅用于 Connection::Statistic 导出
    uint64_t                m_streamUnackedDataBytes{0};
    ev::EventTimer          m_pathValidationTimer;
    ev::EventTimer          m_handshakeDoneTimer;
    ev::EventTimer          m_ackTimer;
    ev::EventTimer          m_keepaliveTimer;
    ev::EventTimer          m_closeDrainTimer;
    ReceiveHistory          m_receiveHistory;
    MtuDiscovery            m_mtuDiscovery;
    bool                    m_handshakeDonePending{false};
    bool                    m_handshakeDoneSent{false};
    utp_packno_t            m_handshakeDoneLastPacketNo{0};
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
    uint32_t                m_peerAckMaxDelayMs{UTP_DEFAULT_MAX_ACK_DELAY_MS};
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
    bool                    m_closeByPeer{false};
    bool                    m_closedNotified{false};
    bool                    m_zeroRttEarlyDataSent{false};
    uint32_t                m_zeroRttEarlyStreamId{0};
    bool                    m_sessionTokenIssued{false};
    bool                    m_hasCachedResumptionState{false};
    CachedResumptionState   m_cachedResumptionInfo{};
    uint64_t                m_cachedResumptionExpiresAt{0};
    ZeroRttConfig           m_zeroRttConfig{};
    /// @b Stream 调度状态
    std::array<uint32_t, 8> m_strictRrCursor{{0}};     // STRICT: 每个优先级桶的 RR 游标
    uint32_t                m_disabledRrCursor{0};     // DISABLED: 全局 RR 游标
    uint32_t                m_drrCursor{0};            // DRR: 上次命中的 stream_id
    std::unordered_map<uint32_t, uint32_t> m_drrDeficit; // DRR: 每个 stream 的 deficit(bytes)

    /// @b Stream 调度指标
    struct {
        uint64_t            selectTotal{0};            // 总选流次数
        uint64_t            selectDisabled{0};         // DISABLED 模式选流次数
        uint64_t            selectStrict{0};           // STRICT 模式选流次数
        uint64_t            selectDrr{0};              // DRR 模式选流次数
        uint64_t            strictAgingPromoted{0};    // STRICT 中触发 aging 提升的次数
        uint64_t            wouldBlock{0};             // 选中流发送返回 WOULD_BLOCK 的次数
        uint64_t            emptyRounds{0};            // flush 中未选到可发流的轮次
        uint64_t            modeSwitches{0};           // 调度模式热切换次数
        uint64_t            drrDeficitRefills{0};      // DRR deficit 补充次数
        uint64_t            drrDeficitConsumes{0};     // DRR deficit 消耗次数
        utp_time_t          lastReportUs{0};           // 最近一次指标日志时间(us)
        StreamSchedulerMode lastMode{kStreamSchedulerStrict}; // 最近观测到的模式
    } m_schedulerStats;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_CONNECTION_H__
