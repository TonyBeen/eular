/*************************************************************************
    > File Name: event_timer.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 16时06分37秒
 ************************************************************************/

#include "event/timer.h"

#include <event2/event.h>

namespace ev {
EventTimer::~EventTimer()
{
    reset();
}

bool EventTimer::reset(EventLoop *loop, TimerCB cb) noexcept
{
    if (loop == nullptr && cb == nullptr) {
        stop();
        if (m_event) {
            event_free(m_event);
            m_event = nullptr;
        }
        return true;
    }

    if (loop == nullptr || loop->loop() == nullptr) {
        return false;
    }

    if (cb == nullptr) {
        return false;
    }

    stop();
    if (m_event) {
        event_free(m_event);
        m_event = nullptr;
    }

    m_cb = cb;
    m_event = event_new(loop->loop(), -1, EV_TIMEOUT, [](evutil_socket_t, short, void* data) {
        auto self = static_cast<EventTimer *>(data);
        if (self->m_repeat != 0) {
            self->addTimerEvent(self->m_repeat);
        }

        self->m_cb();
    }, this);

    return m_event != nullptr;
}

bool EventTimer::start(uint64_t timeout, uint64_t repeat) noexcept
{
    m_repeat = repeat;
    return addTimerEvent(timeout);
}

void EventTimer::stop() noexcept
{
    if (m_event != nullptr) {
        event_del(m_event);
    }
}

bool EventTimer::addTimerEvent(uint64_t timeout)
{
    if (m_event == nullptr) {
        return false;
    }

    if (timeout == 0) {
        event_active(m_event, 0, 0);
        return 0;
    }

    struct timeval tv{};
    using sec_t = decltype(tv.tv_sec);
    using usec_t = decltype(tv.tv_usec);
    tv.tv_sec = sec_t(timeout / 1000);
    tv.tv_usec = usec_t((timeout % 1000) * 1000);
    return 0 == event_add(m_event, &tv);
}

} // namespace ev
