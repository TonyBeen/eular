/*************************************************************************
    > File Name: connection_impl.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:40:15 PM CST
 ************************************************************************/

#include "context/connection_impl.h"
#include "utp/errno.h"
#include "util/random.hpp"
#include "proto/frame.h"
#include "make_unique.hpp"

#include "connection_impl.h"

namespace eular {
namespace utp {
ConnectionImpl::ConnectionImpl(ContextImpl *ctx, UdpSocket *udpSocket, uint32_t cid) :
    m_ctx(ctx),
    m_udpSocket(udpSocket),
    m_localConnectionID(cid)
{
    m_connTimer.reset(ctx->loop(), [this] () {
        onConnTimeout();
    });
}

int32_t ConnectionImpl::connect(const Context::ConnectInfo &info)
{
    if (m_state != State::kStateDisconnected) {
        return UTP_ERR_INVALID_STATE;
    }

    m_state = State::kStateWaitSendInitial;
    m_connectInfo = info;
    m_ctx->wantWrite(this);
    m_connTimer.start(info.timeout);
    return UTP_ERR_NO_ERROR;
}

void ConnectionImpl::onWrite()
{
    if (m_state == State::kStateWaitSendInitial) {
        
    }
}

int32_t ConnectionImpl::sendInitialPacket()
{
    if (m_connectInfo.encrypted) {
        m_x25519 = std::make_unique<X25519Wrapper>();
        
    }
    
    
}

void ConnectionImpl::onConnTimeout()
{
}

} // namespace utp
} // namespace eular
