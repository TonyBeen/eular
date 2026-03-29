/*************************************************************************
    > File Name: utp.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 02:57:22 PM CST
 ************************************************************************/

#include "utp/utp.h"

#include "version.h"
#include "context/context_impl.h"
#include "util/error.h"

namespace {
int32_t NormalizePublicStatus(int32_t status)
{
    if (status == UTP_ERR_OK) {
        return 0;
    }

    if (status == -1) {
        return -1;
    }

    if (status < 0) {
        const utp_error_t legacyErr = static_cast<utp_error_t>(-status);
        SetLastErrorV(legacyErr, "legacy negative status: {}", status);
        return -1;
    }

    SetLastErrorV(static_cast<utp_error_t>(status), "legacy positive status: {}", status);
    return -1;
}
} // namespace

namespace eular {
namespace utp {
Context::Context(event_base *base, Config *config)
{
    m_impl = std::make_shared<ContextImpl>(base, config);
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

void Context::setOnZeroRttDecision(const OnZeroRttDecision &cb)
{
    m_impl->setOnZeroRttDecision(cb);
}

void Context::setResumptionSecret(const std::vector<uint8_t> &secret)
{
    m_impl->setResumptionSecret(secret);
}

void Context::clearResumptionSecret()
{
    m_impl->clearResumptionSecret();
}

int32_t Context::connect0RttWithState(const Connect0RttWithStateInfo &info, const std::string &state)
{
    return NormalizePublicStatus(m_impl->connect0RttWithState(info, state));
}

Context::Statistic Context::statistic() const
{
    return m_impl->statistic();
}

int32_t Context::bind(const std::string &ip, uint16_t port, const std::string &ifname)
{
    return NormalizePublicStatus(m_impl->bind(ip, port, ifname));
}

int32_t Context::connect(const ConnectInfo &info)
{
    return NormalizePublicStatus(m_impl->connect(info));
}

int32_t Context::connect0Rtt(const Connect0RttInfo &info)
{
    return NormalizePublicStatus(m_impl->connect0Rtt(info));
}

int32_t Context::accept()
{
    return NormalizePublicStatus(m_impl->accept());
}

} // namespace utp
} // namespace eular
