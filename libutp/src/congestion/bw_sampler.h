/*************************************************************************
    > File Name: bw_sampler.h
    > Author: eular
    > Brief:
    > Created Time: Mon 08 Dec 2025 04:23:14 PM CST
 ************************************************************************/

#ifndef __CONGESTION_BW_SAMPLER_H__
#define __CONGESTION_BW_SAMPLER_H__

#include <list>

#include "congestion/congestion.h"
#include "util/malo.hpp"

/* This struct provides a type for bits per second units.  It's made into
 * a struct so that it is a little harder to make a mistake.  The Chromium
 * equivalent of this is QuicBandwidth.  Use macros to operate.
 */
struct BandWidth {
    uint64_t    value; // bits per second
};

#define BW_INFINITE() ((struct BandWidth) { .value = UINT64_MAX, })
#define BW_ZERO() ((struct BandWidth) { .value = 0, })
#define BW_FROM_BYTES_AND_DELTA(bytes_, usecs_) ((struct BandWidth) { .value = (bytes_) * 8 * 1000000 / (usecs_), })
#define BW_IS_ZERO(bw_) ((bw_)->value == 0)
#define BW_TO_BYTES_PER_SEC(bw_) ((bw_)->value / 8)
#define BW_VALUE(bw_) (+(bw_)->value)
#define BW_TIMES(bw_, factor_) ((struct BandWidth) { .value = BW_VALUE(bw_) * (factor_), })
#define BW(initial_value_) ((struct BandWidth) { .value = (initial_value_) })

namespace eular {
namespace utp {
struct SendState
{
    uint64_t    totalBytesSent;
    uint64_t    totalBytesAcked;
    uint64_t    totalBytesLost;
    bool        isAppLimited;
};

struct BWPacketState
{
    SendState   sendState;
    uint16_t    packetSize;
    uint64_t    sentAtLastAck;
    uint64_t    lastAckSentTime;
    uint64_t    lastAckAckTime;
};

struct BWSample
{
    BandWidth   bandwidth;
    int64_t     rtt;
    bool        isAppLimited;

    bool valid() const { return rtt >= 0; }
};

class BandwidthSampler
{
public:
    enum {
        BWS_CONN_ABORTED    = 1 << 0,
        BWS_WARNED          = 1 << 1,
        BWS_APP_LIMITED     = 1 << 2,
    };

    BandwidthSampler();
    ~BandwidthSampler();

    void        onPacketSent(PacketInfo *packetInfo, uint64_t inFlight);
    BWSample    onPacketAcked(PacketInfo *packetInfo, uint64_t ackTime);
    void        onPacketLost(PacketInfo *packetInfo);
    void        appLimited();

    uint64_t    totalAcked() const { return m_totalAcked; }

private:
    uint32_t    m_flags;
    uint64_t    m_totalSent;                // 总发送字节数
    uint64_t    m_totalAcked;               // 总确认字节数
    uint64_t    m_totalLost;                // 总丢包字节数
    uint64_t    m_lastAckedTotalSent;       // m_totalSent 旧值, 仅当 m_lastAckedSentTime 是有效值时有效

    uint64_t    m_lastAckedSentTime{0};     // 上一个被确认包的发送时间
    uint64_t    m_lastAckedPacketTime{0};   // 上一次ACK包接收的时间
    uint64_t    m_lastSentPackNo{0};        // 最近一次发出的包号
    uint64_t    m_endOfAppLimitedPhase{0};  // app-limited状态结束时点的包号

    Malo<BWPacketState>     m_samplePool;      // 带宽采样包状态池
    std::list<PacketInfo>   m_inflightPackets; // 未确认包信息列表
};

} // namespace utp
} // namespace eular

#endif // __CONGESTION_BW_SAMPLER_H__
