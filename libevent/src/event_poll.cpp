/*************************************************************************
    > File Name: event_poll.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年10月30日 星期三 10时00分15秒
 ************************************************************************/

#include "event/event_poll.h"

#include <errno.h>
#include <string.h>

#include <event2/event.h>

namespace ev {
EventPoll::EventPoll()
{
}

EventPoll::~EventPoll()
{
    clean();
}

int32_t EventPoll::reset(EventLoop *loop, socket_t sock, event_t flag, EventCB cb)
{
    if (loop == nullptr && cb == nullptr) {
        clean();
        return 0;
    }

    if (loop == nullptr || sock == INVALID_SOCKET || cb == nullptr) {
        errno = EINVAL;
        return -1;
    }

    uint32_t eventFlag = EventParse(flag);
    if (eventFlag == static_cast<uint32_t>(Event::None)) {
        errno = EINVAL;
        return -1;
    }

    clean();

    m_cb = std::move(cb);
    m_ev = event_new(loop->loop(), sock, (short)eventFlag, [] (evutil_socket_t sock, short flag, void *ptr) {
        EventPoll *self = static_cast<EventPoll *>(ptr);
        event_t evFlag = Event::None;
        if (flag & EV_READ) {
            evFlag |= Event::Read;
        }
        if (flag & EV_WRITE) {
            evFlag |= Event::Write;
        }
        self->m_cb(sock, evFlag);
    }, this);

    return m_ev != nullptr ? 0 : -2;
}

bool EventPoll::start()
{
    if (m_ev == nullptr) {
        return false;
    }

    return event_add(m_ev, nullptr) == 0;
}

void EventPoll::stop()
{
    if (m_ev != nullptr) {
        event_del(m_ev);
    }
}

bool EventPoll::hasPending() const
{
    if (m_ev == nullptr) {
        return false;
    }

    auto flags = event_pending(m_ev, EV_READ | EV_WRITE, nullptr);
    return flags != 0;
}

uint32_t EventPoll::EventParse(event_t eventFlag)
{
    uint32_t persistEventMask = static_cast<uint32_t>(Event::Read | Event::Write);
    uint32_t onceEventMask = static_cast<uint32_t>(Event::ReadOnce | Event::WriteOnce);

    uint32_t flag = static_cast<uint32_t>(eventFlag);
    uint32_t event = static_cast<uint32_t>(Event::None);

    bool isPersist = (flag & persistEventMask) && !(flag & onceEventMask);
    if (isPersist) {
        event |= EV_PERSIST;

        if (flag & Event::Read) {
            event |= EV_READ;
        }

        if (flag & Event::Write) {
            event |= EV_WRITE;
        }
    }

    bool isOnce = (flag & onceEventMask) && !(flag & persistEventMask);
    if (isOnce) {
        if (flag & Event::ReadOnce) {
            event |= EV_READ;
        }

        if (flag & Event::WriteOnce) {
            event |= EV_WRITE;
        }
    }

    if (flag & Event::EdgeTrigger) {
        event |= EV_ET;
    }

    return event;
}

void EventPoll::clean()
{
    m_cb = nullptr;
    if (m_ev) {
        event_free(m_ev);
        m_ev = nullptr;
    }
}

} // namespace ev
