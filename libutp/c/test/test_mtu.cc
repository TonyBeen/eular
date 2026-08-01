#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "mtu/mtu.h"
}

TEST_CASE("mtu discovery normalizes IP-layer MTU and UDP payload budgets", "[mtu]")
{
    REQUIRE(utp_mtu_minimum_supported(UTP_ADDRESS_FAMILY_IPV4) == 49u);
    REQUIRE(utp_mtu_minimum_supported(UTP_ADDRESS_FAMILY_IPV6) == 69u);
    REQUIRE(utp_mtu_normalize(1u, UTP_ADDRESS_FAMILY_IPV6) == 69u);
    REQUIRE(utp_mtu_normalize(2000u, UTP_ADDRESS_FAMILY_IPV4) == 2000u);
    REQUIRE(utp_mtu_normalize(UINT16_MAX, UTP_ADDRESS_FAMILY_IPV4) == UINT16_MAX);
    REQUIRE(utp_mtu_packet_size_from_mtu(1500u, UTP_ADDRESS_FAMILY_IPV4) == 1472u);
    REQUIRE(utp_mtu_packet_size_from_mtu(1500u, UTP_ADDRESS_FAMILY_IPV6) == 1452u);
    REQUIRE(utp_mtu_from_packet_size(1472u, UTP_ADDRESS_FAMILY_IPV4) == 1500u);
    REQUIRE(utp_mtu_packet_size_from_mtu(UINT16_MAX, UTP_ADDRESS_FAMILY_IPV4) == 65507u);
    REQUIRE(utp_mtu_from_packet_size(65507u, UTP_ADDRESS_FAMILY_IPV4) == UINT16_MAX);
}

TEST_CASE("mtu discovery confirms a configured jumbo ceiling", "[mtu]")
{
    utp_mtu_config_t    config    = UTP_MTU_CONFIG_INIT;
    utp_mtu_discovery_t discovery = {};
    uint64_t            packet_number;

    config.mtu_max = 9000u;
    utp_mtu_discovery_init(&discovery, &config, UTP_ADDRESS_FAMILY_IPV4);

    for (packet_number = 1u; packet_number <= 32u; ++packet_number) {
        const uint16_t probe_mtu = utp_mtu_discovery_next_probe_mtu(&discovery);

        REQUIRE(probe_mtu > discovery.search_low_mtu);
        REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, packet_number, probe_mtu, packet_number));
        REQUIRE(utp_mtu_discovery_on_probe_ack(&discovery, packet_number, packet_number));
        if (probe_mtu == config.mtu_max) {
            break;
        }
    }

    REQUIRE(packet_number <= 32u);
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == config.mtu_max);
    REQUIRE(!utp_mtu_discovery_should_probe(&discovery, 33u));
}

TEST_CASE("mtu discovery converges through ladder probes", "[mtu]")
{
    utp_mtu_discovery_t discovery = {};

    utp_mtu_discovery_init(&discovery, nullptr, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == UTP_MTU_DEFAULT_BASE);
    REQUIRE(utp_mtu_discovery_current_max_packet_size(&discovery) == 1372u);
    REQUIRE(utp_mtu_discovery_should_probe(&discovery, 0u));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1450u);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 1u, 1450u, 0u));
    REQUIRE(utp_mtu_discovery_on_probe_ack(&discovery, 1u, 10u));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1492u);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 2u, 1492u, 11u));
    REQUIRE(utp_mtu_discovery_on_probe_ack(&discovery, 2u, 20u));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1500u);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 3u, 1500u, 21u));
    REQUIRE(utp_mtu_discovery_on_probe_ack(&discovery, 3u, 30u));
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == 1500u);
    REQUIRE(!utp_mtu_discovery_should_probe(&discovery, 31u));
}

TEST_CASE("mtu discovery narrows a failed probe with binary search", "[mtu]")
{
    utp_mtu_config_t    config    = UTP_MTU_CONFIG_INIT;
    utp_mtu_discovery_t discovery = {};

    config.probe_retries = 0u;
    utp_mtu_discovery_init(&discovery, &config, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 11u, 1450u, 100u));
    REQUIRE(utp_mtu_discovery_on_probe_lost(&discovery, 11u, 101u));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1425u);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 12u, 1425u, 102u));
    REQUIRE(utp_mtu_discovery_on_probe_lost(&discovery, 12u, 103u));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1412u);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 13u, 1412u, 104u));
    REQUIRE(utp_mtu_discovery_on_probe_ack(&discovery, 13u, 105u));
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == 1412u);
    REQUIRE(!utp_mtu_discovery_has_in_flight_probe(&discovery));
}

