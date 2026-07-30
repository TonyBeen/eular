#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "congestion/minmax.h"
}

TEST_CASE("minmax retains a windowed maximum without allocation", "[congestion][minmax]") {
    utp_minmax_t maximum = {};

    utp_minmax_init(&maximum, 100u);
    utp_minmax_update_max(&maximum, 10u, 100u);
    utp_minmax_update_max(&maximum, 20u, 80u);
    utp_minmax_update_max(&maximum, 40u, 90u);
    REQUIRE(utp_minmax_get(&maximum) == 100u);

    utp_minmax_update_max(&maximum, 150u, 70u);
    REQUIRE(utp_minmax_get(&maximum) == 70u);
    utp_minmax_update_max(&maximum, 270u, 60u);
    REQUIRE(utp_minmax_get(&maximum) == 60u);
}

TEST_CASE("minmax retains a windowed minimum without allocation", "[congestion][minmax]") {
    utp_minmax_t minimum = {};

    utp_minmax_init(&minimum, 100u);
    utp_minmax_update_min(&minimum, 10u, 100u);
    utp_minmax_update_min(&minimum, 20u, 120u);
    utp_minmax_update_min(&minimum, 40u, 110u);
    REQUIRE(utp_minmax_get(&minimum) == 100u);

    utp_minmax_update_min(&minimum, 150u, 130u);
    REQUIRE(utp_minmax_get(&minimum) == 130u);
    utp_minmax_update_min(&minimum, 270u, 140u);
    REQUIRE(utp_minmax_get(&minimum) == 140u);
}
