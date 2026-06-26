/*************************************************************************
    > File Name: test_event_async.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年10月31日 星期四 09时48分13秒
 ************************************************************************/

#include <assert.h>
#include <atomic>
#include <iostream>
#include <thread>
#include <chrono>

#include <unistd.h>

#include <event/loop.h>
#include <event/async.h>

#include <event2/event.h>

int main(int argc, char **argv)
{
    ev::EventLoop::SP eventLoop = std::make_shared<ev::EventLoop>();
    ev::EventAsync::SP eventAsync = std::make_shared<ev::EventAsync>(eventLoop);

    const uint32_t times = 100;
    std::atomic<uint32_t> helloTimes(0);
    std::atomic<uint32_t> worldTimes(0);
    std::atomic<uint32_t> strictTimes(0);
    std::atomic<uint32_t> strictNewTimes(0);

    const ev::EventAsync::AsyncId helloId = 1;
    const ev::EventAsync::AsyncId worldId = 2;
    const ev::EventAsync::AsyncId strictId = 3;

    auto cb = [&helloTimes, &worldTimes, helloId] (ev::EventAsync::AsyncId id) {
        if (id == helloId) {
            ++helloTimes;
        } else {
            ++worldTimes;
        }
    };

    eventAsync->addAsync(helloId, cb);
    eventAsync->addAsync(worldId, cb);
    eventAsync->addAsync(strictId, [&strictTimes](ev::EventAsync::AsyncId) {
        ++strictTimes;
    }, ev::EventAsync::AsyncMode::StrictIsolation);

    eventAsync->start();

    std::thread th([eventLoop]() {
        eventLoop->dispatch();
    });

    for (int32_t i = 0; i < times; ++i) {
        eventAsync->notify(helloId);
        eventAsync->notify(worldId);
    }

    // 先投递严格模式通知，再删除旧回调并重新注册。
    eventAsync->notify(strictId);
    eventAsync->delAsync(strictId);
    eventAsync->addAsync(strictId, [&strictNewTimes](ev::EventAsync::AsyncId) {
        ++strictNewTimes;
    }, ev::EventAsync::AsyncMode::StrictIsolation);

    // 等待回调执行完毕
    sleep(2);

    printf("------------------\n");

    eventLoop->breakLoop();
    th.join();

    printf("hello: %u, world: %u, notify_times = %u\n", helloTimes.load(), worldTimes.load(), times);

    // 同一ID在消费前重复notify会被合并，只回调一次。
    assert(helloTimes == 1);
    assert(worldTimes == 1);
    // 严格隔离模式下，旧通知不能被新注册回调消费。
    assert(strictTimes == 0);
    assert(strictNewTimes == 0);
    printf("\033[32m" "SUCCESS" "\033[0m" "\n");
    return 0;
}
