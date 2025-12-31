/*************************************************************************
    > File Name: bw_sampler.cpp
    > Author: hsz
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:23:17 PM CST
 ************************************************************************/

#include "congestion/bw_sampler.h"

#include <inttypes.h>

#include "logger/utp_log.h"
#include "bw_sampler.h"

namespace eular {
namespace utp {
BandwidthSampler::BandwidthSampler() :
    m_samplePool(256)
{
    m_flags |= BWS_APP_LIMITED;
}

BandwidthSampler::~BandwidthSampler()
{
}

void BandwidthSampler::onPacketSent(PacketInfo *packetInfo, uint64_t inFlight)
{
    BWPacketState *packetState;
    uint16_t sentSize = 0;

    if (packetInfo->packetState) {
        UTP_LOGW("packet %"PRIu64" already has associated packet state", packetInfo->packetNo);
        return;
    }

    m_lastSentPackNo = packetInfo->packetNo;
    sentSize = packetInfo->packetSize;
    m_totalSent += sentSize;

    // 如果没有正在传输的数据包，则可以将新传输打开的时间视为A_0点，以便进行带宽采样。
    // 这在一定程度上低估了带宽，并为飞行中的大多数数据包产生了一些人为的低样本，
    // 但它在重要点提供了样本，否则我们将无法获得这些样本，最重要的是在连接开始时。
    if (inFlight == 0) {
        m_lastAckedPacketTime = packetInfo->sendTimeUs;
        m_lastAckedTotalSent = m_totalSent;
        m_lastAckedSentTime = packetInfo->sendTimeUs;
    }

    packetState = m_samplePool.get();
    if (!packetState) {
        UTP_LOGE("failed to allocate packet state for packet %"PRIu64, packetInfo->packetNo);
        return;
    }
    packetState->sendState.totalBytesSent = m_totalSent;
    packetState->sendState.totalBytesAcked = m_totalAcked;
    packetState->sendState.totalBytesLost = m_totalLost;
    packetState->sendState.isAppLimited = !!(m_flags & BWS_APP_LIMITED);

    packetState->sentAtLastAck = m_lastAckedTotalSent;
    packetState->lastAckSentTime = m_lastAckedSentTime;
    packetState->lastAckAckTime = m_lastAckedPacketTime;
    packetState->packetSize = sentSize;

    packetInfo->packetState = packetState;
    UTP_LOGD("add info for packet %"PRIu64, packetInfo->packetNo);
}

BWSample BandwidthSampler::onPacketAcked(PacketInfo *packetInfo, uint64_t ackTime)
{
    BWSample sample;
    sample.rtt = -1;
    BandWidth sendRate;
    BandWidth ackRate;
    uint64_t rtt;
    uint16_t sentSize;
    bool isAppLimited;
    if (!packetInfo->packetState) {
        return sample;
    }

    BWPacketState *packetState = static_cast<BWPacketState *>(packetInfo->packetState);
    sentSize = packetInfo->packetSize;

    m_totalAcked += sentSize;
    m_lastAckedTotalSent = packetState->sendState.totalBytesSent;
    m_lastAckedSentTime = packetInfo->sendTimeUs; // 当前包已收到Ack, 更新旧包发送时间
    m_lastAckedPacketTime = ackTime; // 当前包已收到Ack, 更新旧包收到Ack时间

    // 一旦确认连接不受应用程序限制, 就退出应用程序限制阶段
    if (m_flags & BWS_APP_LIMITED && packetInfo->packetNo > m_endOfAppLimitedPhase) {
        m_flags &= ~BWS_APP_LIMITED;
        UTP_LOGD("exit app-limited phase due to packet %"PRIu64" being acked", packetInfo->packetNo);
    }

    do {
        // 当前数据包发送时可能没有确认的数据包。在这种情况下，不需要进行带宽采样
        if (packetState->lastAckSentTime == 0) {
            break;
        }

        // 无限速率表示采样器应该丢弃当前的发送速率样本, 只使用ack速率。
        if (packetInfo->sendTimeUs > packetState->lastAckSentTime) {
            sendRate = BW_FROM_BYTES_AND_DELTA(
                packetState->sendState.totalBytesSent - packetState->sentAtLastAck,
                packetInfo->sendTimeUs - packetState->lastAckSentTime);
        } else {
            sendRate = BW_INFINITE();
        }

        // 在斜率计算过程中，确保当前数据包的ack时间始终大于前一个数据包的时间，否则可能会发生除零或整数下溢
        if (ackTime <= packetState->lastAckAckTime) {
            UTP_LOGD("Time of the previously acked packet (%"PRIu64") is larger than the ack time of the current packet (%"PRIu64")",
                packetState->lastAckAckTime, ackTime);
            break;
        }

        ackRate = BW_FROM_BYTES_AND_DELTA(m_totalAcked - packetState->sendState.totalBytesAcked, ackTime - packetState->lastAckAckTime);
        UTP_LOGD("send rate: %"PRIu64"; ack rate: %"PRIu64, sendRate.value, ackRate.value);

        rtt = ackTime - packetInfo->sendTimeUs;
        isAppLimited = packetState->sendState.isAppLimited;

        m_samplePool.put(packetState);

        if (BW_VALUE(&sendRate) < BW_VALUE(&ackRate)) {
            sample.bandwidth = sendRate;
        } else {
            sample.bandwidth = ackRate;
        }
        sample.rtt = rtt;
        sample.isAppLimited = isAppLimited;

        UTP_LOGD("packet %"PRIu64" acked, bandwidth: %"PRIu64" bps", packetInfo->packetNo, BW_VALUE(&sample.bandwidth));
        return sample;
    } while (0);

    m_samplePool.put(packetState);
    packetInfo->packetState = nullptr;
    return sample;
}

void BandwidthSampler::onPacketLost(PacketInfo *packetInfo)
{
    if (!packetInfo->packetState) {
        return;
    }

    BWPacketState *packetState = static_cast<BWPacketState *>(packetInfo->packetState);
    m_totalLost += packetInfo->packetSize;
    m_samplePool.put(packetState);
    packetInfo->packetState = nullptr;
    UTP_LOGD("packet %"PRIu64" lost, total_lost goes to %"PRIu64, packetInfo->packetNo, m_totalLost);
}

void BandwidthSampler::appLimited()
{
    m_flags |= BWS_APP_LIMITED;
    m_endOfAppLimitedPhase = m_lastSentPackNo;
    UTP_LOGD("app limited, end of limited phase is %"PRIu64, m_endOfAppLimitedPhase);
}
} // namespace utp
} // namespace eular
