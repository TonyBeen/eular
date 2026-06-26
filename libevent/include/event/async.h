/*************************************************************************
    > File Name: async.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 10时31分04秒
 ************************************************************************/

#ifndef __LIBEVENT_EVENT_ASYNC_H__
#define __LIBEVENT_EVENT_ASYNC_H__

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <event/base.h>
#include <event/loop.h>

namespace ev {
class EVENT_WRAPPER_API EventAsync
{
    DISALLOW_COPY_AND_ASSIGN(EventAsync);
    DISALLOW_MOVE(EventAsync);

public:
    using AsyncId = uint32_t;
    using AsyncCallback = std::function<void(AsyncId)>;
    enum class AsyncMode : uint8_t {
        Normal = 0,
        StrictIsolation = 1,
    };

    struct AsyncSlot {
        AsyncMode       mode = AsyncMode::Normal;
        uint32_t        generation = 0;
        AsyncCallback   cb;
    };

    struct AsyncToken {
        AsyncId     id = 0;
        uint32_t    generation = 0;
    };

    using SP  = std::shared_ptr<EventAsync>;
    using WP  = std::weak_ptr<EventAsync>;
    using Ptr = std::unique_ptr<EventAsync>;

    EventAsync(EventLoop::SP loop);
    EventAsync(const EventLoop *loop);
    EventAsync(event_base *base);
    ~EventAsync();

    bool start() noexcept;

    void stop() noexcept;

    /**
     * @brief 添加异步回调（线程安全）
     *
     * @param id 回调ID
     * @param cb 回调 参数为ID
     * @param mode 普通模式下不校验历史代次，严格模式下隔离旧通知
     * @return true 成功
     * @return false 失败（空回调或重复ID）
     */
    bool addAsync(AsyncId id, AsyncCallback cb, AsyncMode mode = AsyncMode::Normal);

    /**
     * @brief 删除指定的异步回调 ⚠️：加锁, 异步线程会等待锁释放
     *
     * @param id 回调ID
     */
    void delAsync(AsyncId id);

    bool notify(AsyncId id) noexcept;

    void reset(event_base *loop = nullptr);

private:
    using AsyncMap = std::unordered_map<AsyncId, AsyncSlot>;

    event*          m_event = nullptr;
#if defined(_WIN32) || defined(_WIN64)
    intptr_t        m_sockPair[2] = { -1, -1 };
#else
    int32_t         m_sockPair[2] = {-1, -1};
#endif
    bool            m_started = false;
    std::mutex      m_mapMtx;
    uint32_t        m_nextGeneration = 1;
    std::shared_ptr<const AsyncMap> m_asyncSnapshot;
    std::vector<uint8_t> m_recvBuffer;
};
} // namespace ev

#endif // __LIBEVENT_EVENT_ASYNC_H__
