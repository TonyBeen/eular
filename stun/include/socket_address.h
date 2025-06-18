/*************************************************************************
    > File Name: socket_address.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月03日 星期二 17时27分07秒
 ************************************************************************/

#ifndef STUN_CORE_SOCKET_ADDRESS_H
#define STUN_CORE_SOCKET_ADDRESS_H

#include <string>

#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t; // 某些老版本 Windows 需要手动定义
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace eular {
namespace stun {
class SocketAddress
{
public:
    SocketAddress();
    SocketAddress(const sockaddr *addr);
    SocketAddress(const std::string &ip, uint16_t port);
    SocketAddress(const SocketAddress &other);
    ~SocketAddress();

    SocketAddress &operator=(const SocketAddress &other);
    bool operator==(const SocketAddress &other) const;

    uint16_t        getPort() const;
    uint16_t        getNetEndianPort() const;
    void            setPort(uint16_t port);

    std::string     getIp() const;
    uint16_t        getIPLength() const;
    uint16_t        getNetEndianIp(void* pAddr, uint16_t length) const;
    void            setIp(const std::string &ip);

    uint16_t        family() const;
    const sockaddr* getSockAddr() const;
    socklen_t       getSockAddrLength() const;

    /**
     * @brief This function converts the IP address to a string representation.
     * @example IPv4: 127.0.0.1:8080
     * @example IPv6: [::1]:8080
     *
     * @return std::string
     */
    std::string     toString() const;

private:
    typedef union socket_address
    {
        struct sockaddr addr;
        struct sockaddr_in addr4;
        struct sockaddr_in6 addr6;
        struct sockaddr_storage addr_storage;
    } socket_address_t;

    socket_address_t m_address;
};

} // namespace stun
} // namespace eular
#endif // STUN_CORE_SOCKET_ADDRESS_H
