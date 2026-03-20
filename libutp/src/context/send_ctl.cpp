/*************************************************************************
    > File Name: send_ctl.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 10:42:15 AM CST
 ************************************************************************/

#include "send_ctl.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include <utils/serialize.hpp>

#include "context/context_impl.h"
#include "context/connection_impl.h"

#include "congestion/cubic.h"
#include "congestion/bbr_v1.h"

#include "proto/proto.h"
#include "proto/frame/padding.h"
#include "utp/errno.h"
#include "util/ack_info.h"
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

namespace {

using eular::Serialize;

bool IsPackNoAcked(const eular::utp::AckInfo &ackInfo, utp_packno_t packno)
{
    if (ackInfo.range_size == 0 || ackInfo.range_size > ackInfo.ack_ranges.size()) {
        return false;
    }

    const uint32_t rangeCount = ackInfo.range_size;
    for (uint32_t i = 0; i < rangeCount; ++i) {
        const Range &range = ackInfo.ack_ranges[i];
        if (packno >= range.low && packno <= range.high) {
            return true;
        }
    }

    return false;
}

eular::utp::FrameType FirstFrameType(uint32_t frameTypes)
{
    for (uint32_t bit = 0; bit < static_cast<uint32_t>(eular::utp::kFrameMax); ++bit) {
        if (frameTypes & (1u << bit)) {
            return static_cast<eular::utp::FrameType>(bit);
        }
    }

    return eular::utp::kFrameInvalid;
}

bool RewritePacketNumber(eular::utp::ConnectionImpl *conn, eular::utp::PacketOut *pkt)
{
    constexpr size_t kPacketNumberOffset = offsetof(eular::utp::UTPHeaderProto, pn);

    if (conn == nullptr || pkt == nullptr || pkt->raw_data == nullptr) {
        return false;
    }
    if (pkt->data_size < UTP_HEADER_SIZE) {
        return false;
    }
    if (pkt->alloc_size < kPacketNumberOffset + sizeof(pkt->packno)) {
        return false;
    }

    pkt->packno = conn->packetNumber();
    uint8_t *offset = pkt->raw_data + kPacketNumberOffset;
    size_t left = pkt->alloc_size - kPacketNumberOffset;
    return Serialize::SerializeTo(offset, left, pkt->packno) != nullptr;
}

bool BuildMtuProbePayload(uint16_t packetSize,
                          std::vector<uint8_t> &payload)
{
    if (packetSize <= UTP_HEADER_SIZE + 1) {
        return false;
    }

    const size_t payloadSize = static_cast<size_t>(packetSize - UTP_HEADER_SIZE);
    payload.assign(payloadSize, 0);
    payload[0] = static_cast<uint8_t>(eular::utp::kFramePing);
    if (payloadSize == 1) {
        return true;
    }

    const size_t paddingFrameSize = payloadSize - 1;
    if (paddingFrameSize < FRAME_PADDING_HDR_SIZE) {
        return false;
    }

    eular::utp::FramePadding padding;
    padding.padding_length = static_cast<uint16_t>(paddingFrameSize - FRAME_PADDING_HDR_SIZE);
    int32_t encoded = padding.encode(payload.data() + 1, paddingFrameSize);
    if (encoded < 0 || static_cast<size_t>(encoded) != paddingFrameSize) {
        return false;
    }

    return true;
}

} // namespace

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

SendControl::~SendControl()
{
    if (m_retransTimer.isActive()) {
        m_retransTimer.stop();
    }

    auto drainQueue = [this] (PacketOutTailQ &queue) {
        PacketOut *pkt = nullptr;
        while ((pkt = TAILQ_FIRST(&queue)) != nullptr) {
            TAILQ_REMOVE(&queue, pkt, po_next);
            pkt->po_flags &= ~(PacketOutFlags::kPoUnAcked | PacketOutFlags::kPoSched | PacketOutFlags::kPoLost);
            destroyPacket(pkt);
        }
    };

    drainQueue(m_unackedPackets);
    drainQueue(m_scheduledPackets);
    drainQueue(m_lostPackets);
    for (int32_t i = 0; i < PriorityCount; ++i) {
        drainQueue(m_bufferedPackets[i].packets);
        m_bufferedPackets[i].count = 0;
    }
}

