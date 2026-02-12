/*************************************************************************
    > File Name: send_ctl.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 10:42:15 AM CST
 ************************************************************************/

#include "send_ctl.h"

#include "congestion/cubic.h"
#include "congestion/bbr_v1.h"

namespace eular {
namespace utp {
SendControl::SendControl(ConnectionImpl *conn, ContextImpl *ctx) :
    m_conn(conn),
    m_ctx(ctx)
{
    TAILQ_INIT(&m_unackedPackets);
    m_congestion = (ctx->config()->cc_algorithm == 2 ?
        std::dynamic_pointer_cast<Congestion>(std::make_shared<Cubic>()) :
        std::dynamic_pointer_cast<Congestion>(std::make_shared<BbrV1>()));

    m_flags |= SendCtlFlags::Pace; // 默认开启速率控制
}

bool SendControl::canSend() const
{
    // if (m_conn->state() != ConnectionImpl::kStateConnected) {
        
    // }

    uint64_t cwnd = m_congestion->getCwnd();
    uint64_t bytesOut = bytesOutTotal();
    if (m_flags & SendCtlFlags::Pace) {
        if (bytesOut >= cwnd) {
            return false;
        }

        uint64_t bytesInFlight = m_bytesScheduled - m_bytesAcked;
        if (bytesInFlight >= cwnd) {
            return false;
        }
    } else {
        return bytesOut < cwnd;
    }
}

uint64_t SendControl::bytesOutTotal() const
{
    return m_bytesScheduled + m_bytesUnackedAll;
}

} // namespace utp
} // namespace eular
