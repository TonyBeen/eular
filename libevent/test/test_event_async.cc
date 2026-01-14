/*************************************************************************
    > File Name: test_event_async.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年10月31日 星期四 09时48分13秒
 ************************************************************************/

#include <assert.h>
#include <iostream>
#include <thread>

#include <unistd.h>

#include <event/loop.h>
#include <event/async.h>

#include <event2/event.h>

int main(int argc, char **argv)
{
    ev::EventLoop::SP eventLoop = std::make_shared<ev::EventLoop>();
    ev::EventAsync::SP eventAsync = std::make_shared<ev::EventAsync>(eventLoop);

    uint32_t times = 100;
    uint32_t helloTimes = 0;
    uint32_t worldTimes = 0;

    const std::string helloKey = "Hello";
    const std::string worldKey = "World";

    auto cb = [&helloTimes, &worldTimes, &helloKey] (const std::string &key) {
        if (key == helloKey) {
            ++helloTimes;
        } else {
            ++worldTimes;
        }

        // printf("event async: %s\n", key.c_str());
    };

    eventAsync->addAsync(helloKey, cb);
    eventAsync->addAsync(worldKey, cb);

    eventAsync->start();

    std::thread th([eventLoop]() {
        eventLoop->dispatch();
    });

    for (int32_t i = 0; i < times; ++i) {
        eventAsync->notify(helloKey);
        eventAsync->notify(worldKey);
    }

    // 等待回调执行完毕
    sleep(2);

    printf("------------------\n");

    eventLoop->breakLoop();
    th.join();

    printf("hello: %u, world: %u, times = %u\n", helloTimes, worldTimes, times);

    assert(helloTimes == times);
    assert(worldTimes == times);
    printf("\033[32m" "SUCCESS" "\033[0m" "\n");
    return 0;
}
