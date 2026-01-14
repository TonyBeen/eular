/*************************************************************************
    > File Name: context_impl.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:29 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_IMPL_H__
#define __UTP_CONTEXT_IMPL_H__

#include "utp/utp.h"

#include <utils/utils.h>

#include <socket/udp.h>

namespace eular {
namespace utp {
class ContextImpl
{
    DISALLOW_COPY_AND_ASSIGN(ContextImpl);
    DISALLOW_MOVE(ContextImpl);

public:
    ContextImpl(event_base *base);
    ~ContextImpl();

    const std::string &tag() const;

public:
    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname);

private:
    std::string     m_tag;
    event_base*     m_base;
    UdpSocket       m_udpSocket;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_IMPL_H__
