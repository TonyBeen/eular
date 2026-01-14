/*************************************************************************
    > File Name: event_async.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 14时29分20秒
 ************************************************************************/

#include "event/async.h"
#include <assert.h>
#include <event2/event.h>

#define SOCK_PAIR_RECV 0
#define SOCK_PAIR_SEND 1

namespace ev {
EventAsync::EventAsync(EventLoop::SP loop)
{
    m_asyncMap.reserve(32);
    reset(loop.get());
}

EventAsync::EventAsync(const EventLoop *loop)
{
    m_asyncMap.reserve(32);
    reset(loop);
}

EventAsync::~EventAsync()
{
    reset();
}

bool EventAsync::start() noexcept
{
    if (m_event == nullptr) {
        return false;
    }

    m_started = (0 == event_add(m_event, nullptr));
    return m_started;
}

void EventAsync::stop() noexcept
{
    if (m_event == nullptr) {
        return;
    }

    event_del(m_event);
}

bool EventAsync::addAsync(const std::string &name, AsyncCallback cb)
{
    if (name.empty() || cb == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mapMtx);
    auto ret = m_asyncMap.emplace(name, std::move(cb));
    return ret.second;
}

void EventAsync::delAsync(const std::string &key)
{
    std::lock_guard<std::mutex> lock(m_mapMtx);
    m_asyncMap.erase(key);
}

static inline bool WouldBlock() {
#if defined(_WIN32) || defined(_WIN64)
    int32_t code = WSAGetLastError();
    return code == WSAEWOULDBLOCK;
#else
    int32_t code = errno;
    return (code == EAGAIN || code == EWOULDBLOCK);
#endif
}

bool EventAsync::notify(const std::string &key) noexcept
{
    if (m_event == nullptr) {
        return false;
    }

    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mapMtx);
        auto it = m_asyncMap.find(key);
        found = it != m_asyncMap.end();
    }

    if (found) {
#if defined(MSG_NOSIGNAL)
        static const int kSendFlag = MSG_NOSIGNAL;
#else
        static const int kSendFlag = 0;
#endif
        auto sendSize = ::send(m_sockPair[SOCK_PAIR_SEND], key.c_str(), key.size() + 1, kSendFlag);
        if (sendSize > 0) {
            return true;
        } else if (sendSize == 0) { // 对端关闭
            return false;
        } else { // 发送缓存满了
            return WouldBlock() ? false : true;
        }
    }

    return found;
}

void EventAsync::reset(const EventLoop *loop)
{
    stop();
    if (m_event != nullptr) {
        event_free(m_event);
        m_event = nullptr;
    }

    if (m_sockPair[SOCK_PAIR_RECV] > 0) {
        evutil_closesocket(m_sockPair[SOCK_PAIR_RECV]);
        evutil_closesocket(m_sockPair[SOCK_PAIR_SEND]);
        m_sockPair[SOCK_PAIR_RECV] = -1;
        m_sockPair[SOCK_PAIR_SEND] = -1;
    }

    if (loop != nullptr && loop->loop() != nullptr) {
        // NOTE socketpair不支持AF_INET
        int32_t result = evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, (evutil_socket_t *)m_sockPair);
        assert(result == 0);
        if (result != 0) {
            return;
        }

        assert(0 == evutil_make_socket_nonblocking(m_sockPair[SOCK_PAIR_RECV]));
        assert(0 == evutil_make_socket_nonblocking(m_sockPair[SOCK_PAIR_SEND]));

        m_event = event_new(loop->loop(), m_sockPair[SOCK_PAIR_RECV], EV_READ | EV_PERSIST, [](evutil_socket_t, short, void *data) {
            auto self = static_cast<EventAsync *>(data);

            std::string eventDomain;
            eventDomain.reserve(1024);
            do {
                char buffer[1024];
                auto nRecv = ::recv(self->m_sockPair[SOCK_PAIR_RECV], buffer, sizeof(buffer), 0);
                if (nRecv > 0) {
                    eventDomain.append(buffer, nRecv);
                } else {
                    if (errno != EAGAIN) {
                        perror("recv error");
                    }
                    break;
                }
            } while (true);

            size_t pos = std::string::npos;
            size_t off = 0;

            while ((pos = eventDomain.find('\0', off)) != std::string::npos) {
                std::string cbName(eventDomain.c_str() + off, pos - off);

                std::lock_guard<std::mutex> lock(self->m_mapMtx);
                auto it = self->m_asyncMap.find(cbName);
                if (it != self->m_asyncMap.end()) {
                    it->second(cbName);
                }

                off = pos + 1;
            }
        }, this);
    }
}
} // namespace ev
