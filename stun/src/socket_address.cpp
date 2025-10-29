/*************************************************************************
    > File Name: socket_address.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月03日 星期二 17时27分11秒
 ************************************************************************/

#include "socket_address.h"

#include <string.h>
#include <assert.h>
#include <stdexcept>
#include <exception>

#include <utils/endian.hpp>

#define IP_LENGTH 64

namespace eular {
namespace stun {
SocketAddress::SocketAddress()
{
    memset(&this->m_address, 0, sizeof(socket_address_t));
}

SocketAddress::SocketAddress(const sockaddr *addr)
{
    memset(&this->m_address, 0, sizeof(socket_address_t));
    assert(addr->sa_family == AF_INET || addr->sa_family == AF_INET6);
    if (addr->sa_family == AF_INET) {
        this->m_address.addr4 = *(sockaddr_in *)addr;
    } else if (addr->sa_family == AF_INET6) {
        this->m_address.addr6 = *(sockaddr_in6 *)addr;
    } else {
        this->m_address.addr = *addr;
    }
}

SocketAddress::SocketAddress(const std::string &ip, uint16_t port)
{
    memset(&this->m_address, 0, sizeof(socket_address_t));

    if (inet_pton(AF_INET, ip.c_str(), &this->m_address.addr4.sin_addr) <= 0) {
        if (inet_pton(AF_INET6, ip.c_str(), &this->m_address.addr6.sin6_addr) <= 0) {
            throw std::invalid_argument("Invalid IP address format");
        } else {
            this->m_address.addr6.sin6_family = AF_INET6;
            this->m_address.addr6.sin6_port = htobe16(port);
        }
    } else {
        this->m_address.addr4.sin_family = AF_INET;
        this->m_address.addr4.sin_port = htobe16(port);
    }
}

SocketAddress::SocketAddress(const SocketAddress &other)
{
    if (this != &other) {
        memcpy(&this->m_address, &other.m_address, sizeof(socket_address_t));
    }
}

SocketAddress::~SocketAddress()
{
}

SocketAddress &SocketAddress::operator=(const SocketAddress &other)
{
    if (this != &other) {
        memcpy(&this->m_address, &other.m_address, sizeof(socket_address_t));
    }

    return *this;
}

bool SocketAddress::operator==(const SocketAddress &other) const
{
    if (this->m_address.addr.sa_family != other.m_address.addr.sa_family) {
        return false;
    }

    if (m_address.addr.sa_family != AF_INET && m_address.addr.sa_family != AF_INET6) {
        return false; // Unsupported address family
    }

    return getIp() == other.getIp() && getPort() == other.getPort();
}

uint16_t SocketAddress::getPort() const
{
    return be16toh(getNetEndianPort());
}

uint16_t SocketAddress::getNetEndianPort() const
{
    if (this->m_address.addr.sa_family == AF_INET) {
        return this->m_address.addr4.sin_port;
    } else if (this->m_address.addr.sa_family == AF_INET6) {
        return this->m_address.addr6.sin6_port;
    } else {
        return 0;
    }
}

void SocketAddress::setPort(uint16_t port)
{
    if (m_address.addr.sa_family == AF_INET) {
        m_address.addr4.sin_port = htobe16(port);
    } else if (m_address.addr.sa_family == AF_INET6) {
        m_address.addr6.sin6_port = htobe16(port);
    }
}

std::string SocketAddress::getIp() const
{
    char ip[IP_LENGTH] = {0};
    if (m_address.addr.sa_family == AF_INET) {
        inet_ntop(AF_INET, &m_address.addr4.sin_addr, ip, IP_LENGTH);
    } else if (m_address.addr.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &m_address.addr6.sin6_addr, ip, IP_LENGTH);
    }

    return std::string(ip);
}

uint16_t SocketAddress::getIPLength() const
{
    uint16_t length = 0;
    if (m_address.addr.sa_family == AF_INET) {
        length = sizeof(m_address.addr4.sin_addr);
    } else if (m_address.addr.sa_family == AF_INET6) {
        length = sizeof(m_address.addr6.sin6_addr);
    }

    return length;
}

uint16_t SocketAddress::getNetEndianIp(void *pAddr, uint16_t length) const
{
    if (pAddr == NULL || length == 0 || length < getIPLength()) {
        return 0;
    }

    uint16_t bytescopied = 0;
    if (m_address.addr.sa_family == AF_INET) {
        memcpy(pAddr, &m_address.addr4.sin_addr, sizeof(m_address.addr4.sin_addr));
        bytescopied = sizeof(m_address.addr4.sin_addr);
    } else if (m_address.addr.sa_family == AF_INET6) {
        memcpy(pAddr, &m_address.addr6.sin6_addr, sizeof(m_address.addr6.sin6_addr));
        bytescopied = sizeof(m_address.addr6.sin6_addr);
    }

    return bytescopied;
}

void SocketAddress::setIp(const std::string &ip)
{
    if (inet_pton(AF_INET, ip.c_str(), &this->m_address.addr4.sin_addr)) {
        return;
    }

    inet_pton(AF_INET6, ip.c_str(), &this->m_address.addr6.sin6_addr);
}

uint16_t SocketAddress::family() const
{
    return m_address.addr.sa_family;
}

const sockaddr *SocketAddress::getSockAddr() const
{
    return &m_address.addr;
}

socklen_t SocketAddress::getSockAddrLength() const
{
    if (m_address.addr.sa_family == AF_INET) {
        return sizeof(m_address.addr4);
    } else if (m_address.addr.sa_family == AF_INET6) {
        return sizeof(m_address.addr6);
    } else {
        return sizeof(m_address.addr);
    }
}

std::string SocketAddress::toString() const
{
    char ip[IP_LENGTH] = {0};
    char formatted[IP_LENGTH] = {0};
    if (m_address.addr.sa_family == AF_INET) {
        inet_ntop(AF_INET, &m_address.addr4.sin_addr, ip, IP_LENGTH);
        snprintf(formatted, IP_LENGTH, "%s:%u", ip, getPort());
    } else if (m_address.addr.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &m_address.addr6.sin6_addr, ip, IP_LENGTH);
        snprintf(formatted, IP_LENGTH, "[%s]:%u", ip, getPort());
    }

    return std::string(formatted);
}

} // namespace NS_STUN
} // namespace eular
