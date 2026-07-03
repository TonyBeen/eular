/*************************************************************************
    > File Name: test_bbr_new_params.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 29 Apr 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#define protected public
#define private public
#include "congestion/bbr_v1.h"
#undef private
#undef protected
#include "utp/config.h"

using eular::utp::BbrV1;
using eular::utp::Status;
using eular::utp::Config;
using eular::utp::Status;
using eular::utp::RttStats;
using eular::utp::Status;

TEST_CASE("BbrV1: new configurable params are applied", "[BBR]")
{
    Config cfg;
    cfg.bbr_probe_rtt_multiplier = 0.5f;
    cfg.bbr_similar_min_rtt_threshold = 1.5f;
    cfg.bbr_pacing_gains = {2.0f, 0.5f, 1.1f};

    RttStats rtt;
    rtt.update(20000);

    BbrV1 bbr(&cfg);
    bbr.onInit(&rtt);

    // We use #define private public to check these internal members
    REQUIRE(bbr.m_probeRttMultiplier == Approx(0.5f));
    REQUIRE(bbr.m_similarMinRttThreshold == Approx(1.5f));
    REQUIRE(bbr.m_pacingGains.size() == 3);
    REQUIRE(bbr.m_pacingGains[0] == Approx(2.0f));
    REQUIRE(bbr.m_pacingGains[1] == Approx(0.5f));
    REQUIRE(bbr.m_pacingGains[2] == Approx(1.1f));
}

TEST_CASE("BbrV1: probe_rtt_multiplier affects ProbeRTT cwnd", "[BBR]")
{
    RttStats rtt;
    rtt.update(20000);

    Config cfg;
    cfg.bbr_probe_rtt_multiplier = 0.6f;
    BbrV1 bbr(&cfg);
    bbr.onInit(&rtt);
    
    // Manually force ProbeRTT mode with BDP-based CWND
    bbr.m_mode = BbrV1::Mode::ProbeRTT;
    bbr.m_flags |= BbrV1::BBR_FLAG_PROBE_RTT_BASED_ON_BDP;
    
    // Set a dummy bandwidth to get a predictable BDP
    // BDP = minRtt * BW / 1000000
    // getMinRtt() will return 20000 if m_rttStats->minRTT() is 20000
    // BandWidth stores bits per second.
    // BW = 8000000 bps (1 MB/s)
    // BDP = 20000 * 1000000 / 1000000 = 20000 bytes
    bbr.m_maxBandwidth.updateMax(1, 8000000);

    // targetCwnd(0.6) = 0.6 * 20000 = 12000
    uint64_t expectedCwnd = static_cast<uint64_t>(0.6f * 20000);
    REQUIRE(bbr.getProbeRttCwnd() == expectedCwnd);
}
