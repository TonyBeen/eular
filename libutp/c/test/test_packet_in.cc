#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>

extern "C" {
#include "proto/packet_in.h"
}

TEST_CASE("packet_in pool acquire ref release and reuse packet buffers", "[packet_in][pool]")
{
    utp_packet_in_pool_t pool   = {};
    utp_packet_in_t*     packet = nullptr;
    utp_packet_in_t*     again  = nullptr;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 1u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &packet) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->in_use);
    REQUIRE(packet->ref_count == 1u);
    REQUIRE(packet->capacity == 128u);
    REQUIRE(utp_packet_in_ref(packet));
    REQUIRE(packet->ref_count == 2u);
    utp_packet_in_release(packet);
    REQUIRE(packet->in_use);
    REQUIRE(packet->ref_count == 1u);
    packet->length = 10u;
    utp_packet_in_release(packet);
    REQUIRE_FALSE(packet->in_use);
    REQUIRE(packet->ref_count == 0u);
    REQUIRE(packet->length == 0u);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &again) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(again == packet);
    utp_packet_in_release(again);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("packet_in pool reports limit when all packets are referenced", "[packet_in][pool]")
{
    utp_packet_in_pool_t pool    = {};
    utp_packet_in_t*     first   = nullptr;
    utp_packet_in_t*     second  = nullptr;
    utp_packet_in_t*     blocked = nullptr;

    REQUIRE(utp_packet_in_pool_init(&pool, nullptr, 2u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &first) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &second) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &blocked) == UTP_INTERNAL_ERROR_LIMIT);
    REQUIRE(blocked == nullptr);
    utp_packet_in_release(first);
    REQUIRE(utp_packet_in_pool_acquire(&pool, &blocked) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(blocked == first);
    utp_packet_in_release(blocked);
    utp_packet_in_release(second);
    utp_packet_in_pool_cleanup(&pool);
}