void SendControl::init()
{
    TAILQ_INIT(&m_unackedPackets);
    TAILQ_INIT(&m_scheduledPackets);
    TAILQ_INIT(&m_lostPackets);

    const Config *cfg = (m_ctx != nullptr) ? m_ctx->config() : nullptr;
    if (cfg != nullptr && cfg->cc_algorithm == 2) {
        m_congestion = std::dynamic_pointer_cast<Congestion>(std::make_shared<Cubic>());
    } else {
        m_congestion = std::dynamic_pointer_cast<Congestion>(std::make_shared<BbrV1>());
    }

    if (m_congestion && m_conn != nullptr) {
        m_congestion->onInit(&m_conn->m_rttStats);
    }

    m_flags |= SendCtlFlags::Pace; // 默认开启速率控制
    m_currentPackNo = 0;
    m_largestAckedPackNo = 0;
    m_largestAckedSentTime = 0;
    m_lastSentTime = 0;
    m_lastRtoTime = 0;
    m_bytesUnackedRetrans = 0;
    m_bytesScheduled = 0;
    m_bytesUnackedAll = 0;
    m_nInflightAll = 0;
    m_nInflightRetrans = 0;
    m_nConsecRtos = 0;
    m_nHandshake = 0;
    m_nTlp = 0;
    m_largestSentAtCutback = 0;
    m_maxRttPackNo = 0;
    m_largestAck2ed = 0;
    m_largestAcked = 0;
    m_lossTo = 0;
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

    if (!pkt->addSendAttempt(pkt->packno, pkt->sent_time)) {
        UTP_LOGW("%s record send attempt failed: Packet No=%" PRIu64,
            m_tag.c_str(), pkt->packno);
    }

    m_sendHistory.update(pkt->packno);

    PacketInfo info;
    info.packetNo = pkt->packno;
    info.sendTimeUs = pkt->sent_time;
    info.packetSize = pkt->data_size;
    info.packetState = pkt->bw_state;
    m_congestion->onPacketSent(&info, m_bytesUnackedAll, m_flags & SendCtlFlags::AppLimited);
    pkt->bw_state = static_cast<BWPacketState *>(info.packetState);
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

int32_t SendControl::onAckReceived(const AckInfo &ackInfo, utp_time_t nowUs)
{
    bool hasAcked = false;
    bool hasLoss = false;
    const utp_packno_t largestAckedBefore = m_largestAckedPackNo;
    if (ackInfo.largest_ack_packno > m_largestAckedPackNo) {
        m_largestAckedPackNo = ackInfo.largest_ack_packno;
    }
    const bool ackProgress = m_largestAckedPackNo > largestAckedBefore;

    if (m_congestion) {
        m_congestion->onBeginAck(nowUs, m_bytesUnackedAll);
    }

    PacketOut *pkt = nullptr;
    PacketOut *next = nullptr;
    for (pkt = TAILQ_FIRST(&m_unackedPackets); pkt != nullptr; pkt = next) {
        next = TAILQ_NEXT(pkt, po_next);

        if (!IsPackNoAcked(ackInfo, pkt->packno)) {
            continue;
        }

        hasAcked = true;

        if (pkt->packno > m_largestAckedPackNo) {
            m_largestAckedPackNo = pkt->packno;
            m_largestAckedSentTime = pkt->sent_time;
        }

        if (m_congestion) {
            PacketInfo info;
            info.packetNo = pkt->packno;
            info.sendTimeUs = pkt->sent_time;
            info.packetSize = pkt->data_size;
            info.packetState = pkt->bw_state;
            m_congestion->onAck(&info, nowUs, m_flags & SendCtlFlags::AppLimited);
            pkt->bw_state = static_cast<BWPacketState *>(info.packetState);
        }

        if ((pkt->po_flags & PacketOutFlags::kPoMtuProbe) && m_conn != nullptr) {
            m_conn->m_mtuDiscovery.onProbeAck(pkt->packno, nowUs / 1000);
        }

        unackedRemove(pkt);
        destroyPacket(pkt);
    }

    if (m_congestion) {
        m_congestion->onEndAck(m_bytesUnackedAll);
    }

    if (hasAcked) {
        m_nConsecRtos = 0;
    }

    if (!TAILQ_EMPTY(&m_unackedPackets)) {
        if (hasAcked || ackProgress) {
            hasLoss = detectLosses(nowUs);
            retransAlarm(nowUs);

            if (hasLoss && m_conn != nullptr) {
                m_conn->nextScheduleTime(1);
            }
        }
    } else {
        m_lossTo = 0;
        if (m_retransTimer.isActive()) {
            m_retransTimer.stop();
        }
    }

    return UTP_ERR_OK;
}

void SendControl::onCanWrite(utp_time_t nowUs)
{
    if (m_conn == nullptr || m_conn->m_udpSocket == nullptr) {
        return;
    }

    bool sentAny = false;
    while (!TAILQ_EMPTY(&m_lostPackets) && canSend()) {
        PacketOut *pkt = TAILQ_FIRST(&m_lostPackets);
        if (pkt == nullptr) {
            break;
        }

        if (retransmitLostPacket(pkt, nowUs) != UTP_ERR_OK) {
            break;
        }

        sentAny = true;
        nowUs = time::MonotonicUs();
    }

    if (!TAILQ_EMPTY(&m_lostPackets) && !sentAny && m_conn != nullptr) {
        m_conn->nextScheduleTime(1);
    }

    if (m_conn == nullptr || m_ctx == nullptr || !canSend()) {
        return;
    }

    MtuDiscovery &mtu = m_conn->m_mtuDiscovery;
    const utp_time_t nowMs = nowUs / 1000;
    mtu.onProbeTimeout(nowMs);

    if (!mtu.shouldProbe(nowMs)) {
        return;
    }

    const uint16_t probeMtu = mtu.nextProbeMtu();
    const uint16_t probePacketSize = MtuDiscovery::PacketSizeFromMtu(probeMtu, m_conn->m_peerAddress.family());
    if (probePacketSize <= UTP_HEADER_SIZE + 1) {
        return;
    }

    std::vector<uint8_t> probePayload;
    if (!BuildMtuProbePayload(probePacketSize, probePayload)) {
        return;
    }

    utp_packno_t probePackNo = 0;
    int32_t sent = m_conn->sendPacket(UTP_TYPE_CTRL,
                                      probePayload.data(),
                                      probePayload.size(),
                                      PacketOutFlags::kPoMtuProbe,
                                      &probePackNo);
    if (sent == UTP_ERR_OK) {
        mtu.onProbeSent(probePackNo, probeMtu, nowMs);
        UTP_LOGD("%s sent MTU probe packet: packno=%" PRIu64 ", probe_mtu=%u, packet_size=%u",
            m_tag.c_str(),
            probePackNo,
            static_cast<uint32_t>(probeMtu),
            static_cast<uint32_t>(probePacketSize));
    }
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
    if (TAILQ_EMPTY(&m_unackedPackets)) {
        if (m_retransTimer.isActive()) {
            m_retransTimer.stop();
        }
        return;
    }

    if (m_retransTimer.isActive()) {
        m_retransTimer.stop();
    }

    utp_time_t delay = 0;
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

    utp_time_t delayMs = delay / 1000;
    if (delayMs == 0) {
        delayMs = 1;
    }

    m_retransTimer.start(delayMs);
    m_lastRtoTime = now;

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

    if (TAILQ_EMPTY(&m_unackedPackets)) {
        m_lossTo = 0;
        return;
    }

    bool hasLoss = false;
    if (rm == RetransmissionMode::kLoss) {
        hasLoss = detectLosses(now);
    } else {
        PacketOut *pkt = nullptr;
        PacketOut *next = nullptr;
        for (pkt = TAILQ_FIRST(&m_unackedPackets); pkt != nullptr; pkt = next) {
            next = TAILQ_NEXT(pkt, po_next);
            if (0 == (pkt->frame_types & m_retxFrames)) {
                continue;
            }

            if (m_congestion) {
                PacketInfo info;
                info.packetNo = pkt->packno;
                info.sendTimeUs = pkt->sent_time;
                info.packetSize = pkt->data_size;
                info.packetState = pkt->bw_state;
                m_congestion->onLost(&info);
            }

            hasLoss = (handleRegularLostPacket(pkt, next) != nullptr) || hasLoss;
        }

        if (rm == RetransmissionMode::kRto) {
            ++m_nConsecRtos;
            if (m_congestion) {
                m_congestion->onTimeout();
            }
        } else {
            ++m_nTlp;
        }
    }

    if (hasLoss && m_conn != nullptr) {
        m_conn->nextScheduleTime(1);
    }

    retransAlarm(now);
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
    (void)now;
    utp_packno_t largestLostPackNo = 0;
    utp_packno_t largestRetxPackNo = largestRetxPacketNo();
    m_lossTo = 0;
    bool hasLoss = false;

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
            UTP_LOGD("%s loss by FACK detected. (dist: %" PRIu64 "), packet #%" PRIu64,
                m_tag.c_str(), m_largestAckedPackNo - pktOut->packno, pktOut->packno);
            // NOTE mtu 探测包丢失
            if (0 == (pktOut->po_flags & PacketOutFlags::kPoMtuProbe)) {
                largestLostPackNo = pktOut->packno;
                lossRecord = handleRegularLostPacket(pktOut, next);
                if (lossRecord) {
                    lossRecord->local_flags |= PacketOutLocalFlags::kPOLFacked;
                    if (m_congestion) {
                        PacketInfo info;
                        info.packetNo = lossRecord->packno;
                        info.sendTimeUs = lossRecord->sent_time;
                        info.packetSize = lossRecord->data_size;
                        info.packetState = lossRecord->bw_state;
                        m_congestion->onLost(&info);
                        lossRecord->bw_state = static_cast<BWPacketState *>(info.packetState);
                    }
                    hasLoss = true;
                }
            } else {
                hasLoss = handleLostMtuProbe(pktOut) || hasLoss;
            }
        }
    }

    (void)largestLostPackNo;
    (void)largestRetxPackNo;
    return hasLoss;
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
    (void)next;
    if (pkt == nullptr) {
        return nullptr;
    }

    unackedRemove(pkt);
    pkt->po_flags |= PacketOutFlags::kPoLost;
    pkt->po_flags |= PacketOutFlags::kPoLossRecorded;
    pkt->po_flags |= PacketOutFlags::kPoResetPackNo;
    pkt->loss_chain = pkt;
    TAILQ_INSERT_TAIL(&m_lostPackets, pkt, po_next);
    return pkt;

}

