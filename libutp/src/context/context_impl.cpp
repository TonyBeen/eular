/*************************************************************************
    > File Name: context_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:32 PM CST
 ************************************************************************/

#include "context/context_impl.h"

#include <atomic>

#include "utp/errno.h"
#include "context/connection_impl.h"
#include "util/error.h"
#include "util/random.hpp"

static std::atomic<uint32_t> g_contextId{0};

namespace eular {
namespace utp {
ContextImpl::ContextImpl(event_base *base) :
    m_base(base)
{
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

void ContextImpl::setOnNewConnection(const Context::OnNewConnection &cb)
{
    m_onNewConnection = cb;
}

void ContextImpl::setOnConnectionClosed(const Context::OnConnectionClosed &cb)
{
    m_onConnectionClosed = cb;
}

int32_t ContextImpl::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    int32_t status = m_udpSocket.bind(ip, port, ifname);
    if (status != UTP_ERR_NO_ERROR) {
        return status;
    }

    m_readEvent.reset(m_base, m_udpSocket.fd(), ev::EventPoll::Read | ev::EventPoll::EdgeTrigger, [this] (socket_t, ev::EventPoll::event_t) {
        this->onReadEvent();
    });
    m_udpSocket.updateTag(tag());
    return UTP_ERR_NO_ERROR;
}

int32_t ContextImpl::connect(const Context::ConnectInfo &info)
{
    if (info.ip.empty() || info.port == 0) {
        SetLastErrorV(UTP_ERR_INVALID_PARAM, "{} invalid connect info: {}:{}", tag(), info.ip, info.port);
        return -1;
    }

    if (!m_udpSocket.isValid()) {
        SetLastErrorV(UTP_ERR_NOT_BOUND, "{} UDP socket is not bound", tag());
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

            SetLastErrorV(UTP_ERR_ALREADY_CONNECTED, "{} already connected to {}:{}", tag(), info.ip, info.port);
            return -1;
        }
    }

    uint32_t cid = 0;
    for (int32_t i = 0; i < 3; ++i) {
        cid = Random<uint32_t>(1, UINT32_MAX);
        if (m_connections.find(cid) == m_connections.end()) {
            break;
        }
    }

    ConnectionImpl::SP conn = std::make_shared<ConnectionImpl>(this, m_udpSocket, cid);
    int32_t status = conn->connect(info);
    if (status != UTP_ERR_NO_ERROR) {
        return status;
    }

    m_pendingConnections.insert(conn.get());
    m_connections[cid] = std::move(conn);
    return status;
}

Connection::Ptr ContextImpl::accept()
{
    return Connection::Ptr();
}

} // namespace utp
} // namespace eular
