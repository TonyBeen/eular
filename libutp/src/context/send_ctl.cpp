/*************************************************************************
    > File Name: send_ctl.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 10:42:15 AM CST
 ************************************************************************/

#include "send_ctl.h"

#include "congestion/cubic.h"
#include "congestion/bbr_v1.h"

#include "util/time.h"
#include "logger/logger.h"

#ifndef N_NACKS_BEFORE_RETX
#define N_NACKS_BEFORE_RETX 3 // 默认重排序阈值为3，超过3个包未收到ACK则认为发生重排序
#endif

#define MAX_RTO_DELAY       60000000 // us
#define DEFAULT_RETX_DELAY  500000   // us
#define MIN_RTO_DELAY       200000   // us
#define INITIAL_RTT         333333   // us
#define MAX_RTO_BACKOFFS    10

namespace eular {
namespace utp {
SendControl::SendControl(ConnectionImpl *conn, ContextImpl *ctx) :
    m_conn(conn),
    m_ctx(ctx)
{
    m_retransTimer.reset(ctx->loop(), [this] () {
        onRetransTimer();
    });
    init();
}

void SendControl::init()
{
    TAILQ_INIT(&m_unackedPackets);
    TAILQ_INIT(&m_scheduledPackets);
    TAILQ_INIT(&m_lostPackets);
    m_congestion = (m_ctx->config()->cc_algorithm == 2 ?
        std::dynamic_pointer_cast<Congestion>(std::make_shared<Cubic>()) :
        std::dynamic_pointer_cast<Congestion>(std::make_shared<BbrV1>()));

    m_flags |= SendCtlFlags::Pace; // 默认开启速率控制
    m_currentPackNo = 0;
    m_retxFrames = (PacketFrameTypeBit)UTP_FRAME_RETX_MASK;
    m_sendHistory._last_sent = m_currentPackNo;
    m_pacer.init(m_ctx->config()->clock_granularity_us);
    for (int32_t i = 0; i < PriorityCount; ++i) {
        TAILQ_INIT(&m_bufferedPackets[i].packets);
    }
    m_cachedBpt._streamId = UINT32_MAX;
    m_reorderThresh = N_NACKS_BEFORE_RETX;
}

int32_t SendControl::packetSent(PacketOut *pkt)
{
    const std::string &packetFrames = FrameTypeToString(pkt->frame_types);
    UTP_LOGD("%s Packet sent: Packet No=%" PRIu64 ", frames=[%s], bytes=%u",
        m_tag.c_str(), pkt->packno, packetFrames.c_str(), pkt->data_size);
    m_sendHistory.update(pkt->packno);

    PacketInfo info;
    info.packetNo = pkt->packno;
    info.sendTimeUs = pkt->sent_time;
    info.packetSize = pkt->data_size;
    info.packetState = pkt->bw_state;
    m_congestion->onPacketSent(&info, m_bytesUnackedAll, m_flags & SendCtlFlags::AppLimited);
    appendUnacked(pkt);
    if (pkt->frame_types & m_retxFrames) { // 需要超时重传
        retransAlarm(pkt->sent_time);
        if (m_nInflightRetrans == 1) {
            m_flags |= SendCtlFlags::WasQuiet; // 当前存在可重传包, 标记之前为安静状态, 用于拥塞控制计算
        }
    }

#if defined(UTP_SEND_STATS)
    ++m_stats._totalSent;
#endif
    return 0;
}

bool SendControl::canSend()
{
    uint64_t cwnd = m_congestion->getCwnd();
    uint64_t bytesOut = bytesOutTotal();
    if (m_flags & SendCtlFlags::Pace) {
        if (bytesOut >= cwnd) {
            return false;
        }

        if (m_pacer.canSchedule(m_nScheduled + m_nInflightAll)) {
            return true;
        }

        if (m_flags & SendCtlFlags::SchedTick) {
            m_flags &= ~SendCtlFlags::SchedTick;
            auto now = time::MonotonicUs();
            m_conn->nextScheduleTime((m_pacer.nextSched() - now) / 1000 + 1);
        }
        return false;
    } else {
        return bytesOut < cwnd;
    }
}

uint64_t SendControl::bytesOutTotal() const
{
    return m_bytesScheduled + m_bytesUnackedAll;
}

void SendControl::appendUnacked(PacketOut *pkt)
{
    TAILQ_INSERT_TAIL(&m_unackedPackets, pkt, po_next);
    pkt->po_flags |= PacketOutFlags::kPoUnAcked;
    // TODO 确认是否需要加上包头大小
    m_bytesUnackedAll += (pkt->po_flags & PacketOutFlags::kPoEncrypted ? pkt->encrypt_data_size : pkt->data_size);
    m_nInflightAll++;
    if (pkt->frame_types & m_retxFrames) {
        m_bytesUnackedRetrans += (pkt->po_flags & PacketOutFlags::kPoEncrypted ? pkt->encrypt_data_size : pkt->data_size);
        m_nInflightRetrans++;
    }
}

void SendControl::retransAlarm(utp_time_t now)
{
    if (m_retransTimer.isActive()) {
        return;
    }

    utp_time_t delay = 0;
    assert(!TAILQ_EMPTY(&m_unackedPackets));
    RetransmissionMode mode = getRetransmissionMode();
    switch (mode) {
    case RetransmissionMode::kLoss: {
        delay = m_lossTo;
        break;
    }
    case RetransmissionMode::kTlp: {
        delay = calculateTlpDelay();
        break;
    }
    default: {
        assert(mode == RetransmissionMode::kRto);
        delay = calculatePacketRto();
        break;
    }
    }

    if (delay > MAX_RTO_DELAY) {
        delay = MAX_RTO_DELAY;
    }

    UTP_LOGD("%s Set retransmission timer: mode=%s, delay=%.2f ms, inflight=%" PRIu64,
        m_tag.c_str(), util::to_string(mode), delay / 1000.0, m_nInflightAll);
}

RetransmissionMode SendControl::getRetransmissionMode()
{
    if (m_lossTo) {
        return RetransmissionMode::kLoss;
    }
    if (m_nTlp < 2) {
        return RetransmissionMode::kTlp;
    }
    return RetransmissionMode::kRto;
}

void SendControl::onRetransTimer()
{
    utp_time_t now = time::MonotonicUs();
    RetransmissionMode rm = getRetransmissionMode();
    UTP_LOGD("%s Retransmission timer fired: mode=%s", m_tag.c_str(), util::to_string(rm));
    // switch (rm) {
    // case RetransmissionMode::kLoss: {
    //     break;
    // };
}

utp_time_t SendControl::calculateTlpDelay() const
{
    utp_time_t srtt = 0;
    utp_time_t delay = 0;
    srtt = m_conn->m_rttStats.srtt();
    if (0 == srtt) {
        srtt = INITIAL_RTT;
    }
    if (m_nInflightAll > 1) {
        delay = 10000; // 10 ms is the minimum tail loss probe delay
    } else {
        delay = srtt + srtt / 2 + m_conn->m_peerTP.max_ack_delay * 1000;
    }

    if (delay < 2 * srtt) {
        delay = 2 * srtt;
    }

    return delay;
}

utp_time_t SendControl::calculatePacketRto() const
{
    utp_time_t delay = 0;

    auto GetRtexDelay = [this] () -> utp_time_t {
        utp_time_t srtt = 0;
        utp_time_t delay = 0;
        srtt = m_conn->m_rttStats.srtt();
        if (0 == srtt) {
            delay = DEFAULT_RETX_DELAY;
        } else {
            delay = srtt + 4 * m_conn->m_rttStats.rttVar();
            if (delay < MIN_RTO_DELAY) {
                delay = MIN_RTO_DELAY;
            }
        }

        return delay;
    };

    delay = GetRtexDelay();
    auto exp = m_nConsecRtos;
    if (exp > MAX_RTO_BACKOFFS) {
        exp = MAX_RTO_BACKOFFS;
    }

    delay = delay * (1 << exp);
    return delay;
}

bool SendControl::detectLosses(utp_time_t now)
{
    utp_packno_t largestLostPackNo = 0;
    utp_packno_t largestRetxPackNo = largestRetxPacketNo();
    m_lossTo = 0;

    PacketOut *pktOut = nullptr;
    PacketOut *next = nullptr;
    PacketOut *lossRecord = nullptr;
    
    for (pktOut = TAILQ_FIRST(&m_unackedPackets); pktOut != nullptr && pktOut->packno <= m_largestAckedPackNo; pktOut = next) {
        next = TAILQ_NEXT(pktOut, po_next);

        if (pktOut->po_flags & PacketOutFlags::kPoLossRecorded) {
            continue;
        }

        // NOTE FACK 检测, 如果当前包的包号加上重排序阈值小于最大的ACK包号, 则认为发生了重排序, 该包被认为丢失
        if (pktOut->packno + m_reorderThresh < m_largestAckedPackNo) {
            UTP_LOGD("%s loss by FACK detected. (dist: %"PRIu64"), packet #%"PRIu64,
                m_tag.c_str(), m_largestAckedPackNo - pktOut->packno, pktOut->packno);
            // NOTE mtu 探测包丢失
            if (0 == (pktOut->po_flags & PacketOutFlags::kPoMtuProbe)) {
                largestLostPackNo = pktOut->packno;
                lossRecord = handleRegularLostPacket(pktOut, next);
                if (lossRecord) {
                    lossRecord->local_flags |= PacketOutLocalFlags::kPOLFacked;
                }
            } else {

            }
        }
    }
}

utp_packno_t SendControl::largestRetxPacketNo() const
{
    PacketOut *pkt = nullptr;
    TAILQ_FOREACH_REVERSE(pkt, &m_unackedPackets, PacketOutTailQ, po_next) {
        if (0 == (pkt->po_flags & (PacketOutFlags::kPoLossRecorded)) && (pkt->frame_types & m_retxFrames)) {
            return pkt->packno;
        }
    }

    return 0;
}

PacketOut *SendControl::handleRegularLostPacket(PacketOut *pkt, PacketOut *&next)
{

}

bool SendControl::handleLostMtuProbe(PacketOut *pkt)
{
    UTP_LOGD("%s MTU probe packet lost: packet #%" PRIu64, m_tag.c_str(), pkt->packno);
    unackedRemove(pkt);
    assert(pkt->loss_chain == pkt);
}

void SendControl::unackedRemove(PacketOut *pkt)
{
    TAILQ_REMOVE(&m_unackedPackets, pkt, po_next);
    pkt->po_flags &= ~PacketOutFlags::kPoUnAcked;
    assert(m_bytesUnackedAll >= pkt->data_size);
    m_bytesUnackedAll -= pkt->data_size;
    --m_nInflightAll;
    if (pkt->frame_types & m_retxFrames) {
        m_bytesUnackedRetrans -= pkt->data_size;
        --m_nInflightRetrans;
    }
}

} // namespace utp
} // namespace eular
