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

#include <utils/utils.h>

#include "utp/utp.h"
#include "socket/udp.h"

namespace eular {
namespace utp {

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
    ContextImpl(event_base *base);
    ~ContextImpl();

    const std::string &tag() const;

    void setOnConnected(const Context::OnConnected &cb);
    void setOnConnectError(const Context::OnConnectError &cb);
    void setOnNewConnection(const Context::OnNewConnection &cb);
    void setOnConnectionClosed(const Context::OnConnectionClosed &cb);

public:
    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname);
    int32_t connect(const Context::ConnectInfo &info);
    Connection::Ptr accept();

private:
    std::string     m_tag;
    event_base*     m_base;
    UdpSocket       m_udpSocket;

    Context::OnConnected        m_onConnected;
    Context::OnConnectError     m_onConnectError;
    Context::OnNewConnection    m_onNewConnection;
    Context::OnConnectionClosed m_onConnectionClosed;

    std::set<Context::ConnectInfo>  m_pendingConnections; // 正在连接队列
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_IMPL_H__
