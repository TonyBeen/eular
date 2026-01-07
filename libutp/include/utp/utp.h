/*************************************************************************
    > File Name: utp.h
    > Author: eular
    > Brief:
    > Created Time: Tue 23 Dec 2025 05:14:55 PM CST
 ************************************************************************/

#ifndef __UTP_UTP_H__
#define __UTP_UTP_H__

#include <string>
#include <memory>
#include <functional>

#include <utp/platform.h>

struct event;
struct event_base;

namespace eular {
namespace utp {
class ContextImpl;
class UTP_API Context {
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) = delete;
    Context &operator=(Context &&) = delete;

public:
    Context(event_base *base);
    ~Context();

public:
    static const char *Version();

    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname = "");

private:
    std::shared_ptr<ContextImpl> _impl{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTP_H__
