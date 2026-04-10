#include <string>

#include "catch/catch.hpp"
#include "utils/platform.h"
#include "utils/time.h"

using namespace eular;

TEST_CASE("system_time_and_abs_time_are_non_decreasing", "[time]") {
    const uint64_t system1 = Time::SystemTime();
    const uint64_t abs1 = Time::AbsTime();
    eular_msleep(5);
    const uint64_t system2 = Time::SystemTime();
    const uint64_t abs2 = Time::AbsTime();

    CHECK(system2 >= system1);
    CHECK(abs2 >= abs1);
}

TEST_CASE("format_and_parse_roundtrip", "[time]") {
    const std::string input = "2025-07-18 15:47:12";
    const time_t parsed = Time::Parse(input.c_str());
    REQUIRE(parsed != static_cast<time_t>(-1));
    CHECK(Time::Format(parsed) == input);
}

TEST_CASE("parse_rejects_invalid_time", "[time]") {
    CHECK(Time::Parse("invalid-time") == static_cast<time_t>(-1));
}

TEST_CASE("gmt_time_contains_gmt_suffix", "[time]") {
    time_t epoch = 0;
    const std::string gmt = Time::GmtTime(&epoch);
    CHECK(!gmt.empty());
    CHECK(gmt.find("GMT") != std::string::npos);
}