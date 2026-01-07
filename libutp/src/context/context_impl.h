/*************************************************************************
    > File Name: context_impl.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:01:29 PM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_IMPL_H__
#define __UTP_CONTEXT_IMPL_H__

#include "utp/utp.h"

namespace eular {
namespace utp {
class ContextImpl
{
public:
    ContextImpl(event_base *base);
    ~ContextImpl();

public:
    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname);
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_IMPL_H__
