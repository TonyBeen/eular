#define CATCH_CONFIG_MAIN

#include <catch2/catch.hpp>
#include <cstdlib>

extern "C" {
#include "internal/ack.h"
#include "internal/ack_scheduler.h"
#include "internal/receive_history.h"
#include "internal/rtt.h"
#include "internal/send_history.h"
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

TEST_CASE("receive history records a singleton packet range", "[receive_history]") {
    utp_receive_history_t      history = {};
    const utp_receive_range_t *range;

    REQUIRE(utp_receive_history_init(&history, nullptr, 4u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 10u, 1234u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_largest(&history) == 10u);
    REQUIRE(utp_receive_history_largest_received_at(&history) == 1234u);
    REQUIRE(utp_receive_history_range_count(&history) == 1u);

    range = utp_receive_history_range_at(&history, 0u);
    REQUIRE(range != nullptr);
    REQUIRE(range->low == 10u);
    REQUIRE(range->high == 10u);

    utp_receive_history_cleanup(&history);
}

TEST_CASE("receive history merges adjacent packets and orders ranges by descending packet number",
          "[receive_history]") {
    utp_receive_history_t      history = {};
    const utp_receive_range_t *range;

    REQUIRE(utp_receive_history_init(&history, nullptr, 4u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 10u, 10u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 12u, 12u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 11u, 11u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_range_count(&history) == 1u);
    REQUIRE(utp_receive_history_insert(&history, 100u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 50u, 50u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 11u, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_largest(&history) == 100u);
    REQUIRE(utp_receive_history_largest_received_at(&history) == 100u);
    REQUIRE(utp_receive_history_range_count(&history) == 3u);

    range = utp_receive_history_range_at(&history, 0u);
    REQUIRE(range != nullptr);
    REQUIRE(range->low == 100u);
    REQUIRE(range->high == 100u);
    range = utp_receive_history_range_at(&history, 1u);
    REQUIRE(range != nullptr);
    REQUIRE(range->low == 50u);
    REQUIRE(range->high == 50u);
    range = utp_receive_history_range_at(&history, 2u);
    REQUIRE(range != nullptr);
    REQUIRE(range->low == 10u);
    REQUIRE(range->high == 12u);

    utp_receive_history_cleanup(&history);
}

TEST_CASE("receive history prunes old packets and treats them as duplicates", "[receive_history]") {
    utp_receive_history_t      history = {};
    const utp_receive_range_t *range;

    REQUIRE(utp_receive_history_init(&history, nullptr, 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 10u, 10u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 20u, 20u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 30u, 30u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_stop_wait(&history, 20u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_cutoff(&history) == 20u);
    REQUIRE(utp_receive_history_contains(&history, 10u));
    REQUIRE(utp_receive_history_contains(&history, 19u));
    REQUIRE(utp_receive_history_contains(&history, 20u));
    REQUIRE(utp_receive_history_range_count(&history) == 2u);

    REQUIRE(utp_receive_history_insert(&history, 40u, 40u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 50u, 50u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_cutoff(&history) == 21u);
    REQUIRE(utp_receive_history_contains(&history, 20u));
    REQUIRE_FALSE(utp_receive_history_contains(&history, 21u));
    REQUIRE(utp_receive_history_range_count(&history) == 3u);
    range = utp_receive_history_range_at(&history, 2u);
    REQUIRE(range != nullptr);
    REQUIRE(range->low == 30u);
    REQUIRE(range->high == 30u);

    utp_receive_history_cleanup(&history);
}

TEST_CASE("receive history allocates only during initialization", "[receive_history][allocation]") {
    allocation_tracker    tracker   = {};
    const utp_allocator_t allocator = {tracked_alloc, tracked_realloc, tracked_free, &tracker};
    utp_receive_history_t history   = {};

    REQUIRE(utp_receive_history_init(&history, &allocator, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(tracker.allocations == 1u);
    REQUIRE(utp_receive_history_insert(&history, 10u, 10u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 20u, 20u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 30u, 30u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_stop_wait(&history, 25u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(tracker.allocations == 1u);

    utp_receive_history_clear(&history);
    REQUIRE(tracker.allocations == 1u);
    REQUIRE(utp_receive_history_range_count(&history) == 0u);
    utp_receive_history_cleanup(&history);
    REQUIRE(tracker.frees == 1u);
}

TEST_CASE("receive history projects its newest ranges into an ACK", "[receive_history][ack]") {
    utp_receive_history_t history           = {};
    utp_ack_range_t       ranges[2]         = {};
    utp_ack_info_t        ack               = {0u, 0u, ranges, 0u, 2u};
    utp_ack_range_t       decoded_ranges[2] = {};
    utp_ack_info_t        decoded           = {0u, 0u, decoded_ranges, 0u, 2u};
    uint8_t               encoded[UTP_ACK_FRAME_HEADER_SIZE + UTP_ACK_FRAME_RANGE_SIZE] = {};
    size_t                encoded_length                                                = 0u;
    size_t                consumed                                                      = 0u;

    REQUIRE(utp_receive_history_init(&history, nullptr, 4u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 10u, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 30u, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 70u, 300u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_receive_history_insert(&history, 100u, 400u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_ack_from_receive_history(&ack, &history, 500u, 2u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(ack.largest_acked == 100u);
    REQUIRE(ack.ack_delay == 100u);
    REQUIRE(ack.range_count == 2u);
    REQUIRE(ack.ranges[0].low == 100u);
    REQUIRE(ack.ranges[1].high == 70u);
    REQUIRE(utp_ack_encode(encoded, sizeof(encoded), &ack, 0u, &encoded_length) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_ack_decode(&decoded, encoded, encoded_length, 0u, &consumed) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(consumed == encoded_length);
    REQUIRE(decoded.largest_acked == ack.largest_acked);
    REQUIRE(decoded.range_count == ack.range_count);
    REQUIRE(decoded.ranges[1].low == ack.ranges[1].low);

    utp_receive_history_cleanup(&history);
}

TEST_CASE("ACK scheduler sends immediately for thresholds and otherwise exposes a deadline", "[ack_scheduler]") {
    utp_ack_scheduler_t scheduler = {};

    REQUIRE(utp_ack_scheduler_init(&scheduler, 2u, 3u, 25u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_ack_scheduler_on_packet(&scheduler, 10u, 9u, true, false, 100u) == UTP_ACK_SCHEDULE_DELAYED);
    REQUIRE(utp_ack_scheduler_deadline(&scheduler) == 25100u);
    REQUIRE(utp_ack_scheduler_on_packet(&scheduler, 11u, 10u, true, false, 200u) == UTP_ACK_SCHEDULE_IMMEDIATE);
    utp_ack_scheduler_on_ack_sent(&scheduler);
    REQUIRE(utp_ack_scheduler_pending_count(&scheduler) == 0u);
    REQUIRE(utp_ack_scheduler_on_packet(&scheduler, 20u, 10u, true, false, 300u) == UTP_ACK_SCHEDULE_IMMEDIATE);
}

TEST_CASE("send history advances monotonically and records one packet-number gap", "[send_history]") {
    utp_send_history_t history = {};

    utp_send_history_init(&history, 5u);
    REQUIRE(utp_send_history_update(&history, 1u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_history_largest(&history) == 1u);
    REQUIRE_FALSE(utp_send_history_gap_detected(&history));
    REQUIRE(utp_send_history_update(&history, 7u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_history_largest(&history) == 7u);
    REQUIRE(utp_send_history_gap_detected(&history));
    REQUIRE(utp_send_history_update(&history, 3u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_send_history_largest(&history) == 7u);
}

TEST_CASE("RTT statistics use RFC 6298 smoothing", "[rtt]") {
    utp_rtt_stats_t stats = {};

    REQUIRE(utp_rtt_stats_update(&stats, 100u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_rtt_stats_srtt(&stats) == 100u);
    REQUIRE(utp_rtt_stats_variance(&stats) == 50u);
    REQUIRE(utp_rtt_stats_minimum(&stats) == 100u);
    REQUIRE(utp_rtt_stats_update(&stats, 200u) == UTP_INTERNAL_ERROR_OK);
    REQUIRE(utp_rtt_stats_srtt(&stats) == 112u);
    REQUIRE(utp_rtt_stats_variance(&stats) == 63u);
    REQUIRE(utp_rtt_stats_minimum(&stats) == 100u);
}
