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

TEST_CASE("packet_in dynamic pool grows and trims complete blocks", "[packet_in][pool]")
{
    static constexpr size_t kGrowCapacity                             = 64u;
    static constexpr size_t kBlockCapacity                            = 8u;
    static constexpr size_t kMaxFreeCapacity                          = 256u;
    utp_packet_in_pool_t    pool                                      = {};
    utp_packet_in_t*        packets[kMaxFreeCapacity + kGrowCapacity] = {};

    REQUIRE(utp_packet_in_pool_init_dynamic(&pool, nullptr, kGrowCapacity, kBlockCapacity, kMaxFreeCapacity, 128u) ==
            UTP_INTERNAL_ERROR_OK);
    REQUIRE(pool.packet_capacity == kGrowCapacity);
    REQUIRE(pool.free_count == kGrowCapacity);
    REQUIRE(pool.max_free_capacity == kMaxFreeCapacity);
    REQUIRE(utp_packet_in_pool_acquire_many(&pool, packets, kGrowCapacity) == UTP_INTERNAL_ERROR_OK);
    packets[0]->data[0] = 42u;
    REQUIRE(utp_packet_in_pool_acquire(&pool, &packets[kGrowCapacity]) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pool.packet_capacity == 2u * kGrowCapacity);
    REQUIRE(packets[0]->data[0] == 42u);
    for (size_t index = kGrowCapacity + 1u; index < kMaxFreeCapacity + kGrowCapacity; ++index) {
        REQUIRE(utp_packet_in_pool_acquire(&pool, &packets[index]) == UTP_INTERNAL_ERROR_OK);
    }
    REQUIRE(pool.packet_capacity == kMaxFreeCapacity + kGrowCapacity);
    REQUIRE(pool.free_count == 0u);
    for (size_t index = 0u; index < kMaxFreeCapacity + kGrowCapacity; ++index) {
        utp_packet_in_release(packets[index]);
    }
    REQUIRE(pool.packet_capacity == kMaxFreeCapacity);
    REQUIRE(pool.free_count == kMaxFreeCapacity);
    utp_packet_in_pool_cleanup(&pool);
}

TEST_CASE("packet_in dynamic pool honors a free cache limit below one growth batch", "[packet_in][pool]")
{
    utp_packet_in_pool_t pool = {};

    REQUIRE(utp_packet_in_pool_init_dynamic(&pool, nullptr, 64u, 8u, 16u, 128u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pool.packet_capacity == 16u);
    REQUIRE(pool.free_count == 16u);
    utp_packet_in_pool_cleanup(&pool);
}
