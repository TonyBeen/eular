/*************************************************************************
    > File Name: connection_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:40:15 PM CST
 ************************************************************************/

#include "context/connection_impl.h"
#include "utp/errno.h"
#include "connection_impl.h"
#include "util/random.hpp"

namespace eular {
namespace utp {
ConnectionImpl::ConnectionImpl(ContextImpl *ctx, UdpSocket *udpSocket, uint32_t cid) :
    m_ctx(ctx),
    m_udpSocket(udpSocket),
    m_cid(cid)
{
    m_connTimer.reset(ctx->loop(), [this] () {
        onConnTimeout();
    });
}

int32_t ConnectionImpl::connect(const Context::ConnectInfo &info)
{
    m_state = State::kStateWaitSendInitial;
    m_connectInfo = info;

    int32_t status = sendInitialPacket();
    if (status == UTP_ERR_NO_ERROR) {
        m_state = State::kStateInitialSent;
        m_connTimer.start(info.timeout);
    } else if (status == UTP_ERR_WOULD_BLOCK) {
        m_ctx->wantWrite(this);
        status = UTP_ERR_NO_ERROR;
    }

    return status;
}

int32_t ConnectionImpl::sendInitialPacket()
{

}

void ConnectionImpl::onConnTimeout()
{
}

} // namespace utp
} // namespace eular
