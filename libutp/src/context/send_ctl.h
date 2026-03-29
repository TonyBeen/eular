/*************************************************************************
    > File Name: send_ctl.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 10:42:13 AM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_SEND_CTL_H__
#define __UTP_CONTEXT_SEND_CTL_H__

#include <string>
#include <deque>

#include <queue.h>

#include <event/timer.h>

#include "congestion/congestion.h"
#include "congestion/pacer.h"

#include "proto/packet_common.h"
#include "proto/packet_out.h"
#include "util/send_history.h"
#include "util/enum.hpp"

enum SendCtlFlags : uint32_t {
    None            = 0,
    Pace            = 1 << 0,   // 启用 Pacer
    SchedTick       = 1 << 1,   // 调度 tick
    WasQuiet        = 1 << 2,   // 刚从静默期恢复
    AppLimited      = 1 << 3,   // 应用层限速
    EcnEnabled      = 1 << 4,   // ECN 启用
    SanityCheck     = 1 << 5,   // 调试模式
    RoughRtt        = 1 << 6,   // 使用粗略 RTT
    LostAckInit     = 1 << 8,   // Initial 空间丢失 ACK
    LostAckHsk      = 1 << 9,   // Handshake 空间丢失 ACK
    LostAckApp      = 1 << 10,  // Application 空间丢失 ACK
    OneRttAcked     = 1 << 11,  // 1-RTT 包已确认
};

ENUM_UTIL_ENABLE_BITMASK(SendCtlFlags);

#define FLAG_ENUM_ITEMS(X)          \
    X(SendCtlFlags, None)           \
    X(SendCtlFlags, Pace)           \
    X(SendCtlFlags, SchedTick)      \
    X(SendCtlFlags, WasQuiet)       \
    X(SendCtlFlags, AppLimited)     \
    X(SendCtlFlags, EcnEnabled)     \
    X(SendCtlFlags, SanityCheck)    \
    X(SendCtlFlags, RoughRtt)       \
    X(SendCtlFlags, LostAckInit)    \
    X(SendCtlFlags, LostAckHsk)     \
    X(SendCtlFlags, LostAckApp)     \
    X(SendCtlFlags, OneRttAcked)    \

ENUM_UTIL_REGISTER_ENUM(SendCtlFlags, FLAG_ENUM_ITEMS, 12)

enum class RetransmissionMode {
    kHandshake,  // 握手阶段重传模式, 优先重传握手包
    kLoss,       // 基于丢包的重传模式, 当检测到丢包时触发, 适用于网络状况较差的环境
    kTlp,        // 主动探测 机制, 在怀疑最后几个包丢失时发送探测包
    kRto         // 基于超时的重传模式, 当没有收到ACK且重传定时器到期时触发, 适用于网络状况极差或丢包率高的环境
};

#define MODE_ENUM_ITEMS(X)          \
    X(RetransmissionMode, kHandshake)\
    X(RetransmissionMode, kLoss)    \
    X(RetransmissionMode, kTlp)     \
    X(RetransmissionMode, kRto)     \

ENUM_UTIL_REGISTER_ENUM(RetransmissionMode, MODE_ENUM_ITEMS, 4)

enum class ExpireFilter {
    kAll,
    kHandshakeOnly,
    kLastOnly,
};

namespace eular {
namespace utp {
class ContextImpl;
class ConnectionImpl;
class AckInfo;

enum BufPacketType : uint8_t {
    HighestPrio = 0, // 高优先级
    OtherPrio = 1,   // 普通优先级
    PriorityCount,   // 优先级数量
};

struct BufPacketQueue {
    PacketOutTailQ  packets;
    uint32_t        count{0};
};

class SendControl
{
public:
    SendControl(ConnectionImpl *conn, ContextImpl *ctx);
    ~SendControl();

    void    init();

    /**
     * @brief pkt 已发送, 更新发送控制状态, 包括拥塞控制、重传计数、统计数据等
     *
     * @param pkt 已发送的包
     * @return int32_t always 0
     */
    int32_t     packetSent(PacketOut *pkt);

public:
    bool        canSend();
    uint64_t    bytesOutTotal() const;
    uint64_t    bandwidthEstimate() const;
    uint64_t    retransmittedBytes() const;
    int32_t     onAckReceived(const AckInfo &ackInfo, utp_time_t nowUs);
    void        onCanWrite(utp_time_t nowUs);
    void        setReorderThreshold(uint32_t threshold);
    bool        isLossFrequent(utp_time_t nowUs, utp_time_t windowUs, uint32_t threshold) const;

private:
    bool        haveUnackedHandshakePackets() const;
    int32_t     expireUnacked(ExpireFilter filter, utp_time_t nowUs);
    void        onLossEvent();
    void        recordLossSignal(utp_time_t nowUs);

    void        appendUnacked(PacketOut *pkt);
    void        retransAlarm(utp_time_t now);
    RetransmissionMode getRetransmissionMode();
    void        onRetransTimer();

