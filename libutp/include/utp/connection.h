/*************************************************************************
    > File Name: connection.h
    > Author: eular
    > Brief:
    > Created Time: Tue 13 Jan 2026 05:42:22 PM CST
 ************************************************************************/

#ifndef __UTP_CONNECTION_H__
#define __UTP_CONNECTION_H__

#include <stdint.h>
#include <string>
#include <memory>
#include <functional>
#include <vector>

#include <utp/platform.h>
#include <utp/stream.h>

namespace eular {
namespace utp {
class UTP_API Connection
{
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;
public:
    using Ptr = std::shared_ptr<Connection>;

    enum StreamType : uint8_t {
        kStreamTypeBidirectional = 0,
        kStreamTypeUnidirectional = 1,
        kStreamTypeAll = 0xFF,
    };

    struct ConnectionErrorInfo {
        int32_t     error_code{0};
        std::string error_reason;
        bool        fatal{false};
    };

    struct ConnectionCloseInfo {
        int32_t     error_code{0};
        std::string error_reason;
        bool        by_peer{false};
    };

    using OnIncomingStream      = std::function<void(Stream *)>;
    using OnSessionTokenReady   = std::function<void()>;
    using OnError               = std::function<void(const ConnectionErrorInfo &)>;
    using OnClosed              = std::function<void(const ConnectionCloseInfo &)>;

    struct Description {
        uint32_t        scid;
        uint32_t        dcid;
        std::string     remoteHost;
        uint16_t        remotePort;
    };

    struct Statistic {
        uint32_t        pmtu;
        uint32_t        rtt;
        uint32_t        rttvar;
        uint32_t        bw_estimate;
        uint64_t        rx_bytes;
        uint64_t        tx_bytes;
        uint64_t        rtx_bytes;

        // stream scheduler metrics
        uint64_t        scheduler_select_total;          // 总选流次数
        uint64_t        scheduler_select_disabled;       // DISABLED 模式选流次数
        uint64_t        scheduler_select_strict;         // STRICT 模式选流次数
        uint64_t        scheduler_select_drr;            // DRR 模式选流次数
        uint64_t        scheduler_strict_aging_promoted; // STRICT 中 aging 提升次数
        uint64_t        scheduler_would_block;           // 选中流发送返回 WOULD_BLOCK 次数
        uint64_t        scheduler_empty_rounds;          // flush 未选到可发流次数
        uint64_t        scheduler_mode_switches;         // 调度模式热切换次数
        uint64_t        scheduler_drr_refills;           // DRR deficit 补充次数
        uint64_t        scheduler_drr_consumes;          // DRR deficit 消耗次数
    };

    Connection() = default;
    virtual ~Connection() = default;

    virtual void        setOnIncomingStream(const OnIncomingStream &cb) = 0;
    virtual void        setOnSessionTokenReady(const OnSessionTokenReady &cb) = 0;
    virtual void        setOnError(const OnError &cb) = 0;
    virtual void        setOnClosed(const OnClosed &cb) = 0;
    virtual int32_t     streamCount(StreamType streamType = kStreamTypeAll) const = 0;
    virtual int32_t     creatableStreamCount(StreamType streamType) const = 0;
    virtual Statistic   statistic() const = 0;
    virtual Description description() const = 0;
    virtual int32_t     exportSessionToken(std::vector<uint8_t> &outToken) = 0;
    virtual int32_t     exportSessionResumptionState(std::string &outState) = 0;

    virtual int32_t     createStream(StreamType streamType = kStreamTypeBidirectional) = 0;
    virtual Stream*     getStream(uint32_t streamId) = 0;
    virtual void        close() = 0;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONNECTION_H__
