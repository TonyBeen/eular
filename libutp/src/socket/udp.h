/*************************************************************************
    > File Name: udp.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 02:59:57 PM CST
 ************************************************************************/

#ifndef __UTP_SOCKET_UDP_H__
#define __UTP_SOCKET_UDP_H__

#include <vector>

#include <utils/buffer.h>

#include "commom.h"
#include "socket/mmsg.h"
#include "socket/packet.h"

namespace eular {
namespace utp {
class UdpSocket
{
public:
    struct MsgMetaInfo {
        void*           data;
        size_t          len;
        PacketMetaInfo  metaInfo;
    };

    struct ErrorMsg {
        std::vector<int32_t>    ee_type;
        std::vector<int32_t>    ee_code;
        std::vector<uint32_t>   ee_info;

        void*       data;
        size_t      len;
        Address     peer_addr;
    };

    UdpSocket();
    ~UdpSocket();

public:
    void updateTag(const std::string& tag);
    const std::string& tag() const { return m_tag; }
    socket_t fd() const { return m_sock; }

public:
    bool isValid() const { return m_sock != INVALID_SOCKET; }

    int32_t bind(const std::string &ip, uint16_t port, const std::string &ifname);

    int32_t recvErrorMsg(ErrorMsg &errMsg);

    /**
     * @brief 读取数据
     *
     * @param msgVec 数据缓存
     * @return int32_t 返回读取到的数据包数量, 小于0表示失败, 等于0表示无数据
     */
    int32_t recv(std::vector<MsgMetaInfo>& msgVec);

    int32_t send(const std::vector<MsgMetaInfo> &msgVec);

private:
    socket_t    m_sock{INVALID_SOCKET};
    Address     m_bindAddr;
    Address     m_localAddr;

#if defined(USE_SENDMMSG)
    MultipleMsg     m_mmsg;
#endif
    ByteBuffer      m_recvBuffer;
    std::string     m_tag;
};

} // namespace utp
} // namespace eular

#endif // __UTP_SOCKET_UDP_H__