private:
    utp_time_t  calculateHandshakeDelay();
    /// @brief 计算 TLP 探测的延迟发送时间 (us)
    utp_time_t  calculateTlpDelay() const;
    utp_time_t  calculatePacketRto() const;

    bool        detectLosses(utp_time_t now);
    utp_packno_t largestRetxPacketNo() const;
    PacketOut*  handleRegularLostPacket(PacketOut *pkt, PacketOut *&next);
    bool        handleLostMtuProbe(PacketOut *pkt);
    int32_t     retransmitLostPacket(PacketOut *pkt, utp_time_t nowUs);
    void        unackedRemove(PacketOut *pkt);

    void        destroyPacket(PacketOut *pkt);

private:
    std::string         m_tag;
    ev::EventTimer      m_retransTimer;                 // 重传定时器, 用于触发基于时间的重传 (如 RTO)
    ContextImpl*        m_ctx{};
    ConnectionImpl*     m_conn{};
    SendHistory         m_sendHistory{};                // 发送历史记录
    SendCtlFlags        m_flags{SendCtlFlags::None};    // 发送控制标志
    PacketOutTailQ      m_unackedPackets{};             // 待确认的包队列
    utp_packno_t        m_largestAckedPackNo{0};        // 对端确认的最大包号 (用于丢包检测)
    utp_time_t          m_largestAckedSentTime{0};      // 最大已确认包的发送时间 (用于 RTT 采样)
    utp_time_t          m_lastSentTime{0};              // 最后一次发包时间 (用于空闲检测)
    utp_time_t          m_lastRtoTime{0};               // 最后一次 RTO 发生时间 (用于 RTO 重置判断)
    Congestion::SP      m_congestion{};                 // 拥塞控制算法实例

    /// @b 拥塞控制
    uint64_t            m_bytesUnackedRetrans{0};       // 重传未确认的字节数
    uint64_t            m_bytesScheduled{0};            // 已调度但未发送的字节数

    /// @b 统计相关
    uint64_t            m_bytesUnackedAll;              // 所有未确认的字节数 (包括重传和非重传)
    uint64_t            m_nInflightAll;                 // 飞行中的数据包总量, 包括一些非可重传包 (如 Ack-Only 包)
    uint64_t            m_nInflightRetrans;             // 飞行中可重传包的数量
    uint64_t            m_bytesRetransTotal{0};         // 累计重传字节数

    /// @b 重传计数
    uint32_t            m_nConsecRtos;                  // 连续 RTO 次数, 用于计算RTO时指数退避
    uint32_t            m_nHandshake;                   // Handshake 包重传次数
    uint32_t            m_nTlp;                         // TLP (Tail Loss Probe) 探测次数

    /// @b 可重传帧类型掩码
    PacketFrameTypeBit  m_retxFrames;                   // 可重传的帧类型掩码, 用于控制哪些类型的帧可以被重传

    /// @b 数据包队列
    PacketOutTailQ      m_scheduledPackets;             // 已调度待发送队列 (FIFO)
    PacketOutTailQ      m_lostPackets;                  // 检测到丢失的包队列 (待重传)

    /// @b 缓冲队列
    BufPacketQueue      m_bufferedPackets[PriorityCount];   // 按优先级分的缓冲包队列

    /// @b 速率控制
    Pacer               m_pacer;                        // 速率控制器实例, 平滑突发流量

    /// @b 包号管理
    utp_packno_t        m_currentPackNo{0};             // 当前包号, 每发送一个包递增
    utp_packno_t        m_largestSentAtCutback{0};      // 拥塞减窗时的最大已发包号, 用于判断是否是新的丢包事件
    utp_packno_t        m_maxRttPackNo{0};              // 最后一次 RTT 采样的包号, 避免重复采样同一包

    utp_packno_t        m_largestAck2ed{0};             // 对端已确认收到我方 ACK 的最大包号, 用于裁剪接收历史. (Ack帧与stream或ping帧一起传输时会被Ack)
    utp_time_t          m_lossTo;                       // 丢包超时时间 (早期重传触发时间), 非 0 时触发 kLoss

    /// @b 缓冲包类型缓存(性能优化)
    struct {
        uint32_t        _streamId;
        BufPacketType   _packetType;
    } m_cachedBpt;                                      // 避免重复调用 determine_bpt()

    uint32_t            m_nextLimit{0};                 // 下一次发送限制的时间点 (用于速率控制和空闲检测)
    uint32_t            m_nScheduled{0};                // 已调度但未发送的包数量 (队列长度)

#if defined(UTP_SEND_STATS)
    struct {
        uint32_t        _totalSent{0};                  // 总发送包数
        uint32_t        _retrans{0};                    // 重传包数
        uint32_t        _delayed{0};                    // 因速率控制而延迟发送的包数
    } m_stats;
#endif // UTP_SEND_STATS

    uint32_t            m_reorderThresh;                // 重排序阈值 (包号差值), 用于丢包检测中的重排序容忍度
    std::deque<utp_time_t> m_lossSignalsUs;             // 最近丢包信号时间戳，用于频率判定
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_SEND_CTL_H__
