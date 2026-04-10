#include <atomic>
#include <thread>
#include <vector>

#include "catch/catch.hpp"
#include <utils/mutex.h>

using namespace eular;

TEST_CASE("mutex_protects_shared_counter", "[mutex]")
{
    Mutex mutex;
    int counter = 0;

    auto worker = [&]() {
        for (int i = 0; i < 1000; ++i) {
            mutex.lock();
            ++counter;
            mutex.unlock();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(worker);
    }
    for (std::thread &thread : threads) {
        thread.join();
    }

    CHECK(counter == 4000);
}

TEST_CASE("call_once_runs_exactly_once", "[mutex][once]")
{
    once_flag flag;
    std::atomic<int> invokeCount(0);

    auto worker = [&]() {
        for (int i = 0; i < 16; ++i) {
            call_once(flag, [&]() {
                ++invokeCount;
            });
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(worker);
    }
    for (std::thread &thread : threads) {
        thread.join();
    }

    CHECK(invokeCount.load() == 1);
}