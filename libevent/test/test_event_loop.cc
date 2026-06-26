/*************************************************************************
    > File Name: test_event_loop.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 17时27分58秒
 ************************************************************************/

#include <thread>

#include <unistd.h>

#include <event/loop.h>
#include <event/async.h>

#include <event2/event.h>

int main(int argc, char **argv)
{
    ev::EventLoop::SP eventLoop = std::make_shared<ev::EventLoop>();
    ev::EventAsync::SP eventAsync = std::make_shared<ev::EventAsync>(eventLoop);

    auto cb = [](ev::EventAsync::AsyncId id) {
        printf("event async id: %u\n", id);
    };

    eventAsync->addAsync(1, cb);
    eventAsync->addAsync(2, cb);

    eventAsync->start();

    std::thread th([eventLoop]() {
        eventLoop->dispatch();
    });

    eventAsync->notify(1);
    eventAsync->notify(2);

    sleep(1);
    eventLoop->breakLoop();

    th.join();
    return 0;
}
