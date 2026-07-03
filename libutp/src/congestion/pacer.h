/*************************************************************************
    > File Name: pacer.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 04:50:34 PM CST
 ************************************************************************/

#ifndef __UTP_CONGESTION_PACER_H__
#define __UTP_CONGESTION_PACER_H__

#include <functional>

#include <utp/types.h>

namespace eular {
namespace utp {
struct Pacer {
public:
    enum {
        LAST_SCHED_DELAYED = (1 << 0),  // 上次调度被延迟
        DELAYED_ON_TICK_IN = (1 << 1),  // 在 tick_in 时被延迟
    };
    using TxTime = std::function<utp_time_t()>;

    Pacer() = default;
    ~Pacer() = default;

    void init(uint32_t clock_granularity);
    void cleanup();
    void tickIn(utp_time_t now);
    void tickOut(utp_time_t now);
    bool canSchedule(uint64_t inflight);
    void packetScheduled(uint64_t inflight, bool inRecovery, TxTime txcb);
    void lossEvent();
    bool delayed() const { return _flags & LAST_SCHED_DELAYED; }
    utp_time_t nextSched() const { return _next_sched; }
    bool canScheduleProbe(uint64_t inflight, utp_time_t txTime);

private:
    utp_time_t  _next_sched;    // 下一次调度时间(us)
    utp_time_t  _last_delayed;  // 上次延迟的时间(us)
    utp_time_t  _now;           // 当前时间(us)

    uint32_t    _clock_granularity; // 时钟粒度 (us)
    uint32_t    _burst_tokens;      // 突发令牌数
    uint32_t    _n_scheduled;       // 单个时钟周期内已调度的数据包数
    uint16_t    _flags;
};

} // namespace utp
} // namespace eular

#endif // __UTP_CONGESTION_PACER_H__
