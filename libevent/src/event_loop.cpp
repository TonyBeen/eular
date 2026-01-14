/*************************************************************************
    > File Name: event_loop.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 11时02分49秒
 ************************************************************************/

#include "event/loop.h"

#include <assert.h>
#include <exception>
#include <stdexcept>
#include <string>
#include <memory>

#include <event2/event.h>

namespace ev {
EventLoop::EventLoop() :
    m_eventBase(nullptr)
{
    event_config *pConfig = event_config_new();
    int32_t flags = EVENT_BASE_FLAG_IGNORE_ENV | EVENT_BASE_FLAG_PRECISE_TIMER;
    event_config_set_flag(pConfig, flags);
    m_eventBase = event_base_new_with_config(pConfig);
    if (pConfig) {
        event_config_free(pConfig);
    }
}

EventLoop::~EventLoop()
{
    reset();
}

EventLoop::EventLoop(EventLoop &&other)
{
    std::swap(this->m_eventBase, other.m_eventBase);
}

EventLoop &EventLoop::operator=(EventLoop &&other)
{
    if (std::addressof(other) != this) {
        std::swap(this->m_eventBase, other.m_eventBase);
    }

    return *this;
}

int32_t EventLoop::dispatch(uint32_t flags)
{
    assert(m_eventBase);
    if (!m_eventBase) {
        return -1;
    }

    return event_base_loop(m_eventBase, static_cast<int32_t>(flags));
}

void EventLoop::breakLoop()
{
    if (!m_eventBase) {
        return;
    }

    int32_t code = event_base_loopbreak(m_eventBase);
    assert(0 == code);
    if (code != 0) {
        throw std::runtime_error("event_base_loopbreak return " + std::to_string(code));
    }
}

void EventLoop::exitLoop(uint64_t timeout)
{
    if (!m_eventBase) {
        return;
    }

    struct timeval tv;
    struct timeval *ptr = nullptr;
    if (timeout > 0) {
        using sec_t = decltype(tv.tv_sec);
        using usec_t = decltype(tv.tv_usec);
        tv.tv_sec = sec_t(timeout / 1000);
        tv.tv_usec = usec_t((timeout % 1000) * 1000);
        ptr = &tv;
    }

    int32_t code = event_base_loopexit(m_eventBase, ptr);
    assert(0 == code);
    if (code != 0) {
        throw std::runtime_error("event_base_loopexit return " + std::to_string(code));
    }
}

void EventLoop::reset()
{
    if (m_eventBase != nullptr) {
        event_base_free(m_eventBase);
        m_eventBase = nullptr;
    }
}

} // namespace event