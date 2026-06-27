/**
 * @file socket_address.h
 * @brief NTRS 通用套接字地址封装。
 */

#ifndef NTRS_CORE_SOCKET_ADDRESS_H
#define NTRS_CORE_SOCKET_ADDRESS_H

#include <string>

#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;  // 某些老版本 Windows 需要手动定义
#else
#include <netdb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace eular {
namespace ntrs {
/**
 * @brief 通用 IP/端口地址封装。
 *
 * 该类型仅承担地址读写、字符串转换和基础比较职责，不包含任何 NAT 探测或
 * 打洞协议语义，可作为私有探测、控制面和传输层的公共工具类型。
 */
class SocketAddress
{
public:
    SocketAddress();
    SocketAddress(const sockaddr* addr);
    SocketAddress(const std::string& ip, uint16_t port);
    SocketAddress(const SocketAddress& other);
    ~SocketAddress();

    SocketAddress& operator=(const SocketAddress& other);
    bool           operator==(const SocketAddress& other) const;

    uint16_t getPort() const;
    uint16_t getNetEndianPort() const;
    void     setPort(uint16_t port);

    std::string getIp() const;
    uint16_t    getIPLength() const;
    uint16_t    getNetEndianIp(void* pAddr, uint16_t length) const;
    void        setIp(const std::string& ip);

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
    std::string toString() const;

private:
    typedef union socket_address {
        struct sockaddr         addr;
        struct sockaddr_in      addr4;
        struct sockaddr_in6     addr6;
        struct sockaddr_storage addr_storage;
    } socket_address_t;

    socket_address_t m_address;
};

}  // namespace ntrs
}  // namespace eular
#endif  // NTRS_CORE_SOCKET_ADDRESS_H
