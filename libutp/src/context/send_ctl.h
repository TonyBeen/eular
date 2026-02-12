/*************************************************************************
    > File Name: send_ctl.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 10:42:13 AM CST
 ************************************************************************/

#ifndef __UTP_CONTEXT_SEND_CTL_H__
#define __UTP_CONTEXT_SEND_CTL_H__

#include "context/context_impl.h"
#include "context/connection_impl.h"

#include "congestion/congestion.h"

#include "proto/packet_out.h"
#include "util/send_history.h"

namespace eular {
namespace utp {
enum class SendCtlFlags : uint32_t {
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

inline SendCtlFlags operator|(SendCtlFlags a, SendCtlFlags b) {
    return static_cast<SendCtlFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SendCtlFlags& operator|=(SendCtlFlags& a, SendCtlFlags b) {
    a = a | b;
    return a;
}

inline bool operator&(SendCtlFlags a, SendCtlFlags b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

enum class BufPacketType : uint8_t {
    HighestPrio = 0, // 高优先级 (ACK/PING)
    OtherPrio = 1    // 普通优先级 (STREAM)
};

class SendControl
{
public:
    SendControl(ConnectionImpl *conn, ContextImpl *ctx);
    ~SendControl() = default;


public:
    bool canSend() const;
    uint64_t bytesOutTotal() const;

private:
    ContextImpl*        m_ctx{};
    ConnectionImpl*     m_conn{};
    SendHistory         m_sendHistory{};                // 发送历史记录
    SendCtlFlags        m_flags{SendCtlFlags::None};    // 发送控制标志
    PacketOutTailQ      m_unackedPackets{};             // 待确认的包队列
    utp_packno_t        m_largestAckedPackNo{0};        // 已确认的最大包号
    utp_time_t          m_lastSentTime{0};              // 上次包发送的时间
    utp_time_t          m_lastRtoTime{0};               // 最后一次重传超时的时间
    Congestion::SP      m_congestion{};                 // 拥塞控制算法实例

    /// @b 拥塞控制相关
    uint64_t            m_bytesUnackedRetrans{0};       // 重传未确认的字节数
    uint64_t            m_bytesScheduled{0};            // 已调度但未发送的字节数

    /// @b 统计相关
    uint64_t            m_bytesUnackedAll;              // 所有未确认的字节数 (包括重传和非重传)
    uint64_t            m_nInflightAll;                 // 所有在飞行中的数据包数量 (包括重传和非重传)
    uint64_t            m_nInflightRetrans;             // 飞行中重传包的数量

    /// @b 速率估计相关
    uint32_t            m_nConsecRtos;                  // 连续 RTO 次数
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONTEXT_SEND_CTL_H__
