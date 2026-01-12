#include "socket/mmsg.h"

#ifdef USE_SENDMMSG

#include <stdlib.h>

#include "util/error.h"
#include "mmsg.h"

namespace eular {
namespace utp {

static constexpr size_t MSG_CTRL_SIZE = CMSG_SPACE(sizeof(in6_pktinfo));

MultipleMsg::MultipleMsg(uint32_t size, uint32_t mss) :
    m_nMsg(size),
    m_mss(mss),
    m_buffer(nullptr)
{
    resize(size, mss);
}

MultipleMsg::~MultipleMsg()
{
    resize(0, 0);
}

void MultipleMsg::resize(uint32_t size, uint32_t mss)
{
    if (m_buffer) {
        free(m_buffer);
        m_buffer = nullptr;
    }

    m_nMsg = size;
    m_mss = mss;
    if (m_nMsg == 0 || m_mss == 0) {
        return;
    }

    size_t bufSize = bufferSize();
    m_buffer = reinterpret_cast<char *>(malloc(bufSize));
    if (!m_buffer) {
        SetLastErrorV(UTP_ERR_NO_MEMORY, "MultipleMsg::resize malloc failed! size={}", bufSize);
        return;
    }

    // Initialize mmsghdr, iovec
    for (uint32_t i = 0; i < m_nMsg; ++i) {
        iovec *iov = iovecAt(i);
        iov->iov_base = dataAt(i);
        iov->iov_len = m_mss;

        mmsghdr *msg = mmsghdrAt(i);
        msg->msg_hdr.msg_name = sockaddrAt(i);
        msg->msg_hdr.msg_namelen = sizeof(sockaddr_storage);
        msg->msg_hdr.msg_iov = iov;
        msg->msg_hdr.msg_iovlen = 1;
        msg->msg_hdr.msg_control = msgctrlAt(i);
        msg->msg_hdr.msg_controllen = MSG_CTRL_SIZE;
        msg->msg_hdr.msg_flags = 0;
        msg->msg_len = 0;
    }
}

void MultipleMsg::reset()
{
    if (!m_buffer) {
        return;
    }

    // Initialize mmsghdr, iovec
    for (uint32_t i = 0; i < m_nMsg; ++i) {
        iovec *iov = iovecAt(i);
        iov->iov_base = dataAt(i);
        iov->iov_len = m_mss;

        mmsghdr *msg = mmsghdrAt(i);
        msg->msg_hdr.msg_name = sockaddrAt(i);
        msg->msg_hdr.msg_namelen = sizeof(sockaddr_storage);
        msg->msg_hdr.msg_iov = iov;
        msg->msg_hdr.msg_iovlen = 1;
        msg->msg_hdr.msg_control = msgctrlAt(i);
        msg->msg_hdr.msg_controllen = MSG_CTRL_SIZE;
        msg->msg_hdr.msg_flags = 0;
        msg->msg_len = 0;
    }
}

mmsghdr *MultipleMsg::mmsghdrAt(uint32_t idx)
{
    return &mmsghdrBegin()[idx];
}

sockaddr *MultipleMsg::sockaddrAt(uint32_t idx)
{
    return reinterpret_cast<sockaddr *>(&sockaddrBegin()[idx]);
}

iovec *MultipleMsg::iovecAt(uint32_t idx)
{
    return &iovecBegin()[idx];
}

char *MultipleMsg::dataAt(uint32_t idx)
{
    return dataBegin() + idx * m_mss;
}

char *MultipleMsg::msgctrlAt(uint32_t idx)
{
    return msgctrlBegin() + idx * MSG_CTRL_SIZE;
}

uint32_t MultipleMsg::bufferSize()
{
    // buffer structure:
    // - mmsghdr * m_nMsg
    // - sockaddr_storage * m_nMsg
    // - iovec * m_nMsg
    // - data buffer * m_nMsg
    // - control message * m_nMsg
    return m_nMsg * (sizeof(mmsghdr) + sizeof(sockaddr_storage) + sizeof(iovec) + m_mss + MSG_CTRL_SIZE);
}
mmsghdr *MultipleMsg::mmsghdrBegin()
{
    return reinterpret_cast<mmsghdr *>(m_buffer);
}

sockaddr_storage *MultipleMsg::sockaddrBegin()
{
    return reinterpret_cast<sockaddr_storage *>(m_buffer + m_nMsg * sizeof(mmsghdr));
}

iovec *MultipleMsg::iovecBegin()
{
    return reinterpret_cast<iovec *>(m_buffer + m_nMsg * (sizeof(mmsghdr) + sizeof(sockaddr_storage)));
}

char *MultipleMsg::dataBegin()
{
    return reinterpret_cast<char *>(m_buffer + m_nMsg * (sizeof(mmsghdr) + sizeof(sockaddr_storage) + sizeof(iovec)));
}

char *MultipleMsg::msgctrlBegin()
{
    return reinterpret_cast<char *>(m_buffer + m_nMsg * (sizeof(mmsghdr) + sizeof(sockaddr_storage) + sizeof(iovec) + m_mss));
}
} // namespace utp
} // namespace eular

#endif // USE_SENDMMSG
