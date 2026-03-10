/*************************************************************************
    > File Name: pacer.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 04:50:37 PM CST
 ************************************************************************/

#include "congestion/pacer.h"

#include <string.h>
#include <assert.h>

#include "logger/logger.h"

void eular::utp::Pacer::init(uint32_t clock_granularity)
{
    memset(this, 0, sizeof(Pacer));
    _clock_granularity = clock_granularity;
    _burst_tokens = 10;
}

void eular::utp::Pacer::cleanup()
{
}

void eular::utp::Pacer::tickIn(utp_time_t now)
{
    assert(now >= _now);
    _now = now;
    if (_flags & LAST_SCHED_DELAYED) {
        _flags |= DELAYED_ON_TICK_IN;
    }
    _n_scheduled = 0;
}

void eular::utp::Pacer::tickOut(utp_time_t now)
{
    if ((_flags & DELAYED_ON_TICK_IN) && _n_scheduled == 0 && _now > _next_sched) {
        UTP_LOGD("tick passed without scheduled packets: reset delayed flag");
        _flags &= ~LAST_SCHED_DELAYED;
    }

    _flags &= ~DELAYED_ON_TICK_IN;
}

bool eular::utp::Pacer::canSchedule(uint64_t inflight)
{
    bool can = false;
    if (_burst_tokens > 0 || inflight == 0) {
        can = true;
    } else if (_next_sched > _now + _clock_granularity) {
        _flags |= LAST_SCHED_DELAYED;
    } else {
        can = true;
    }

    return can;
}

void eular::utp::Pacer::packetScheduled(uint64_t inflight, bool inRecovery, TxTime txcb)
{
    utp_time_t delay;
    utp_time_t schedTime;
    bool appLimited;
    bool makingUp;

    ++_n_scheduled;

    if (inflight == 0 && !inRecovery) {
        _burst_tokens = 10;
        UTP_LOGD("replenish tokens: %u", _burst_tokens);
    }

    if (_burst_tokens > 0) {
        --_burst_tokens;
        _flags &= ~LAST_SCHED_DELAYED;
        _next_sched = 0;
        _last_delayed = 0;
        UTP_LOGD("burst token: %u left", _burst_tokens);
        return;
    }

    schedTime = _now;
    delay = txcb();
    if (_flags & LAST_SCHED_DELAYED) {
        _next_sched += delay;
        appLimited = _last_delayed != 0 && (_last_delayed + delay) <= schedTime;
        makingUp = _next_sched <= schedTime;
        UTP_LOGD("delayed schedule: delay=%u, next_sched=%u, appLimited=%d, makingUp=%d", delay, _next_sched, appLimited, makingUp);
        if (appLimited || makingUp) {
            _last_delayed = schedTime;
        } else {
            _flags &= ~LAST_SCHED_DELAYED;
            _last_delayed = 0;
        }
    } else {
        _next_sched = std::max(_next_sched + delay, schedTime + delay);
    }

    UTP_LOGD("next_sched is set to %"PRIu64" usec from now", _next_sched - _now);
}

void eular::utp::Pacer::lossEvent()
{
    _burst_tokens = 0;
}

bool eular::utp::Pacer::canScheduleProbe(uint64_t inflight, utp_time_t txTime)
{
    return _burst_tokens > 1 || inflight == 0 || _next_sched > _now + txTime / 2;
}
