#define CATCH_CONFIG_MAIN

#include <limits>

#include <catch2/catch.hpp>

extern "C" {
#include "congestion/bbr.h"
}

TEST_CASE("bbr starts with configured cwnd and produces a pacing rate after an ACK", "[congestion][bbr]")
{
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

TEST_CASE("bbr applies configured ProbeRTT timings", "[congestion][bbr]")
{
    utp_bbr_config_t             config       = {};
    utp_bbr_t                    bbr          = {};
    utp_rtt_stats_t              rtt          = {};
    utp_bw_packet_state_t        first_state  = {};
    utp_bw_packet_state_t        second_state = {};
    utp_bw_packet_state_t        third_state  = {};
    utp_bw_packet_state_t        fourth_state = {};
    utp_bw_packet_state_t        fifth_state  = {};
    utp_congestion_packet_info_t first        = {1u, 100u, 1460u, &first_state};
    utp_congestion_packet_info_t second       = {2u, UINT64_C(1000100), 1460u, &second_state};
    utp_congestion_packet_info_t third        = {3u, UINT64_C(1004000), 1460u, &third_state};
    utp_congestion_packet_info_t fourth       = {4u, UINT64_C(1004100), 1460u, &fourth_state};
    utp_congestion_packet_info_t fifth        = {5u, UINT64_C(1053001), 1460u, &fifth_state};
    utp_congestion_t*            congestion;

    for (uint32_t index = 0u; index < UTP_BBR_PACING_GAIN_COUNT; ++index) {
        config.pacing_gains[index] = 1.0;
    }
    config.probe_rtt_ms      = 1u;
    config.min_rtt_expiry_ms = 1u;
    utp_bbr_init(&bbr, &config);
    REQUIRE(bbr.probe_rtt_time_us == 50000u);
    REQUIRE(bbr.min_rtt_expiry_us == UINT64_C(1000000));

    config.probe_rtt_ms      = 50u;
    config.min_rtt_expiry_ms = 1000u;
    utp_bbr_init(&bbr, &config);
    congestion = utp_bbr_as_congestion(&bbr);
    REQUIRE(utp_rtt_stats_update(&rtt, 1000u) == UTP_INTERNAL_ERROR_OK);
    utp_congestion_init(congestion, &rtt);

    utp_congestion_on_packet_sent(congestion, &first, 0u, 0);
    utp_congestion_on_begin_ack(congestion, 2000u, 1460u);
    utp_congestion_on_ack(congestion, &first, 2000u, 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(bbr.mode == UTP_BBR_STARTUP);
    REQUIRE(bbr.min_rtt_timestamp_us == 2000u);
    REQUIRE(bbr.max_bandwidth.samples[0].time == 1u);

    utp_congestion_on_packet_sent(congestion, &second, 0u, 0);
    utp_congestion_on_begin_ack(congestion, UINT64_C(1003000), 1460u);
    utp_congestion_on_ack(congestion, &second, UINT64_C(1003000), 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(bbr.mode == UTP_BBR_PROBE_RTT);
    REQUIRE(bbr.min_rtt_us == 2900u);
    REQUIRE(bbr.probe_rtt_exit_us == UINT64_C(1053000));

    utp_congestion_on_packet_sent(congestion, &third, 0u, 0);
    utp_congestion_on_packet_sent(congestion, &fourth, 1460u, 0);
    utp_congestion_on_begin_ack(congestion, UINT64_C(1052000), 2920u);
    utp_congestion_on_ack(congestion, &third, UINT64_C(1052000), 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(bbr.mode == UTP_BBR_PROBE_RTT);
    REQUIRE(bbr.probe_rtt_round_passed);

    utp_congestion_on_begin_ack(congestion, UINT64_C(1053000), 1460u);
    utp_congestion_on_ack(congestion, &fourth, UINT64_C(1053000), 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(bbr.mode == UTP_BBR_STARTUP);
    REQUIRE(bbr.min_rtt_timestamp_us == UINT64_C(1053000));

    bbr.mode                   = UTP_BBR_PROBE_RTT;
    bbr.full_bandwidth_reached = true;
    bbr.probe_rtt_exit_us      = UINT64_C(1054000);
    bbr.probe_rtt_round_passed = false;
    utp_congestion_on_packet_sent(congestion, &fifth, 0u, 0);
    utp_congestion_on_begin_ack(congestion, UINT64_C(1054000), 1460u);
    utp_congestion_on_ack(congestion, &fifth, UINT64_C(1054000), 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(bbr.mode == UTP_BBR_PROBE_BW);
    REQUIRE(bbr.min_rtt_timestamp_us == UINT64_C(1054000));
}

TEST_CASE("bbr applies configured ProbeBW pacing gains", "[congestion][bbr]")
{
    utp_bbr_config_t             config       = {};
    utp_bbr_t                    bbr          = {};
    utp_rtt_stats_t              rtt          = {};
    utp_bw_packet_state_t        packet_state = {};
    utp_congestion_packet_info_t packet       = {1u, 100u, 1460u, &packet_state};
    utp_congestion_t*            congestion;

    for (uint32_t index = 0u; index < UTP_BBR_PACING_GAIN_COUNT; ++index) {
        config.pacing_gains[index] = 1.0;
    }
    config.pacing_gains[2u] = 1.5;
    utp_bbr_init(&bbr, &config);
    congestion = utp_bbr_as_congestion(&bbr);
    REQUIRE(utp_rtt_stats_update(&rtt, 1000u) == UTP_INTERNAL_ERROR_OK);
    utp_congestion_init(congestion, &rtt);
    bbr.mode                   = UTP_BBR_DRAIN;
    bbr.full_bandwidth_reached = true;

    utp_congestion_on_packet_sent(congestion, &packet, 0u, 0);
    utp_congestion_on_begin_ack(congestion, 2000u, 1460u);
    utp_congestion_on_ack(congestion, &packet, 2000u, 0);
    utp_congestion_on_end_ack(congestion, 0u);
    REQUIRE(bbr.mode == UTP_BBR_PROBE_BW);
    REQUIRE(bbr.cycle_index == 2u);
    REQUIRE(bbr.pacing_gain == 1.5);
}

TEST_CASE("bbr rejects invalid scalar gains", "[congestion][bbr]")
{
    utp_bbr_config_t config = {};
    utp_bbr_t        bbr    = {};

    config.startup_high_gain     = std::numeric_limits<double>::quiet_NaN();
    config.cwnd_gain             = std::numeric_limits<double>::quiet_NaN();
    config.startup_growth_target = std::numeric_limits<double>::quiet_NaN();
    for (uint32_t index = 0u; index < UTP_BBR_PACING_GAIN_COUNT; ++index) {
        config.pacing_gains[index] = 1.0;
    }

    utp_bbr_init(&bbr, &config);
    REQUIRE(bbr.high_gain == 2.885);
    REQUIRE(bbr.configured_cwnd_gain == 2.0);
    REQUIRE(bbr.startup_growth_target == 1.25);
}
