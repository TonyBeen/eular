/*************************************************************************
    > File Name: address.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:00:14 PM CST
 ************************************************************************/

#ifndef __UTP_SOCKET_ADDRESS_H__
#define __UTP_SOCKET_ADDRESS_H__

#include <stdint.h>
#include <string.h>

#include <string>
#include <array>

#include <utils/optional.hpp>

#include "utp/platform.h"

#if defined(OS_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#elif defined(OS_LINUX) || defined(OS_APPLE)
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netdb.h>
#endif

namespace eular {
namespace utp {

class Address
{
public:
    enum Family {
        NONE = AF_UNSPEC,
        IPv4 = AF_INET,
        IPv6 = AF_INET6,
    };

    static constexpr size_t kIPv4AddrLen = 4;
    static constexpr size_t kIPv6AddrLen = 16;
    static constexpr size_t kMaxStrLen   = 64;

    Address() noexcept;
    Address(const char *ip, uint16_t port);
    Address(const std::string &ip, uint16_t port) : Address(ip.c_str(), port) {}
    explicit Address(const sockaddr* addr, socklen_t len);
    explicit Address(const sockaddr_in &addr);
    explicit Address(const sockaddr_in6 &addr);
    Address(const Address &other) = default;
    ~Address() = default;

public:
    static Address AnyIPv4(uint16_t port = 0);
    static Address AnyIPv6(uint16_t port = 0);
    static Address LoopbackIPv4(uint16_t port = 0);
    static Address LoopbackIPv6(uint16_t port = 0);

    // 从字符串解析 "ip:port" 或 "[ipv6]:port"
    static eular::optional<Address> Parse(const std::string& addrStr);

public:
    void reset();
    bool parse(const char* ip, uint16_t port);
    bool fromSockAddr(const struct sockaddr* addr, socklen_t len);
    void fromSockAddrIn(const struct sockaddr_in& addr);
    void fromSockAddrIn6(const struct sockaddr_in6& addr);
    bool fromSocket(int32_t sockfd);
    bool parseHostPort(const std::string& addrStr);

public:
    bool toSockAddrIn(sockaddr_in& addr) const;
    bool toSockAddrIn6(struct sockaddr_in6& addr) const;
    socklen_t toSockAddr(struct sockaddr_storage& storage) const;
    std::string toString() const;

public:
    Family family() const { return m_family; }
    uint16_t port() const { return m_port; }

    bool isValid() const { return m_family != NONE; }
    bool isIPv4() const { return m_family == IPv4; }
    bool isIPv6() const { return m_family == IPv6; }

    bool isLoopback() const noexcept;
    bool isAny() const noexcept;
    bool isMulticast() const noexcept;
    bool isPrivate() const noexcept;
    bool isLinkLocal() const noexcept;

public:
    bool operator==(const Address& other) const noexcept;
    bool operator!=(const Address& other) const noexcept;
    bool operator<(const Address& other) const noexcept;

    size_t hash() const noexcept;

private:
    Family      m_family;
    uint16_t    m_port;

    union {
        uint8_t v4[kIPv4AddrLen];
        uint8_t v6[kIPv6AddrLen];
    } m_addr;
};

} // namespace utp
} // namespace eular

// std::hash 支持
namespace std {
template <>
struct hash<eular::utp::Address> {
    size_t operator()(const eular::utp::Address& addr) const noexcept {
        return addr.hash();
    }
};
} // namespace std

#endif // __UTP_SOCKET_ADDRESS_H__
