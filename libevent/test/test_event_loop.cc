/*************************************************************************
    > File Name: test_event_loop.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 17时27分58秒
 ************************************************************************/

#include <thread>

#include <event/loop.h>
#include <event/async.h>

#include <event2/event.h>

int main(int argc, char **argv)
{
    ev::EventLoop::SP eventLoop = std::make_shared<ev::EventLoop>();
    ev::EventAsync::SP eventAsync = std::make_shared<ev::EventAsync>(eventLoop);

    auto cb = [](const std::string &key) {
        printf("event async: %s\n", key.c_str());
    };

    eventAsync->addAsync("Hello", cb);
    eventAsync->addAsync("World", cb);

    eventAsync->start();

    std::thread th([eventLoop]() {
        eventLoop->dispatch();
    });

    eventAsync->notify("Hello");
    eventAsync->notify("World");

    th.join();
    return 0;
}
