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
#include <utp/connection.h>

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
    struct ConnectInfo {
        std::string ip;
        uint16_t    port{0};
        uint32_t    timeout{3000}; // ms
        int8_t      retries{0};
        bool        encrypted{false};
    };

    using OnConnected = std::function<void(Connection::Ptr)>;
    using OnConnectError = std::function<void(int32_t, const std::string &, ConnectInfo)>;
    using OnConnectionClosed = std::function<void(Connection::Ptr)>;
    using OnNewConnection = std::function<void(Connection::Ptr)>;

    Context(event_base *base);
    ~Context();

public:
    static const char *Version();

    // 主动调用connect接口时回调
    void setOnConnected(const OnConnected &cb);
    // 主动调用connect接口失败时回调
    void setOnConnectError(const OnConnectError &cb);
    // 被动接受连接时回调
    void setOnNewConnection(const OnNewConnection &cb);
    // 连接关闭时回调
    void setOnConnectionClosed(const OnConnectionClosed &cb);

    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname = "");
    int32_t connect(const ConnectInfo &info);
    Connection::Ptr accept();

private:
    std::shared_ptr<ContextImpl>    m_impl{};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTP_H__