bool SendControl::handleLostMtuProbe(PacketOut *pkt)
{
    if (pkt == nullptr) {
        return false;
    }

    UTP_LOGD("%s MTU probe packet lost: packet #%" PRIu64, m_tag.c_str(), pkt->packno);
    if (m_conn != nullptr) {
        m_conn->m_mtuDiscovery.onProbeLost(pkt->packno, time::MonotonicMs());
    }
    unackedRemove(pkt);
    assert(pkt->loss_chain == pkt);
    destroyPacket(pkt);
    return true;
}

int32_t SendControl::retransmitLostPacket(PacketOut *pkt, utp_time_t nowUs)
{
    if (pkt == nullptr || m_conn == nullptr || m_conn->m_udpSocket == nullptr) {
        return UTP_ERR_INVALID_PARAM;
    }

    const FrameType frameType = FirstFrameType(pkt->frame_types);
    if (!m_conn->canSendOnCurrentPath(pkt->data_size, frameType)) {
        return UTP_ERR_WOULD_BLOCK;
    }

    const utp_packno_t previousPackNo = pkt->packno;
    if ((pkt->po_flags & PacketOutFlags::kPoResetPackNo) && !RewritePacketNumber(m_conn, pkt)) {
        return UTP_ERR_INTERNAL_ERROR;
    }

    UdpSocket::MsgMetaInfo msg;
    std::memset(&msg, 0, sizeof(msg));
    if ((pkt->po_flags & PacketOutFlags::kPoEncrypted) && pkt->encrypt_data != nullptr) {
        msg.data = pkt->encrypt_data;
        msg.len = pkt->data_size;
    } else {
        msg.data = pkt->raw_data;
        msg.len = pkt->data_size;
        msg.slice_count = pkt->slice_count;
        for (uint8_t i = 0; i < pkt->slice_count && i < UdpSocket::kMaxMsgSlices; ++i) {
            msg.slices[i].data = pkt->raw_data + pkt->slices[i].offset;
            msg.slices[i].len = pkt->slices[i].length;
        }
    }
    msg.metaInfo.peerAddress = m_conn->m_peerAddress;

    std::vector<UdpSocket::MsgMetaInfo> msgVec(1, msg);
    int32_t sent = m_conn->m_udpSocket->send(msgVec);
    if (sent <= 0) {
        return UTP_ERR_SOCKET_WRITE;
    }

    TAILQ_REMOVE(&m_lostPackets, pkt, po_next);
    pkt->po_flags &= ~(PacketOutFlags::kPoLost | PacketOutFlags::kPoLossRecorded | PacketOutFlags::kPoResetPackNo);
    pkt->sent_time = nowUs;

    if (!pkt->addSendAttempt(pkt->packno, pkt->sent_time)) {
        UTP_LOGW("%s record retransmit attempt failed: Packet No=%" PRIu64,
            m_tag.c_str(), pkt->packno);
    }

    m_conn->m_bytesOut += pkt->data_size;

    m_sendHistory.update(pkt->packno);
    if (m_congestion) {
        PacketInfo info;
        info.packetNo = pkt->packno;
        info.sendTimeUs = pkt->sent_time;
        info.packetSize = pkt->data_size;
        info.packetState = pkt->bw_state;
        m_congestion->onPacketSent(&info, m_bytesUnackedAll, m_flags & SendCtlFlags::AppLimited);
        pkt->bw_state = static_cast<BWPacketState *>(info.packetState);
    }

    appendUnacked(pkt);
    if (pkt->frame_types & m_retxFrames) {
        retransAlarm(nowUs);
    }

    const std::string packetFrames = FrameTypeToString(pkt->frame_types);
    UTP_LOGD("%s Retransmit packet sent: old Packet No=%" PRIu64 ", new Packet No=%" PRIu64 ", frames=[%s], bytes=%u",
        m_tag.c_str(), previousPackNo, pkt->packno, packetFrames.c_str(), static_cast<uint32_t>(pkt->data_size));

    return UTP_ERR_OK;
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

void SendControl::destroyPacket(PacketOut *pkt)
{
    if (pkt == nullptr) {
        return;
    }

    if (pkt->bw_state != nullptr && m_congestion) {
        PacketInfo info;
        info.packetNo = pkt->packno;
        info.sendTimeUs = pkt->sent_time;
        info.packetSize = pkt->data_size;
        info.packetState = pkt->bw_state;
        m_congestion->onLost(&info);
        pkt->bw_state = static_cast<BWPacketState *>(info.packetState);
    }

    if (m_conn == nullptr) {
        UTP_LOGE("%s destroy packet failed: null connection", m_tag.c_str());
        return;
    }

    m_conn->m_mm.putPacketOut(pkt);
}

} // namespace utp
} // namespace eular
