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

TEST_CASE("MtuDiscovery: disabled dplpmtud uses mtu_base", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = false;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE_FALSE(mtu.enabled());
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE(mtu.currentMaxPacketSize() == 1400 - 20 - 8);
    REQUIRE_FALSE(mtu.shouldProbe(0));
}

TEST_CASE("MtuDiscovery: probe ack defers mtu commit until probing converges", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;
    cfg.mtu_probe_step = 16;
    cfg.mtu_probe_interval = 300;
    cfg.mtu_probe_timeout = 1000;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE(mtu.shouldProbe(0));
    REQUIRE(mtu.nextProbeMtu() == 1450);
    REQUIRE(mtu.pathMtu() == 1400);

    REQUIRE(mtu.onProbeSent(101, 1450, 10));
    REQUIRE(mtu.hasInFlightProbe());
    REQUIRE_FALSE(mtu.shouldProbe(20));

    REQUIRE(mtu.onProbeAck(101, 30));
    REQUIRE_FALSE(mtu.hasInFlightProbe());
    // 统一切换：单次探测成功后不立即修改 pathMtu
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE(mtu.shouldProbe(31));
    REQUIRE(mtu.nextProbeMtu() == 1492);

    REQUIRE(mtu.onProbeSent(102, 1492, 40));
    REQUIRE(mtu.onProbeAck(102, 50));
    REQUIRE(mtu.pathMtu() == 1400);
}

TEST_CASE("MtuDiscovery: probe loss backs off ceiling", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;
    cfg.mtu_probe_step = 16;
    cfg.mtu_probe_interval = 2;
    cfg.mtu_probe_timeout = 100;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE(mtu.onProbeSent(200, 1450, 0));
    REQUIRE(mtu.onProbeAck(200, 10));
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE(mtu.shouldProbe(11));
    REQUIRE(mtu.nextProbeMtu() == 1492);

    REQUIRE(mtu.onProbeSent(201, 1492, 20));
    REQUIRE(mtu.onProbeLost(201, 30));
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE(mtu.shouldProbe(31));
    const uint16_t next = mtu.nextProbeMtu();
    REQUIRE(next > 1450);
    REQUIRE(next < 1492);

    MtuDiscovery timeoutMtu;
    timeoutMtu.init(&cfg, Address::IPv4);
    REQUIRE(timeoutMtu.onProbeSent(301, 1492, 2000));
    REQUIRE(timeoutMtu.onProbeTimeout(2200));
    REQUIRE_FALSE(timeoutMtu.hasInFlightProbe());
}

TEST_CASE("MtuDiscovery: blackhole fallback to safety mtu", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1450;
    cfg.mtu_probe_step = 16;
    cfg.mtu_probe_timeout = 100;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    const uint16_t nearMaxPacket = mtu.currentMaxPacketSize();
    REQUIRE(mtu.onDataPacketLoss(nearMaxPacket, 1000) == false);
    REQUIRE(mtu.onDataPacketLoss(nearMaxPacket, 1200) == false);
    REQUIRE(mtu.onDataPacketLoss(nearMaxPacket, 1400) == true);
    REQUIRE(mtu.pathMtu() == 1280);
    REQUIRE_FALSE(mtu.shouldProbe(2000));
}

TEST_CASE("MtuDiscovery: path validation success resets to mtu_base and restarts probing", "[Mtu]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1400;
    cfg.mtu_probe_step = 16;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    REQUIRE(mtu.onProbeSent(100, 1450, 0));
    REQUIRE(mtu.onProbeAck(100, 10));
    REQUIRE(mtu.onProbeSent(101, 1492, 20));
    REQUIRE(mtu.onProbeAck(101, 30));
    REQUIRE(mtu.pathMtu() == 1400);

    mtu.onPathValidated(2000);
    REQUIRE(mtu.pathMtu() == 1400);
    REQUIRE(mtu.shouldProbe(2000));
    REQUIRE(mtu.nextProbeMtu() == 1450);

    cfg.enable_dplpmtud = false;
    MtuDiscovery disabledMtu;
    disabledMtu.init(&cfg, Address::IPv4);
    disabledMtu.onPathValidated(3000);
    REQUIRE(disabledMtu.pathMtu() == 1400);
    REQUIRE_FALSE(disabledMtu.shouldProbe(3000));
}

TEST_CASE("MtuDiscovery: integration path switch 1500 to 1280 recovers quickly", "[Mtu][Integration]")
{
    Config cfg;
    cfg.enable_dplpmtud = true;
    cfg.mtu_min = 1280;
    cfg.mtu_max = 1500;
    cfg.mtu_base = 1450;
    cfg.mtu_probe_step = 16;
    cfg.mtu_probe_timeout = 100;
    cfg.mtu_blackhole_loss_threshold = 3;
    cfg.mtu_blackhole_loss_window_ms = 1200;
    cfg.mtu_blackhole_cooldown_ms = 800;

    MtuDiscovery mtu;
    mtu.init(&cfg, Address::IPv4);

    // 初始阶段自动上探到较高 MTU，模拟路径切换前稳定运行。
    utp_packno_t probeNo = 1000;
    utp_time_t nowMs = 0;
    for (int i = 0; i < 8; ++i) {
        if (!mtu.shouldProbe(nowMs)) {
            break;
        }
        const uint16_t probeMtu = mtu.nextProbeMtu();
        REQUIRE(probeMtu >= 1450);
        REQUIRE(mtu.onProbeSent(++probeNo, probeMtu, nowMs));
        nowMs += 20;
        REQUIRE(mtu.onProbeAck(probeNo, nowMs));
        nowMs += 10;
    }
    REQUIRE(mtu.pathMtu() >= 1492);

    // 路径切换到 1280 后，大包连续丢失，要求快速触发黑洞回退。
    const uint16_t largePacket = mtu.currentMaxPacketSize();
    const utp_time_t switchAtMs = 1000;
    REQUIRE_FALSE(mtu.onDataPacketLoss(largePacket, switchAtMs + 100));
    REQUIRE_FALSE(mtu.onDataPacketLoss(largePacket, switchAtMs + 200));
    REQUIRE(mtu.onDataPacketLoss(largePacket, switchAtMs + 300));
    REQUIRE(mtu.pathMtu() == 1280);

    // 恢复时延检查：在切换后 300ms 内完成回退。
    REQUIRE((switchAtMs + 300) - switchAtMs <= 300);

    // 冷静期内不应继续探测，冷静期结束后应重新进入梯队探测。
    REQUIRE_FALSE(mtu.shouldProbe(switchAtMs + 900));
    REQUIRE(mtu.shouldProbe(switchAtMs + 1200));
    REQUIRE(mtu.nextProbeMtu() == 1380);
}
