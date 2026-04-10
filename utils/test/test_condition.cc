#include <atomic>
#include <thread>

#include "catch/catch.hpp"
#include "utils/condition.h"
#include "utils/platform.h"

using namespace eular;

namespace {

bool wait_for_atomic_value(const std::atomic<int> &value, int expected, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        if (value.load() == expected) {
            return true;
        }
        eular_msleep(5);
    }
    return value.load() == expected;
}

} // namespace

TEST_CASE("condition_timed_wait_times_out", "[condition]") {
    Condition condition;
    Mutex mutex;

    mutex.lock();
    const int code = condition.timedWait(mutex, 50);
    mutex.unlock();

    CHECK(code != 0);
}

TEST_CASE("condition_signal_wakes_waiter", "[condition]") {
    Condition condition;
    Mutex mutex;
    bool ready = false;
    bool awakened = false;
    int waitCode = -1;

    std::thread waiter([&]() {
        mutex.lock();
        while (!ready) {
            waitCode = condition.timedWait(mutex, 500);
            if (waitCode != 0 && !ready) {
                break;
            }
        }
        awakened = ready;
        mutex.unlock();
    });

    eular_msleep(30);
    mutex.lock();
    ready = true;
    condition.signal();
    mutex.unlock();

    waiter.join();
    CHECK(waitCode == 0);
    CHECK(awakened);
}

TEST_CASE("condition_broadcast_wakes_all_waiters", "[condition]") {
    Condition condition;
    Mutex mutex;
    std::atomic<int> started(0);
    std::atomic<int> woke(0);
    bool ready = false;

    auto waiter = [&]() {
        mutex.lock();
        ++started;
        while (!ready) {
            condition.wait(mutex);
        }
        ++woke;
        mutex.unlock();
    };

    std::thread t1(waiter);
    std::thread t2(waiter);

    REQUIRE(wait_for_atomic_value(started, 2, 500));

    mutex.lock();
    ready = true;
    condition.broadcast();
    mutex.unlock();

    t1.join();
    t2.join();
    CHECK(woke.load() == 2);
}