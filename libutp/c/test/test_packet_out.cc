#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>
#include <cstdlib>

extern "C" {
#include "proto/packet_out.h"
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

TEST_CASE("packet_out records a bounded sequence of send attempts", "[packet_out][attempt]") {
    utp_packet_out_t packet = {};

    REQUIRE(utp_packet_out_add_send_attempt(nullptr, 1u, 1u) == false);
    REQUIRE(utp_packet_out_add_send_attempt(&packet, 0u, 1u) == false);
    REQUIRE(utp_packet_out_add_send_attempt(&packet, 1u, 0u) == false);
    for (uint64_t index = 0u; index < UTP_PACKET_OUT_MAX_ATTEMPTS; ++index) {
        REQUIRE(utp_packet_out_add_send_attempt(&packet, index + 1u, (index + 1u) * 100u));
    }
    REQUIRE(packet.attempt_count == UTP_PACKET_OUT_MAX_ATTEMPTS);
    REQUIRE(packet.attempts[0].packet_number == 1u);
    REQUIRE(packet.attempts[UTP_PACKET_OUT_MAX_ATTEMPTS - 1u].sent_time_us == UTP_PACKET_OUT_MAX_ATTEMPTS * 100u);
    REQUIRE_FALSE(utp_packet_out_add_send_attempt(&packet, UTP_PACKET_OUT_MAX_ATTEMPTS + 1u, 500u));

    utp_packet_out_clear_send_attempts(&packet);
    REQUIRE(packet.attempt_count == 0u);
    REQUIRE(packet.attempts[0].packet_number == 0u);
}

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
    utp_packet_out_pool_t          pool                                      = {};
    utp_packet_out_bucket_config_t single[]                                  = {{128u, 1u}};
    utp_packet_out_bucket_config_t too_many[UTP_PACKET_OUT_MAX_BUCKETS + 1u] = {};
    utp_packet_out_bucket_config_t zero_size[]                               = {{0u, 1u}};
    utp_packet_out_bucket_config_t zero_count[]                              = {{128u, 0u}};
    size_t                         i;

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

TEST_CASE("packet_out pool rejects malformed custom allocator with NULL function pointer", "[packet_out][pool]") {
    allocation_tracker             tracker             = {};
    utp_packet_out_pool_t          pool                = {};
    utp_packet_out_bucket_config_t buckets[]           = {{128u, 1u}};
    utp_allocator_t                malformed_allocator = {nullptr, tracked_realloc, tracked_free, &tracker};

    REQUIRE(utp_packet_out_pool_init(&pool, &malformed_allocator, 1u, buckets, 1u) ==
            UTP_INTERNAL_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("packet_out pool acquire selects the smallest bucket regardless of configuration order",
          "[packet_out][acquire]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{512u, 1u}, {128u, 2u}};  // 故意乱序
    utp_packet_out_t              *pkt_small = nullptr;
    utp_packet_out_t              *pkt_large = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 4u, buckets, 2u) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt_small) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pkt_small->alloc_size == 128u);
    REQUIRE(pkt_small->raw_data == pkt_small->encrypt_data);
    REQUIRE(pkt_small->loss_chain == pkt_small);

    REQUIRE(utp_packet_out_pool_acquire(&pool, 200u, &pkt_large) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pkt_large->alloc_size == 512u);

    REQUIRE(utp_packet_out_pool_acquire(&pool, 9000u, &pkt_large) == UTP_INTERNAL_ERROR_INVALID_ARGUMENT);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out pool acquire reports LIMIT when a bucket is exhausted without touching other buckets",
          "[packet_out][acquire]") {
    utp_packet_out_pool_t          pool       = {};
    utp_packet_out_bucket_config_t buckets[]  = {{128u, 1u}, {512u, 1u}};
    utp_packet_out_t              *pkt_small  = nullptr;
    utp_packet_out_t              *pkt_small2 = nullptr;
    utp_packet_out_t              *pkt_large  = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 4u, buckets, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt_small) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt_small2) == UTP_INTERNAL_ERROR_LIMIT);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 500u, &pkt_large) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(pkt_large->alloc_size == 512u);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out pool acquire reports LIMIT when the struct pool is exhausted", "[packet_out][acquire]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 4u}};
    utp_packet_out_t              *pkt1      = nullptr;
    utp_packet_out_t              *pkt2      = nullptr;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 1u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt1) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt2) == UTP_INTERNAL_ERROR_LIMIT);

    // 结构体池耗尽时不消耗缓冲区名额:release 后应能再次成功 acquire。
    utp_packet_out_pool_release(&pool, pkt1);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt2) == UTP_INTERNAL_ERROR_OK);

    utp_packet_out_pool_cleanup(&pool);
}

TEST_CASE("packet_out pool release resets state but preserves the buffer for reuse", "[packet_out][release]") {
    utp_packet_out_pool_t          pool      = {};
    utp_packet_out_bucket_config_t buckets[] = {{128u, 1u}};
    utp_packet_out_t              *pkt       = nullptr;
    utp_packet_out_t              *pkt2      = nullptr;
    uint8_t                       *original_raw_data;
    uint16_t                       original_alloc_size;

    REQUIRE(utp_packet_out_pool_init(&pool, nullptr, 2u, buckets, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt) == UTP_INTERNAL_ERROR_OK);

    pkt->po_flags         = UTP_PO_ENCRYPTED;
    pkt->local_flags      = UTP_POL_LOSS;
    pkt->frame_types      = 0xffu;
    pkt->slice_count      = 3u;
    pkt->frame_meta_count = 2u;
    pkt->attempt_count    = 1u;
    original_raw_data     = pkt->raw_data;
    original_alloc_size   = pkt->alloc_size;

    utp_packet_out_pool_release(&pool, pkt);
    REQUIRE(utp_packet_out_pool_acquire(&pool, 100u, &pkt2) == UTP_INTERNAL_ERROR_OK);

    REQUIRE(pkt2->raw_data == original_raw_data);
    REQUIRE(pkt2->alloc_size == original_alloc_size);
    REQUIRE(pkt2->po_flags == 0u);
    REQUIRE(pkt2->local_flags == 0u);
    REQUIRE(pkt2->frame_types == 0u);
    REQUIRE(pkt2->slice_count == 0u);
    REQUIRE(pkt2->frame_meta_count == 0u);
    REQUIRE(pkt2->attempt_count == 0u);
    REQUIRE(pkt2->loss_chain == pkt2);

    utp_packet_out_pool_cleanup(&pool);
}
