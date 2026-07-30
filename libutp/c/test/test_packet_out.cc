#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>
#include <cstdlib>

extern "C" {
#include "internal/packet_out.h"
}

namespace {

struct allocation_tracker {
    size_t allocations = 0u;
    size_t frees       = 0u;
};

void *tracked_alloc(void *user_data, size_t size) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);

    ++tracker->allocations;
    return std::malloc(size);
}

void *tracked_realloc(void *user_data, void *pointer, size_t size) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);

    ++tracker->allocations;
    return std::realloc(pointer, size);
}

void tracked_free(void *user_data, void *pointer) {
    auto *tracker = static_cast<allocation_tracker *>(user_data);

    ++tracker->frees;
    std::free(pointer);
}

}  // namespace

TEST_CASE("packet_out pool init allocates once for structs plus two allocations per bucket", "[packet_out][pool]") {
    allocation_tracker             tracker   = {};
    const utp_allocator_t          allocator = {tracked_alloc, tracked_realloc, tracked_free, &tracker};
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 2u}, {512u, 1u}};

    REQUIRE(utp_packet_out_pool_init(&pool, &allocator, 3u, buckets, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(tracker.allocations == 5u);  // structs(1) + (storage+nodes) * 2 buckets

    utp_packet_out_pool_cleanup(&pool);
    REQUIRE(tracker.frees == 5u);
}

TEST_CASE("packet_out pool init rejects invalid bucket configuration", "[packet_out][pool]") {
    utp_packet_out_pool_t          pool                                       = {};
    utp_packet_out_bucket_config_t single[]                                   = {{128u, 1u}};
    utp_packet_out_bucket_config_t too_many[UTP_PACKET_OUT_MAX_BUCKETS + 1u]  = {};
    utp_packet_out_bucket_config_t zero_size[]                                = {{0u, 1u}};
    utp_packet_out_bucket_config_t zero_count[]                               = {{128u, 0u}};
    size_t                          i;

    for (i = 0u; i < UTP_PACKET_OUT_MAX_BUCKETS + 1u; ++i) {
        too_many[i].size  = static_cast<uint16_t>(128u + i);
        too_many[i].count = 1u;
    }

    REQUIRE(utp_packet_out_pool_init(nullptr, nullptr, 1u, single, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 0u, single, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, nullptr, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, single, 0u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, too_many, UTP_PACKET_OUT_MAX_BUCKETS + 1u) ==
            UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, zero_size, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, zero_count, 1u) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("packet_out pool accepts a bucket sized at the uint16_t ceiling", "[packet_out][pool]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{65535u, 1u}};

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    utp_packet_out_pool_cleanup(&pool);
}
