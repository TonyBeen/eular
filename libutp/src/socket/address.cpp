/*************************************************************************
    > File Name: address.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 03:00:16 PM CST
 ************************************************************************/

#include "socket/address.h"
#include "address.h"

namespace eular {
namespace utp {
Address::Address() noexcept
{
    reset();
}

Address::Address(const char *ip, uint16_t port)
{
    if (!parse(ip, port)) {
        reset();
    }
}

Address::Address(const sockaddr *addr, socklen_t len)
{
    if (!fromSockAddr(addr, len)) {
        reset();
    }
}

Address::Address(const sockaddr_in &addr)
{
    fromSockAddrIn(addr);
}

Address::Address(const sockaddr_in6 &addr)
{
    fromSockAddrIn6(addr);
}

Address Address::AnyIPv4(uint16_t port)
{
    Address addr;
    addr.m_family = Family::IPv4;
    addr.m_port = port;
    memset(addr.m_addr.v4, 0, kIPv4AddrLen);
    return addr;
}

Address Address::AnyIPv6(uint16_t port)
{
    Address addr;
    addr.m_family = Family::IPv6;
    addr.m_port = port;
    memset(addr.m_addr.v6, 0, kIPv6AddrLen);
    return addr;
}

Address Address::LoopbackIPv4(uint16_t port)
{
    return Address("127.0.0.1", port);
}

Address Address::LoopbackIPv6(uint16_t port)
{
    return Address("::1", port);
}

eular::optional<Address> Address::Parse(const std::string &addrStr)
{
    Address addr;
    if (addr.parseHostPort(addrStr)) {
        return addr;
    }

    return eular::nullopt;
}

void Address::reset()
{
    m_family = Family::NONE;
    m_port = 0;
    memset(&m_addr, 0, sizeof(m_addr));
}

bool Address::parse(const char *ip, uint16_t port)
{
    if (ip == nullptr) {
        return false;
    }

    // 尝试解析为 IPv4
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) == 1) {
        m_family = Family::IPv4;
        m_port = port;
        memcpy(m_addr.v4, &addr4, kIPv4AddrLen);
        return true;
    }

    // 尝试解析为 IPv6
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) == 1) {
        m_family = Family::IPv6;
        m_port = port;
        memcpy(m_addr.v6, &addr6, kIPv6AddrLen);
        return true;
    }

    return false;
}

bool Address::fromSockAddr(const sockaddr *addr, socklen_t len)
{
    if (addr == nullptr) {
        return false;
    }

    if (addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        fromSockAddrIn(*reinterpret_cast<const struct sockaddr_in*>(addr));
        return true;
    }

    if (addr->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        fromSockAddrIn6(*reinterpret_cast<const struct sockaddr_in6*>(addr));
        return true;
    }

    return false;
}

void Address::fromSockAddrIn(const sockaddr_in &addr)
{
    m_family = Family::IPv4;
    m_port = ntohs(addr.sin_port);
    memcpy(m_addr.v4, &addr.sin_addr, kIPv4AddrLen);
}

void Address::fromSockAddrIn6(const sockaddr_in6 &addr)
{
    m_family = Family::IPv6;
    m_port = ntohs(addr.sin6_port);
    memcpy(m_addr.v6, &addr.sin6_addr, kIPv6AddrLen);
}

bool Address::parseHostPort(const std::string &addrStr)
{
    if (addrStr.empty()) {
        return false;
    }

    std::string host;
    uint16_t port = 0;

    if (addrStr[0] == '[') {
        // IPv6 格式: [::1]:8080
        size_t closeBracket = addrStr.find(']');
        if (closeBracket == std::string::npos) {
            return false;
        }
        host = addrStr.substr(1, closeBracket - 1);

        if (closeBracket + 1 < addrStr.size()) {
            if (addrStr[closeBracket + 1] != ':') {
                return false;
            }

            port = static_cast<uint16_t>(std::stoi(addrStr.substr(closeBracket + 2)));
        }
    } else {
        // IPv4 格式: 127.0.0.1:8080 或纯 IP
        size_t colonPos = addrStr.rfind(':');

        // 检查是否为 IPv6（多个冒号）
        size_t firstColon = addrStr.find(':');
        if (firstColon != colonPos) {
            // 纯 IPv6 地址，无端口
            host = addrStr;
            port = 0;
        } else if (colonPos != std::string::npos) {
            host = addrStr.substr(0, colonPos);
            port = static_cast<uint16_t>(std::stoi(addrStr.substr(colonPos + 1)));
        } else {
            host = addrStr;
            port = 0;
        }
    }

    return parse(host.c_str(), port);
}

