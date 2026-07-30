#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "congestion/bbr.h"
}

TEST_CASE("bbr starts with configured cwnd and produces a pacing rate after an ACK", "[congestion][bbr]") {
    utp_bbr_t                    bbr    = {};
    utp_rtt_stats_t              rtt    = {};
    utp_congestion_packet_info_t packet = {1u, 100u, 1460u, nullptr};
    utp_congestion_t*            congestion;

    REQUIRE(utp_rtt_stats_update(&rtt, 10000u) == UTP_INTERNAL_ERROR_OK);
    utp_bbr_init(&bbr, nullptr);
    congestion = utp_bbr_as_congestion(&bbr);
    utp_congestion_init(congestion, &rtt);
    REQUIRE(utp_congestion_get_cwnd(congestion) == 16u * 1460u);

    utp_congestion_on_packet_sent(congestion, &packet, 0u, 0);
    utp_congestion_on_begin_ack(congestion, 10100u, 1460u);
    utp_congestion_on_ack(congestion, &packet, 10100u, 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(utp_congestion_get_pacing_rate(congestion, 0) > 0u);
    REQUIRE(utp_congestion_get_cwnd(congestion) >= 16u * 1460u);
}
