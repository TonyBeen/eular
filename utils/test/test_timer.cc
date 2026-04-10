#include <atomic>

#include "catch/catch.hpp"

#include <utils/platform.h>
#include <utils/timer.h>

using namespace eular;

namespace {

bool wait_for_count_at_least(const std::atomic<int> &count, int expected, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        if (count.load() >= expected) {
            return true;
        }
        eular_msleep(5);
    }
    return count.load() >= expected;
}

} // namespace

TEST_CASE("timer_manager_executes_one_shot_timer", "[timer]")
{
    TimerManager manager;
    std::atomic<int> fired(0);

    REQUIRE(manager.startTimer() == 0);
    REQUIRE(manager.addTimer(30, [&]() {
        ++fired;
    }) != 0);

    REQUIRE(wait_for_count_at_least(fired, 1, 1000));
    manager.stopTimer();
    CHECK(fired.load() == 1);
}

TEST_CASE("timer_manager_executes_recycle_timer_multiple_times", "[timer]")
{
    TimerManager manager;
    std::atomic<int> fired(0);

    REQUIRE(manager.startTimer() == 0);
    REQUIRE(manager.addTimer(20, [&]() {
        ++fired;
    }, 20) != 0);

    REQUIRE(wait_for_count_at_least(fired, 3, 1000));
    manager.stopTimer();
    CHECK(fired.load() >= 3);
}
