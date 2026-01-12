/*************************************************************************
    > File Name: mmsg.h
    > Author: eular
    > Brief:
    > Created Time: Wed 07 Jan 2026 04:04:45 PM CST
 ************************************************************************/

#ifndef __UTP_SOCKET_MMSG_H__
#define __UTP_SOCKET_MMSG_H__

#include <netinet/in.h>
#include <sys/socket.h>

#include "mtu/mtu.h"

#ifdef USE_SENDMMSG

#ifndef MAX_MMSG_SIZE
#define MAX_MMSG_SIZE  32
#endif

namespace eular {
namespace utp {
class MultipleMsg
{
public:
    MultipleMsg() = default;
    MultipleMsg(uint32_t size, uint32_t mss);
    ~MultipleMsg();

    void        resize(uint32_t size, uint32_t mss);
    void        reset();
    bool        valid() const { return m_buffer != nullptr; }
    uint32_t    size() const { return m_nMsg; }

    mmsghdr*    mmsghdrAt(uint32_t idx);
    sockaddr*   sockaddrAt(uint32_t idx);
    iovec*      iovecAt(uint32_t idx);
    char*       dataAt(uint32_t idx);
    char*       msgctrlAt(uint32_t idx);

private:
    uint32_t            bufferSize();
    mmsghdr*            mmsghdrBegin();
    sockaddr_storage*   sockaddrBegin();
    iovec*              iovecBegin();
    char*               dataBegin();
    char*               msgctrlBegin();

private:
    uint32_t        m_nMsg{0};
    uint32_t        m_mss{0};
    void*           m_buffer{nullptr};
};

} // namespace utp
} // namespace eular

#endif // USE_SENDMMSG
#endif // __UTP_SOCKET_MMSG_H__
