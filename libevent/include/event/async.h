/*************************************************************************
    > File Name: async.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 10时31分04秒
 ************************************************************************/

#ifndef __LIBEVENT_EVENT_ASYNC_H__
#define __LIBEVENT_EVENT_ASYNC_H__

#include <string>
#include <functional>
#include <mutex>
#include <unordered_map>

#include <event/base.h>
#include <event/loop.h>

namespace ev {
class EventAsync
{
    DISALLOW_COPY_AND_ASSIGN(EventAsync);
    DISALLOW_MOVE(EventAsync);

public:
    using AsyncCallback = std::function<void(const std::string &)>;

    using SP = std::shared_ptr<EventAsync>;
    using WP = std::weak_ptr<EventAsync>;
    using Ptr = std::unique_ptr<EventAsync>;

    EventAsync(EventLoop::SP loop);
    EventAsync(const EventLoop *loop);
    EventAsync(event_base *base);
    ~EventAsync();

    bool start() noexcept;

    void stop() noexcept;

    /**
     * @brief 调用 start() 后再调用此函数会失败, 需要先 stop() 后调用
     * 
     * @param key 键
     * @param cb 回调 参数为键名
     * @return true 成功
     * @return false 失败
     */
    bool addAsync(const std::string &key, AsyncCallback cb);

    /**
     * @brief 删除指定的异步回调 ⚠️：加锁, 异步线程会等待锁释放
     * 
     * @param key 键
     */
    void delAsync(const std::string &key);

    bool notify(const std::string &key) noexcept;

    void reset(event_base *loop = nullptr);

private:
    event*          m_event = nullptr;
#if defined(_WIN32) || defined(_WIN64)
    intptr_t        m_sockPair[2] = { -1, -1 };
#else
    int32_t         m_sockPair[2] = {-1, -1};
#endif
    bool            m_started = false;
    std::mutex      m_mapMtx;
    std::unordered_map<std::string, AsyncCallback> m_asyncMap;
};
} // namespace ev

#endif // __LIBEVENT_EVENT_ASYNC_H__
