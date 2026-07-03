#include <limits>

#include "catch/catch.hpp"
#include "utils/elapsed_time.h"
#include "utils/platform.h"

using namespace eular;

TEST_CASE("elapsed_time_is_zero_before_measurement", "[elapsed_time]") {
    ElapsedTime timer;
    CHECK(timer.elapsedTime() == 0);
}

TEST_CASE("elapsed_time_records_single_interval", "[elapsed_time]") {
    ElapsedTime timer(ElapsedTimeType::MILLISECOND);
    timer.start();
    eular_msleep(20);
    timer.stop();

    const uint64_t elapsed = timer.elapsedTime();
    CHECK(elapsed >= 10);
    CHECK(timer.elapsedTime() == 0);
}

TEST_CASE("elapsed_time_accumulates_multiple_intervals", "[elapsed_time]") {
    ElapsedTime timer(ElapsedTimeType::MILLISECOND);
    timer.start();
    eular_msleep(10);
    timer.stop();

    timer.start();
    eular_msleep(15);
    timer.stop();

    CHECK(timer.elapsedTime() >= 20);
}

TEST_CASE("elapsed_time_reset_clears_state", "[elapsed_time]") {
    ElapsedTime timer(ElapsedTimeType::MILLISECOND);
    timer.start();
    eular_msleep(10);
    timer.stop();
    timer.reset();

    CHECK(timer.elapsedTime() == 0);
}