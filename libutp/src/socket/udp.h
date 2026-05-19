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

#include "utp/config.h"
#include "commom.h"
#include "socket/mmsg.h"
#include "socket/packet.h"
#include "util/status.h"

namespace eular {
namespace utp {
class UdpSocket
{
public:
    static constexpr uint8_t kMaxMsgSlices = 4;

    struct MsgSlice {
        const void* data;
        size_t      len;
    };

    struct MsgMetaInfo {
        const void*     data;
        size_t          len;
        uint8_t         slice_count;
        MsgSlice        slices[kMaxMsgSlices];
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

    UdpSocket(Config &config);
    ~UdpSocket();

public:
    void updateTag(const std::string& tag);
    const std::string& tag() const { return m_tag; }
    socket_t fd() const { return m_sock; }

public:
    bool isValid() const { return m_sock != INVALID_SOCKET; }

    Status bind(const std::string &ip, uint16_t port, const std::string &ifname);

    int32_t recvErrorMsg(ErrorMsg &errMsg, Status &status);

    /**
     * @brief 读取数据
     *
     * @param msgVec 数据缓存
     * @return int32_t 返回读取到的数据包数量, 小于0表示失败, 等于0表示无数据
     */
    int32_t recv(std::vector<MsgMetaInfo>& msgVec, Status &status);
    int32_t send(const MsgMetaInfo &msg, Status &status);
    int32_t send(const MsgMetaInfo *msgVec, size_t count, Status &status);

    // TODO(next): 在 Linux 上增加 MSG_ZEROCOPY 发送与错误队列完成事件处理（SO_EE_ORIGIN_ZEROCOPY）。
    // NOTE: 需要与 SendControl/PacketOut 生命周期联动，避免在 completion 前释放或改写缓冲。
    int32_t send(const std::vector<MsgMetaInfo> &msgVec, Status &status);

private:
    socket_t        m_sock{INVALID_SOCKET};
    Address         m_bindAddr;
    Address         m_localAddr;
    const Config&   m_config;

#if defined(USE_SENDMMSG)
    MultipleMsg     m_mmsg;
#endif
    ByteBuffer      m_recvBuffer;
    std::string     m_tag;
};

} // namespace utp
} // namespace eular

#endif // __UTP_SOCKET_UDP_H__
