/*************************************************************************
    > File Name: test_cubic.cc
    > Author: eular
    > Brief:
    > Created Time: Tue 31 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include "congestion/cubic.h"
#include "utp/config.h"

using eular::utp::Cubic;
using eular::utp::Config;
using eular::utp::PacketInfo;
using eular::utp::RttStats;

namespace {
PacketInfo MakePacket(uint64_t packetNo, uint64_t sendTimeUs, uint32_t packetSize)
{
    PacketInfo info{};
    info.packetNo = packetNo;
    info.sendTimeUs = sendTimeUs;
    info.packetSize = packetSize;
    info.packetState = nullptr;
    return info;
}
} // namespace

TEST_CASE("Cubic: slow start grows by acked bytes", "[Cubic]")
{
    RttStats rtt;
    rtt.update(20000);

    Cubic cubic;
    cubic.onInit(&rtt);

    const uint64_t base = cubic.getCwnd();

    cubic.onBeginAck(1000, 0);
    PacketInfo p1 = MakePacket(1, 100, 1200);
    PacketInfo p2 = MakePacket(2, 200, 1200);
    cubic.onAck(&p1, 1000, 0);
    cubic.onAck(&p2, 2000, 0);
    cubic.onEndAck(0);

    REQUIRE(cubic.getCwnd() == base + 2400);
}

TEST_CASE("Cubic: loss reduces cwnd and later ack increases it", "[Cubic]")
{
    RttStats rtt;
    rtt.update(25000);

    Cubic cubic;
    cubic.onInit(&rtt);

    PacketInfo lost = MakePacket(10, 1000, 1460);
    const uint64_t beforeLoss = cubic.getCwnd();
    cubic.onLost(&lost);
    const uint64_t afterLoss = cubic.getCwnd();

    REQUIRE(afterLoss < beforeLoss);

    cubic.onBeginAck(200000, afterLoss);
    for (int i = 0; i < 16; ++i) {
        PacketInfo acked = MakePacket(100 + i, 1000 + i * 1000, 1460);
        cubic.onAck(&acked, 200000 + i * 1000, 0);
    }
    cubic.onEndAck(afterLoss);

    REQUIRE(cubic.getCwnd() > afterLoss);
}

TEST_CASE("Cubic: timeout resets cwnd to min and pacing rate stays positive", "[Cubic]")
{
    RttStats rtt;
    rtt.update(30000);

    Cubic cubic;
    cubic.onInit(&rtt);

    PacketInfo lost = MakePacket(20, 1000, 1460);
    cubic.onLost(&lost);
    cubic.onTimeout();

    REQUIRE(cubic.getCwnd() == 4 * 1460);
    REQUIRE(cubic.getPacingRate(0) > 0);
    REQUIRE(cubic.getPacingRate(1) > 0);
}

TEST_CASE("Cubic: configurable init/min cwnd are applied", "[Cubic]")
{
    Config cfg;
    cfg.cubic_init_cwnd_mss = 20;
    cfg.cubic_min_cwnd_mss = 8;
    cfg.cubic_beta = 0.65;
    cfg.cubic_c = 0.6;

    RttStats rtt;
    rtt.update(25000);

    Cubic cubic(&cfg);
    cubic.onInit(&rtt);

    REQUIRE(cubic.getCwnd() == 20 * 1460);

    PacketInfo lost = MakePacket(30, 1000, 1460);
    cubic.onLost(&lost);
    cubic.onTimeout();
    REQUIRE(cubic.getCwnd() == 8 * 1460);
}