bool Address::isLoopback() const noexcept
{
    if (m_family == Family::IPv4) {
        // 127.0.0.0/8
        return m_addr.v4[0] == 127;
    } else if (m_family == Family::IPv6) {
        // ::1
        static const uint8_t loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        return memcmp(m_addr.v6, loopback, kIPv6AddrLen) == 0;
    }

    return false;
}

bool Address::isAny() const noexcept
{
    if (m_family == Family::IPv4) {
        return m_addr.v4[0] == 0 && m_addr.v4[1] == 0 && m_addr.v4[2] == 0 && m_addr.v4[3] == 0;
    } else if (m_family == Family:: IPv6) {
        static const uint8_t any[16] = {0};
        return memcmp(m_addr.v6, any, kIPv6AddrLen) == 0;
    }

    return false;
}

bool Address::isMulticast() const noexcept
{
    if (m_family == Family:: IPv4) {
        // 224.0.0.0 - 239.255.255.255
        return (m_addr.v4[0] & 0xF0) == 0xE0;
    } else if (m_family == Family::IPv6) {
        // ff00::/8
        return m_addr.v6[0] == 0xFF;
    }

    return false;
}

bool Address::isPrivate() const noexcept
{
    if (m_family == Family::IPv4) {
        // 10.0.0.0/8
        if (m_addr.v4[0] == 10) return true;
        // 172.16.0.0/12
        if (m_addr.v4[0] == 172 && (m_addr.v4[1] & 0xF0) == 16) return true;
        // 192.168.0.0/16
        if (m_addr.v4[0] == 192 && m_addr.v4[1] == 168) return true;
    } else if (m_family == Family::IPv6) {
        // fc00::/7 (Unique Local Address)
        return (m_addr.v6[0] & 0xFE) == 0xFC;
    }

    return false;
}

bool Address::isLinkLocal() const noexcept
{
    if (m_family == Family::IPv4) {
        // 169.254.0.0/16
        return m_addr.v4[0] == 169 && m_addr.v4[1] == 254;
    } else if (m_family == Family:: IPv6) {
        // fe80::/10
        return m_addr.v6[0] == 0xFE && (m_addr.v6[1] & 0xC0) == 0x80;
    }

    return false;
}

bool Address::operator==(const Address &other) const noexcept
{
    if (m_family != other.m_family || m_port != other.m_port) {
        return false;
    }

    if (m_family == Family::IPv4) {
        return memcmp(m_addr.v4, other.m_addr.v4, kIPv4AddrLen) == 0;
    } else if (m_family == Family::IPv6) {
        return memcmp(m_addr.v6, other.m_addr.v6, kIPv6AddrLen) == 0;
    }
    return true;  // Both None
}

bool Address::operator!=(const Address &other) const noexcept
{
    return !(*this == other);
}

bool Address::operator<(const Address &other) const noexcept
{
    if (m_family != other.m_family) {
        return m_family < other.m_family;
    }

    int cmp = 0;
    if (m_family == Family::IPv4) {
        cmp = memcmp(m_addr.v4, other.m_addr.v4, kIPv4AddrLen);
    } else if (m_family == Family::IPv6) {
        cmp = memcmp(m_addr.v6, other.m_addr.v6, kIPv6AddrLen);
    }

    if (cmp != 0) return cmp < 0;
    return m_port < other.m_port;
}

size_t Address::hash() const noexcept
{
    size_t h = static_cast<size_t>(m_family) ^ (static_cast<size_t>(m_port) << 16);
    if (m_family == Family::IPv4) {
        uint32_t v4;
        memcpy(&v4, m_addr.v4, sizeof(v4));
        h ^= v4;
    } else if (m_family == Family:: IPv6) {
        for (size_t i = 0; i < kIPv6AddrLen; i += sizeof(size_t)) {
            size_t chunk = 0;
            memcpy(&chunk, m_addr.v6 + i, std:: min(sizeof(size_t), kIPv6AddrLen - i));
            h ^= chunk;
        }
    }

    return h;
}

} // namespace utp
} // namespace eular
