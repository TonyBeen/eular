/*************************************************************************
    > File Name: context_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:32 PM CST
 ************************************************************************/

#include "context/context_impl.h"

#include <atomic>

#include "utp/errno.h"

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

    m_udpSocket.updateTag(tag());
    return UTP_ERR_NO_ERROR;
}

int32_t ContextImpl::connect(const Context::ConnectInfo &info)
{
    if (!m_udpSocket.isValid()) {
        return UTP_ERR_NOT_BOUND;
    }

    return UTP_ERR_NO_ERROR;
}

Connection::Ptr ContextImpl::accept()
{
    return Connection::Ptr();
}

} // namespace utp
} // namespace eular
