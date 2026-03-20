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

class ConnectionImpl : public Connection
{
public:
    using SP = std::shared_ptr<ConnectionImpl>;
    using WP = std::weak_ptr<ConnectionImpl>;

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

    ConnectionImpl(ContextImpl *ctx, UdpSocket *udpSocket, uint32_t cid);
    ~ConnectionImpl();

    int32_t connect(const Context::ConnectInfo &info);
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
    void        registerStreamCanCreate(const OnStreamCanCreate &cb) override;
    void        registerStreamCreated(const OnStreamCreated &cb) override;
    int32_t     streamCount() const override;
    Statistic   statistic() const override;
    Description description() const override;

    int32_t     createStream() override;
    void        close() override;

public:
    uint64_t    packetNumber() { return m_packetNumber++; }
    uint32_t    cid() const { return m_localConnectionID; }
    const Context::ConnectInfo& connectInfo() const { return m_connectInfo; }
    State       state() const { return m_state; }

private:
    void scheduleWrite();
    void flushPendingStreamWrites();
    int32_t sendStreamFrame(uint32_t streamId,
                            uint64_t streamOffset,
                            const uint8_t *data,
                            size_t len,
                            bool fin);
    int32_t sendHandshakeDonePacket();
    int32_t sendInitialPacket();
    int32_t sendHandshakePacket(bool encrypted);
    int32_t sendPacket(uint8_t packetType,
                       const void *payload,
                       size_t payloadLen,
                       uint16_t packetFlags = 0,
                       utp_packno_t *outPacketNo = nullptr);
    int32_t sendPacket(uint8_t packetType,
                       const void *payloadHead,
                       size_t payloadHeadLen,
                       const void *payloadBody,
                       size_t payloadBodyLen,
                       uint16_t packetFlags,
                       utp_packno_t *outPacketNo = nullptr);
    bool    canSendOnCurrentPath(size_t packetLen, FrameType frameType) const;
    void    maybeSendPathChallenge();
    void    handlePathChallengeFrame(const uint8_t *frameData, size_t frameSize);
    void    handlePathResponseFrame(const uint8_t *frameData, size_t frameSize);
    void    onPathValidationTimeout();
    void    onHandshakeDoneTimeout();
    void    armHandshakeDoneTimer();
    uint32_t handshakeDoneDelayMs() const;
    void    onConnTimeout();

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

    OnStreamCanCreate       m_onStreamCanCreate;
    OnStreamCreated         m_onStreamCreated;

    uint64_t                m_bytesIn{};
    uint64_t                m_bytesOut{};
    ev::EventTimer          m_pathValidationTimer;
    ev::EventTimer          m_handshakeDoneTimer;
    ReceiveHistory          m_receiveHistory;
    MtuDiscovery            m_mtuDiscovery;
    bool                    m_handshakeDonePending{false};
    bool                    m_handshakeDoneSent{false};
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_CONNECTION_H__
