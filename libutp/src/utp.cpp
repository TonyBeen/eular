/*************************************************************************
    > File Name: utp.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 02:57:22 PM CST
 ************************************************************************/

#include "utp/utp.h"
#include "utp.h"

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
    return nullptr;
}

int32_t Context::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    return m_impl->bind(ip, port, ifname);
}

} // namespace utp
} // namespace eular
