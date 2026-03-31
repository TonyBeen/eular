/*************************************************************************
    > File Name: test_bbr_config.cc
    > Author: eular
    > Brief:
    > Created Time: Tue 31 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include "congestion/bbr_v1.h"
#include "utp/config.h"

using eular::utp::BbrV1;
using eular::utp::Config;
using eular::utp::RttStats;

TEST_CASE("BbrV1: configurable init/min cwnd are applied", "[BBR]")
{
    Config cfg;
    cfg.bbr_init_cwnd_mss = 24;
    cfg.bbr_min_cwnd_mss = 8;

    RttStats rtt;
    rtt.update(20000);

    BbrV1 bbr(&cfg);
    bbr.onInit(&rtt);

    REQUIRE(bbr.getCwnd() == 24 * 1460);
}

TEST_CASE("BbrV1: startup high gain affects initial pacing estimate", "[BBR]")
{
    RttStats rtt;
    rtt.update(20000);

    Config baseCfg;
    baseCfg.bbr_init_cwnd_mss = 32;
    baseCfg.bbr_startup_high_gain = 2.0f;
    BbrV1 lowGain(&baseCfg);
    lowGain.onInit(&rtt);
    const uint64_t lowRate = lowGain.getPacingRate(0);

    Config highCfg = baseCfg;
    highCfg.bbr_startup_high_gain = 3.0f;
    BbrV1 highGain(&highCfg);
    highGain.onInit(&rtt);
    const uint64_t highRate = highGain.getPacingRate(0);

    REQUIRE(highRate > lowRate);
}
