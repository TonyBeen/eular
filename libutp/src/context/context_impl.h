/*************************************************************************
    > File Name: context_impl.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:29 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_IMPL_H__
#define __UTP_CONTEXT_IMPL_H__

#include <vector>
#include <memory>
#include <set>
#include <tuple>
#include <list>
#include <unordered_map>

#include <event/timer.h>
#include <event/poll.h>
#include <utils/utils.h>

#include "utp/utp.h"
#include "socket/udp.h"
#include "context/connection_impl.h"

#include "util/mm.h"

namespace eular {
namespace utp {

class X25519Wrapper;
class AesGcmContext;
class TokenAuth;
struct PacketIn;

inline bool operator<(const Context::ConnectInfo &lhs, const Context::ConnectInfo &rhs)
{
    return std::tie(lhs.ip, lhs.port) < std::tie(rhs.ip, rhs.port);
}

inline bool operator==(const Context::ConnectInfo &lhs, const Context::ConnectInfo &rhs)
{
    return lhs.ip == rhs.ip && lhs.port == rhs.port;
}

class ContextImpl
{
    DISALLOW_COPY_AND_ASSIGN(ContextImpl);
    DISALLOW_MOVE(ContextImpl);

public:
    ContextImpl(event_base *base, Config *config);
    ~ContextImpl();

    const std::string &tag() const;

    void setOnConnected(const Context::OnConnected &cb);
    void setOnConnectError(const Context::OnConnectError &cb);
    void setOnNewConnection(const Context::OnNewConnection &cb);
    void setOnConnectionClosed(const Context::OnConnectionClosed &cb);
    void wantWrite(ConnectionImpl *conn);

    event_base*     loop() const { return m_base; }
    Config*         config() { return &m_config; }

public:
    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname);
    int32_t connect(const Context::ConnectInfo &info);
    int32_t accept();

private:
    struct PendingIncomingConnection {
        struct EarlyStreamFrame {
            uint32_t            streamId{0};
            uint64_t            streamOffset{0};
            bool                fin{false};
            std::vector<uint8_t> data;
        };

        uint32_t                localCid{0};
        uint32_t                peerCid{0};
        Address                 peerAddress;
        std::string             peerIp;
        bool                    encrypted{false};
        bool                    zeroRttOffered{false};
        bool                    zeroRttAccepted{false};
        uint32_t                zeroRttTokenCid{0};
        uint64_t                packetNumber{1};
        utp_time_t              acceptStartUs{0};
        bool                    handshakeSent{false};
        TransportParams         peerTp{};
        std::vector<EarlyStreamFrame> earlyStreamFrames;
        std::shared_ptr<X25519Wrapper> x25519;
        std::shared_ptr<AesGcmContext> aesCtx;
    };

private:
    static std::string peerKey(const Address &peerAddress, uint32_t peerCid);
    void handleConnectionState(ConnectionImpl *conn);
    int32_t sendPendingHandshake(PendingIncomingConnection &pending);
    int32_t sendPendingConnectionClose(PendingIncomingConnection &pending, uint16_t errorCode, const std::string &reason);
    int32_t sendPendingPacket(PendingIncomingConnection &pending,
                              uint8_t packetType,
                              const void *payload,
                              size_t payloadLen);
    bool decodeIncomingPendingPacket(const UdpSocket::MsgMetaInfo &msg,
                                     PendingIncomingConnection &pending,
                                     PacketIn &packet) const;
    bool allocLocalCid(uint32_t &cid);
    void removePendingIncoming(uint32_t localCid);
    void onPendingHandshakeTimeout();
    void processPendingHandshakeTimeouts();
    TokenAuth *tokenAuth();
    bool validateZeroRttTicket(const Address &peerAddress,
                               const std::vector<uint8_t> &ticket,
                               uint16_t validityPeriod,
                               uint32_t &ticketCid);
    void purgeZeroRttReplayCache(uint64_t nowMs);
    bool rememberZeroRttNonce(uint32_t ticketCid, uint64_t nonce, uint64_t nowMs = 0);

private:
    void onReadEvent();
    void onWriteEvent();

private:
    std::string     m_tag;
    event_base*     m_base;
    Config          m_config;
    UdpSocket       m_udpSocket;

    ev::EventPoll   m_readEvent;
    ev::EventPoll   m_writeEvent;
    ev::EventTimer  m_pendingHandshakeTimer;

    Context::OnConnected        m_onConnected;
    Context::OnConnectError     m_onConnectError;
    Context::OnNewConnection    m_onNewConnection;
    Context::OnConnectionClosed m_onConnectionClosed;

    using ConnectionMap = std::unordered_map<uint32_t, ConnectionImpl::SP>; // cid -> ConnectionImpl
    ConnectionMap                   m_connections;          // 所有连接容器
    std::set<ConnectionImpl *>      m_pendingConnections;   // 正在连接队列
    std::list<ConnectionImpl *>     m_wantWriteConns;       // 需要写数据的连接队列

    std::unordered_map<uint32_t, PendingIncomingConnection> m_pendingIncoming; // local cid -> pending incoming
    std::unordered_map<std::string, uint32_t> m_pendingIncomingPeerIndex;       // peer address+scid -> local cid
    std::list<uint32_t>             m_pendingIncomingQueue; // 回调通知后的待 accept 队列
    std::set<uint32_t>              m_waitHandshakeDone;    // 已 accept，等待 HandshakeDone
    std::unique_ptr<TokenAuth>      m_tokenAuth;

    struct ZeroRttReplayKey {
        uint32_t ticketCid{0};
        uint64_t nonce{0};

        bool operator==(const ZeroRttReplayKey &other) const {
            return ticketCid == other.ticketCid && nonce == other.nonce;
        }
    };

    struct ZeroRttReplayKeyHash {
        size_t operator()(const ZeroRttReplayKey &key) const {
            return static_cast<size_t>(key.ticketCid)
                ^ static_cast<size_t>(key.nonce)
                ^ static_cast<size_t>(key.nonce >> 32);
        }
    };

    std::unordered_map<ZeroRttReplayKey, uint64_t, ZeroRttReplayKeyHash> m_zeroRttReplayCache;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_IMPL_H__
