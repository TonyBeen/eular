/*************************************************************************
    > File Name: single_async.h
    > Author: Codex
    > Brief: 单回调异步通知器
 ************************************************************************/

#ifndef __LIBEVENT_SINGLE_ASYNC_H__
#define __LIBEVENT_SINGLE_ASYNC_H__

#include <functional>
#include <memory>

#include <event/base.h>
#include <event/loop.h>

namespace ev {
class EVENT_WRAPPER_API SingleAsync
{
    DISALLOW_COPY_AND_ASSIGN(SingleAsync);
    DISALLOW_MOVE(SingleAsync);

public:
    using AsyncCallback = std::function<void()>;

    using SP = std::shared_ptr<SingleAsync>;
    using WP = std::weak_ptr<SingleAsync>;
    using Ptr = std::unique_ptr<SingleAsync>;

    SingleAsync(EventLoop::SP loop, AsyncCallback cb);
    SingleAsync(const EventLoop *loop, AsyncCallback cb);
    SingleAsync(event_base *base, AsyncCallback cb);
    ~SingleAsync();

    bool start() noexcept;
    void stop() noexcept;

    // 只负责把一次通知投递到异步通道，不保证回调一定被消费。
    bool notify() noexcept;

    void reset(event_base *loop = nullptr);

private:
    event*          m_event = nullptr;
#if defined(_WIN32) || defined(_WIN64)
    intptr_t        m_sockPair[2] = { -1, -1 };
#else
    int32_t         m_sockPair[2] = {-1, -1};
#endif
    bool            m_started = false;
    AsyncCallback   m_cb;
};
} // namespace ev

#endif // __LIBEVENT_SINGLE_ASYNC_H__
