/*************************************************************************
    > File Name: timer.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 16时06分25秒
 ************************************************************************/

#ifndef __EVENT_TIMER_H__
#define __EVENT_TIMER_H__

#include <functional>

#include <event/base.h>
#include <event/loop.h>

namespace ev {
class EventTimer
{
    DISALLOW_COPY_AND_ASSIGN(EventTimer);
    DISALLOW_MOVE(EventTimer);
public:
    using TimerCB = std::function<void()>;

    using SP = std::shared_ptr<EventTimer>;
    using WP = std::weak_ptr<EventTimer>;
    using Ptr = std::unique_ptr<EventTimer>;

    EventTimer() noexcept = default;
    ~EventTimer();

    bool reset(EventLoop *loop = nullptr, TimerCB cb = nullptr) noexcept;

    bool start(uint64_t timeout, uint64_t repeat = 0) noexcept;

    void stop() noexcept;

protected:
    bool addTimerEvent(uint64_t timeout);

private:
    TimerCB     m_cb;
    event*      m_event = nullptr;
    uint64_t    m_repeat = 0;
};

} // namespace ev

#endif // __EVENT_TIMER_H__
