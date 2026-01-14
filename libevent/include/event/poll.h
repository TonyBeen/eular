/*************************************************************************
    > File Name: poll.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年10月30日 星期三 10时00分08秒
 ************************************************************************/

#ifndef __LIBEVENT_EVENT_POLL_H__
#define __LIBEVENT_EVENT_POLL_H__

#include <functional>

#include <event/base.h>
#include <event/loop.h>

namespace ev {
class EventPoll
{
    DISALLOW_COPY_AND_ASSIGN(EventPoll);
public:
    enum Event {
        None = 0,

        Read = 0b0001,
        Write = 0b0010,

        // 以下内容不可与上面混用, 将返回错误
        ReadOnce = 0b0100,
        WriteOnce = 0b1000,

        // 边缘触发, libevent默认水平触发
        EdgeTrigger = 0b10000,
    };
    using event_t = uint32_t;
    using EventCB = std::function<void(socket_t, event_t)>;

    using SP = std::shared_ptr<EventPoll>;
    using WP = std::weak_ptr<EventPoll>;
    using Ptr = std::unique_ptr<EventPoll>;

    EventPoll();
    ~EventPoll();

    /**
     * @brief 重置事件
     * 
     * @param loop 事件循环指针
     * @param sock 套接字
     * @param flag 事件标志
     * @param cb 事件回调
     * @return int32_t 成功返回0, 失败返回负值
     */
    int32_t reset(EventLoop *loop = nullptr, socket_t sock = INVALID_SOCKET,
                  event_t flag = Event::None, EventCB cb = nullptr);

    bool start();
    void stop();
    bool hasPending() const;

private:
    static uint32_t EventParse(event_t eventFlag);
    void clean();

private:
    EventCB     m_cb;
    event*      m_ev = nullptr;
};

} // namespace ev

#endif // __LIBEVENT_EVENT_POLL_H__
