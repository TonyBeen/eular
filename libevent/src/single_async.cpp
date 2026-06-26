/*************************************************************************
    > File Name: single_async.cpp
    > Author: Codex
    > Brief: 单回调异步通知器实现
 ************************************************************************/

#include "event/single_async.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <event2/event.h>

#define SOCK_PAIR_RECV 0
#define SOCK_PAIR_SEND 1

namespace ev {
namespace {
static inline bool WouldBlock()
{
#if defined(_WIN32) || defined(_WIN64)
    int32_t code = WSAGetLastError();
    return code == WSAEWOULDBLOCK;
#else
    int32_t code = errno;
    return (code == EAGAIN || code == EWOULDBLOCK);
#endif
}
} // namespace

SingleAsync::SingleAsync(EventLoop::SP loop, AsyncCallback cb) :
    m_cb(std::move(cb))
{
    reset(loop ? loop->loop() : nullptr);
}

SingleAsync::SingleAsync(const EventLoop *loop, AsyncCallback cb) :
    m_cb(std::move(cb))
{
    reset(loop ? loop->loop() : nullptr);
}

SingleAsync::SingleAsync(event_base *base, AsyncCallback cb) :
    m_cb(std::move(cb))
{
    reset(base);
}

SingleAsync::~SingleAsync()
{
    reset();
}

bool SingleAsync::start() noexcept
{
    if (m_event == nullptr) {
        return false;
    }

    m_started = (0 == event_add(m_event, nullptr));
    return m_started;
}

void SingleAsync::stop() noexcept
{
    if (m_event == nullptr) {
        m_started = false;
        return;
    }

    event_del(m_event);
    m_started = false;
}

bool SingleAsync::notify() noexcept
{
    if (m_event == nullptr) {
        return false;
    }

#if defined(MSG_NOSIGNAL)
    static const int kSendFlag = MSG_NOSIGNAL;
#else
    static const int kSendFlag = 0;
#endif

    const uint8_t payload = 1;
    auto sendSize = ::send(m_sockPair[SOCK_PAIR_SEND], reinterpret_cast<const char*>(&payload), 1, kSendFlag);
    return sendSize == 1;
}

void SingleAsync::reset(event_base *loop)
{
    stop();
    if (m_event != nullptr) {
        event_free(m_event);
        m_event = nullptr;
    }

    if (m_sockPair[SOCK_PAIR_RECV] >= 0) {
        evutil_closesocket(m_sockPair[SOCK_PAIR_RECV]);
        evutil_closesocket(m_sockPair[SOCK_PAIR_SEND]);
        m_sockPair[SOCK_PAIR_RECV] = -1;
        m_sockPair[SOCK_PAIR_SEND] = -1;
    }

    if (loop != nullptr) {
#if defined(_WIN32) || defined(_WIN64)
        int32_t result = evutil_socketpair(AF_INET, SOCK_STREAM, 0, m_sockPair);
#else
        int32_t result = evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, m_sockPair);
#endif
        assert(result == 0);
        if (result != 0) {
            return;
        }

        // 必须真实执行非阻塞设置, 不能只放在 assert() 中。
        // Release/NDEBUG 构建会移除 assert 参数求值, 导致 notify()/recv 在高并发下阻塞卡死。
        int32_t recvNonblock = evutil_make_socket_nonblocking(m_sockPair[SOCK_PAIR_RECV]);
        int32_t sendNonblock = evutil_make_socket_nonblocking(m_sockPair[SOCK_PAIR_SEND]);
        assert(0 == recvNonblock);
        assert(0 == sendNonblock);
        if (recvNonblock != 0 || sendNonblock != 0) {
            evutil_closesocket(m_sockPair[SOCK_PAIR_RECV]);
            evutil_closesocket(m_sockPair[SOCK_PAIR_SEND]);
            m_sockPair[SOCK_PAIR_RECV] = -1;
            m_sockPair[SOCK_PAIR_SEND] = -1;
            return;
        }

        m_event = event_new(loop, m_sockPair[SOCK_PAIR_RECV], EV_READ | EV_PERSIST, [](evutil_socket_t, short, void *data) {
            auto self = static_cast<SingleAsync *>(data);
            bool notified = false;

            // 单回调模型下只关心“本轮发生过通知”，因此把可读字节清空后只回调一次。
            for (;;) {
                uint8_t buffer[256];
                auto nRecv = ::recv(self->m_sockPair[SOCK_PAIR_RECV], reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
                if (nRecv > 0) {
                    notified = true;
                    continue;
                }

                if (!WouldBlock()) {
                    perror("recv error");
                }
                break;
            }

            if (notified && self->m_cb) {
                self->m_cb();
            }
        }, this);
    }
}
} // namespace ev