TEST_CASE("mtu discovery rebuilds a lost probe before narrowing the search", "[mtu]")
{
    utp_mtu_discovery_t discovery = {};

    utp_mtu_discovery_init(&discovery, nullptr, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(discovery.probe_retries == 1u);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 11u, 1450u, 100u));
    REQUIRE(utp_mtu_discovery_on_probe_lost(&discovery, 11u, 101u));
    REQUIRE(!utp_mtu_discovery_has_in_flight_probe(&discovery));
    REQUIRE(discovery.retry_pending);
    REQUIRE(discovery.probe_retry_count == 1u);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1450u);
    REQUIRE(utp_mtu_discovery_should_probe(&discovery, 102u));

    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 12u, 1450u, 102u));
    REQUIRE(!discovery.retry_pending);
    REQUIRE(discovery.probe_retry_count == 1u);
    REQUIRE(utp_mtu_discovery_on_probe_lost(&discovery, 12u, 103u));
    REQUIRE(!discovery.retry_pending);
    REQUIRE(discovery.probe_retry_count == 0u);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1425u);
}

TEST_CASE("mtu discovery immediately narrows an oversized local probe write", "[mtu]")
{
    utp_mtu_discovery_t discovery = {};

    utp_mtu_discovery_init(&discovery, nullptr, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1450u);
    REQUIRE(utp_mtu_discovery_on_probe_send_failed(&discovery, 1450u, 100u));
    REQUIRE(!utp_mtu_discovery_has_in_flight_probe(&discovery));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1425u);
    REQUIRE(utp_mtu_discovery_should_probe(&discovery, 101u));
}

TEST_CASE("mtu discovery times out probes and backs off black holes", "[mtu]")
{
    utp_mtu_discovery_t discovery   = {};
    const uint16_t      packet_size = 1372u;

    utp_mtu_discovery_init(&discovery, nullptr, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 21u, 1450u, 100u));
    REQUIRE(!utp_mtu_discovery_on_probe_timeout(&discovery, 2099u));
    REQUIRE(utp_mtu_discovery_on_probe_timeout(&discovery, 2100u));
    REQUIRE(!utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 10000u));
    REQUIRE(!utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 11000u));
    REQUIRE(utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 12000u));
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == UTP_MTU_DEFAULT_MIN);
    REQUIRE(!utp_mtu_discovery_should_probe(&discovery, 16999u));
    REQUIRE(utp_mtu_discovery_should_probe(&discovery, 17000u));
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == UTP_MTU_DEFAULT_BASE);
}

TEST_CASE("mtu discovery confirms base before searching upward after a black hole", "[mtu]")
{
    utp_mtu_config_t    config      = UTP_MTU_CONFIG_INIT;
    utp_mtu_discovery_t discovery   = {};
    const uint16_t      packet_size = 1372u;

    config.probe_retries = 0u;
    utp_mtu_discovery_init(&discovery, &config, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(!utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 10000u));
    REQUIRE(!utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 11000u));
    REQUIRE(utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 12000u));
    REQUIRE(discovery.probe_phase == UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE);
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == config.mtu_min);
    REQUIRE(utp_mtu_discovery_should_probe(&discovery, 17000u));
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 31u, config.mtu_base, 17000u));
    REQUIRE(utp_mtu_discovery_on_probe_ack(&discovery, 31u, 17001u));
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == config.mtu_base);
    REQUIRE(discovery.probe_phase == UTP_MTU_PROBE_PHASE_LADDER);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) == 1450u);
}

TEST_CASE("mtu discovery restricts a failed black-hole base probe below base", "[mtu]")
{
    utp_mtu_config_t    config      = UTP_MTU_CONFIG_INIT;
    utp_mtu_discovery_t discovery   = {};
    const uint16_t      packet_size = 1372u;

    config.probe_retries = 0u;
    utp_mtu_discovery_init(&discovery, &config, UTP_ADDRESS_FAMILY_IPV4);
    REQUIRE(!utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 10000u));
    REQUIRE(!utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 11000u));
    REQUIRE(utp_mtu_discovery_on_data_packet_loss(&discovery, packet_size, 12000u));
    REQUIRE(utp_mtu_discovery_on_probe_sent(&discovery, 41u, config.mtu_base, 17000u));
    REQUIRE(utp_mtu_discovery_on_probe_lost(&discovery, 41u, 17001u));
    REQUIRE(utp_mtu_discovery_path_mtu(&discovery) == config.mtu_min);
    REQUIRE(discovery.probe_phase == UTP_MTU_PROBE_PHASE_BINARY);
    REQUIRE(discovery.ceiling_mtu == config.mtu_base - 1u);
    REQUIRE(utp_mtu_discovery_next_probe_mtu(&discovery) < config.mtu_base);
}
