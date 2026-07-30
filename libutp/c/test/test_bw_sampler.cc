#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "congestion/bw_sampler.h"
}

TEST_CASE("bandwidth sampler derives a delivery-rate sample from packet state", "[congestion][bw_sampler]") {
    utp_bw_sampler_t      sampler = {};
    utp_bw_packet_state_t packet  = {};
    utp_bw_sample_t       sample  = {};

    utp_bw_sampler_init(&sampler);
    utp_bw_sampler_on_packet_sent(&sampler, &packet, 1u, 100u, 100u);
    REQUIRE(utp_bw_sampler_on_packet_acked(&sampler, &packet, 1u, 1000u, &sample));
    REQUIRE(sample.bandwidth_bytes_per_second == 100000u);
    REQUIRE(sample.rtt_us == 900u);
    REQUIRE_FALSE(sample.is_app_limited);
}
