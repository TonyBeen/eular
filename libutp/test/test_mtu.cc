/*************************************************************************
    > File Name: test_mtu.cc
    > Author: eular
    > Brief:
    > Created Time: Thu 19 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include "mtu/mtu.h"
#include "utp/config.h"

using eular::utp::Address;
using eular::utp::Config;
using eular::utp::MtuDiscovery;

TEST_CASE("MtuDiscovery: initialize and conversion", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;
    cfg.mtu_probe_step = 16;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE(mtu.enabled());
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE(mtu.currentMaxPacketSize() == 1400 - 20 - 8);

    REQUIRE(MtuDiscovery::PacketSizeFromMtu(1500, Address::IPv4) == 1472);
    REQUIRE(MtuDiscovery::MtuFromPacketSize(1472, Address::IPv4) == 1500);
    REQUIRE(MtuDiscovery::PacketSizeFromMtu(1500, Address::IPv6) == 1452);
}

TEST_CASE("MtuDiscovery: probe ack raises mtu", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;
    cfg.mtu_probe_step = 20;
    cfg.mtu_probe_interval = 300;
    cfg.mtu_probe_timeout = 1000;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE(mtu.shouldProbe(0));
    REQUIRE(mtu.nextProbeMtu() == 1420);

    REQUIRE(mtu.onProbeSent(101, 1420, 10));
    REQUIRE(mtu.hasInFlightProbe());
    REQUIRE_FALSE(mtu.shouldProbe(20));

    REQUIRE(mtu.onProbeAck(101, 30));
    REQUIRE_FALSE(mtu.hasInFlightProbe());
    REQUIRE(mtu.pathMtu() == 1420);
    REQUIRE(mtu.shouldProbe(31));
}

TEST_CASE("MtuDiscovery: probe loss backs off ceiling", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;
    cfg.mtu_probe_step = 20;
    cfg.mtu_probe_interval = 2;
    cfg.mtu_probe_timeout = 100;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE(mtu.onProbeSent(200, 1420, 0));
    REQUIRE(mtu.onProbeLost(200, 10));
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE_FALSE(mtu.shouldProbe(1500));

    MtuDiscovery timeoutMtu;
    timeoutMtu.init(&cfg, Address::IPv4);
    REQUIRE(timeoutMtu.onProbeSent(201, 1420, 2000));
    REQUIRE(timeoutMtu.onProbeTimeout(2200));
    REQUIRE_FALSE(timeoutMtu.hasInFlightProbe());
}
