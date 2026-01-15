/*************************************************************************
    > File Name: utp.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 02:57:22 PM CST
 ************************************************************************/

#include "utp/utp.h"

#include "version.h"
#include "context/context_impl.h"

namespace eular {
namespace utp {
Context::Context(event_base *base)
{
    m_impl = std::make_shared<ContextImpl>(base);
}

Context::~Context()
{
}

const char *Context::Version()
{
    return UTP_VERSION_STRING;
}

void Context::setOnConnected(const OnConnected &cb)
{
    m_impl->setOnConnected(cb);
}

void Context::setOnConnectError(const OnConnectError &cb)
{
    m_impl->setOnConnectError(cb);
}

void Context::setOnNewConnection(const OnNewConnection &cb)
{
    m_impl->setOnNewConnection(cb);
}

void Context::setOnConnectionClosed(const OnConnectionClosed &cb)
{
    m_impl->setOnConnectionClosed(cb);
}

int32_t Context::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    return m_impl->bind(ip, port, ifname);
}

int32_t Context::connect(const ConnectInfo &info)
{
    return m_impl->connect(info);
}

Connection::Ptr Context::accept()
{
    return m_impl->accept();
}

} // namespace utp
} // namespace eular
