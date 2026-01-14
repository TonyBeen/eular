/*************************************************************************
    > File Name: event_loop.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 10时50分29秒
 ************************************************************************/

#ifndef __EVENT_LOOP_H__
#define __EVENT_LOOP_H__

#include <memory>
#include <event/event_base.h>

namespace ev {
class EventLoop
{
    DISALLOW_COPY_AND_ASSIGN(EventLoop);
public:
    using SP = std::shared_ptr<EventLoop>;
    using WP = std::weak_ptr<EventLoop>;
    using Ptr = std::unique_ptr<EventLoop>;

    EventLoop();
    ~EventLoop();

    EventLoop(EventLoop &&other);
    EventLoop &operator=(EventLoop &&other);

    /**
     * @brief event loop begin
     * 
     * @param flags EVLOOP_ONCE | EVLOOP_NONBLOCK | EVLOOP_NO_EXIT_ON_EMPTY
     * @return int32_t 小于0失败
     */
    int32_t dispatch(uint32_t flags = 0);

    void breakLoop();

    void exitLoop(uint64_t timeout = 0);

    void reset();

    event_base *loop() const { return m_eventBase; }

private:
    event_base *m_eventBase;
};

} // namespace event

#endif // __EVENT_LOOP_H__
