/*************************************************************************
    > File Name: connection_impl.h
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:40:12 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_CONNECTION_H__
#define __UTP_CONTEXT_CONNECTION_H__

#include <event/timer.h>

#include "utp/connection.h"
#include "socket/udp.h"
#include "context/context_impl.h"

namespace eular {
namespace utp {

class ConnectionImpl : public Connection
{
public:
    using SP = std::shared_ptr<ConnectionImpl>;
    using WP = std::weak_ptr<ConnectionImpl>;

    enum State {
        // 没有 Handshake 状态是因为响应后即连接成功
        kStateDisconnected,     // 断连状态
        kStateWaitSendInitial,  // 等待发送初始包
        kStateWaitSend0RTT,     // 等待发送0-RTT包
        kStateInitialSent,      // 已发送初始包
        kStateConnected,        // 已连接
        kStateCloseSent,        // 已发送关闭包
        kStateCloseReceived,    // 收到关闭包
        kStatePtoTimedWait,     // 3 PTO超时等待
    };

    ConnectionImpl(ContextImpl *ctx, UdpSocket *udpSocket, uint32_t cid);
    ~ConnectionImpl() = default;

    int32_t connect(const Context::ConnectInfo &info);

    void onWrite();

public:
    uint64_t packetNumber() { return m_packetNumber++; }
    uint32_t cid() const { return m_cid; }
    const Context::ConnectInfo& connectInfo() const { return m_connectInfo; }

private:
    int32_t sendInitialPacket();
    // int32_t send0RTTPacket();
    void onConnTimeout();

private:
    ContextImpl*            m_ctx{};
    UdpSocket*              m_udpSocket{};
    State                   m_state{kStateDisconnected};

    Context::ConnectInfo    m_connectInfo{};
    ev::EventTimer          m_connTimer;

    uint32_t                m_cid;
    uint32_t                m_peerCid;
    uint64_t                m_packetNumber{1};
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_CONNECTION_H__
