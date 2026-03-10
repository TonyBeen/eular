/*************************************************************************
    > File Name: test_receive_history.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 25 Feb 2026 02:24:26 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <catch2/catch.hpp>

#include <vector>
#include <utility>
#include <cstdint>

#include "util/receive_history.h"

using eular::utp::ReceiveHistory;

static std::vector<std::pair<uint64_t, uint64_t>>
collectRanges(const ReceiveHistory& hist)
{
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (auto it = hist.begin(); it != hist.end(); ++it)
    {
        const auto& r = *it;
        out.emplace_back(static_cast<uint64_t>(r.low),
                         static_cast<uint64_t>(r.high));
    }
    return out;
}

TEST_CASE("ReceiveHistory: insert into empty creates one range", "[ReceiveHistory]")
{
    ReceiveHistory hist(256);

    REQUIRE(hist.empty());
    hist.insert(10, 1000);

    REQUIRE_FALSE(hist.empty());
    REQUIRE(hist.largest() == 10);
    REQUIRE(hist.rangeCount() == 1);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(10, 10));
}

TEST_CASE("ReceiveHistory: inserting same pn is idempotent", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(10, 1000);
    hist.insert(10, 2000); // should not change ranges; largest remains 10

    REQUIRE(hist.largest() == 10);
    REQUIRE(hist.rangeCount() == 1);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(10, 10));
}

TEST_CASE("ReceiveHistory: right extension merges adjacent on the right", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(10, 1000);
    hist.insert(11, 1100); // extends [10,10] -> [10,11]

    REQUIRE(hist.largest() == 11);
    REQUIRE(hist.rangeCount() == 1);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(10, 11));
}

TEST_CASE("ReceiveHistory: left extension merges adjacent on the left", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(11, 1000);
    hist.insert(10, 1100); // left-extends [11,11] -> [10,11]

    REQUIRE(hist.largest() == 11);
    REQUIRE(hist.rangeCount() == 1);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(10, 11));
}

TEST_CASE("ReceiveHistory: bridge insertion merges two ranges into one", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    // Two separated singletons
    hist.insert(12, 1000);
    hist.insert(10, 1000);

    // Should be two ranges: [12,12] then [10,10] (descending order)
    {
        auto ranges = collectRanges(hist);
        REQUIRE(ranges.size() == 2);
        REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(12, 12));
        REQUIRE(ranges[1] == std::make_pair<uint64_t, uint64_t>(10, 10));
    }

    // Insert 11 bridges them: expect [10,12]
    hist.insert(11, 1500);

    REQUIRE(hist.largest() == 12);
    REQUIRE(hist.rangeCount() == 1);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(10, 12));
}

TEST_CASE("ReceiveHistory: iteration order is by descending packet numbers (newest first)", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(5, 1000);
    hist.insert(100, 1000);
    hist.insert(50, 1000);

    auto ranges = collectRanges(hist);
    // Each is singleton range; order should be 100, 50, 5
    REQUIRE(ranges.size() == 3);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(100, 100));
    REQUIRE(ranges[1] == std::make_pair<uint64_t, uint64_t>(50, 50));
    REQUIRE(ranges[2] == std::make_pair<uint64_t, uint64_t>(5, 5));
}

TEST_CASE("ReceiveHistory: stopWait deletes ranges completely below cutoff", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    // Create ranges: [100,100], [50,50], [10,12]
    hist.insert(12, 0);
    hist.insert(11, 0);
    hist.insert(10, 0);
    hist.insert(50, 0);
    hist.insert(100, 0);

    {
        auto ranges = collectRanges(hist);
        REQUIRE(ranges.size() == 3);
        REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(100, 100));
        REQUIRE(ranges[1] == std::make_pair<uint64_t, uint64_t>(50, 50));
        REQUIRE(ranges[2] == std::make_pair<uint64_t, uint64_t>(10, 12));
    }

    // cutoff=60 should drop [50,50] and [10,12], leaving [100,100]
    hist.stopWait(60);

    REQUIRE(hist.cutoff() == 60);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(100, 100));
}

TEST_CASE("ReceiveHistory: stopWait truncates a range when cutoff is inside it", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    // One range [10, 20]
    for (uint64_t pn = 10; pn <= 20; ++pn)
        hist.insert(pn, 0);

    REQUIRE(hist.rangeCount() == 1);

    // cutoff=15 should turn [10,20] into [15,20]
    hist.stopWait(15);

    REQUIRE(hist.cutoff() == 15);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(15, 20));
}

TEST_CASE("ReceiveHistory: stopWait is monotonic (smaller/equal cutoff has no effect)", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    for (uint64_t pn = 10; pn <= 12; ++pn)
        hist.insert(pn, 0);

    hist.stopWait(12);
    auto ranges1 = collectRanges(hist);

    hist.stopWait(11); // should be ignored because m_cutoff >= cutoff
    auto ranges2 = collectRanges(hist);

    REQUIRE(hist.cutoff() == 12);
    REQUIRE(ranges1 == ranges2);
}

TEST_CASE("ReceiveHistory: insert ignores packet numbers below cutoff", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(100, 1000);
    hist.insert(10,  1000);

    // Now set cutoff=50: should drop [10,10]
    hist.stopWait(50);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.size() == 1);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(100, 100));

    // Try insert below cutoff; should be ignored
    hist.insert(49, 2000);

    auto ranges2 = collectRanges(hist);
    REQUIRE(ranges2 == ranges);
    REQUIRE(hist.largest() == 100);
}

TEST_CASE("ReceiveHistory: dropOldestRange is triggered when ranges exceed maxRanges and advances cutoff", "[ReceiveHistory]")
{
    // maxRanges=3, then insert 4 separated pns => 4 ranges => drop oldest (tail)
    ReceiveHistory hist(/*maxRanges=*/3);

    // Create 4 non-adjacent singletons, descending order in list:
    // head: 100, then 80, then 60, then 40 (tail oldest/smallest)
    hist.insert(40, 0);
    hist.insert(60, 0);
    hist.insert(80, 0);
    hist.insert(100, 0);

    // After inserting 4th range, usedCount should be <= 3
    REQUIRE(hist.rangeCount() <= 3);

    // The oldest/smallest range [40,40] should be dropped, cutoff becomes 41
    REQUIRE(hist.cutoff() == 41);

    auto ranges = collectRanges(hist);
    // Remaining should be [100],[80],[60]
    REQUIRE(ranges.size() == 3);
    REQUIRE(ranges[0] == std::make_pair<uint64_t, uint64_t>(100, 100));
    REQUIRE(ranges[1] == std::make_pair<uint64_t, uint64_t>(80, 80));
    REQUIRE(ranges[2] == std::make_pair<uint64_t, uint64_t>(60, 60));

    // Insert 40 again should be ignored due to cutoff
    hist.insert(40, 0);
    auto ranges2 = collectRanges(hist);
    REQUIRE(ranges2 == ranges);
}

TEST_CASE("ReceiveHistory: largestAckedReceived is updated when a new largest pn is inserted", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(10, 1000);
    REQUIRE(hist.largest() == 10);
    REQUIRE(hist.largestAckedReceived() == 1000);

    // Insert smaller pn: should not update largest/time
    hist.insert(9, 2000);
    REQUIRE(hist.largest() == 10);
    REQUIRE(hist.largestAckedReceived() == 1000);

    // Insert larger pn: should update
    hist.insert(11, 3000);
    REQUIRE(hist.largest() == 11);
    REQUIRE(hist.largestAckedReceived() == 3000);
}

TEST_CASE("ReceiveHistory: clear resets state", "[ReceiveHistory]")
{
    ReceiveHistory hist;

    hist.insert(10, 1000);
    hist.stopWait(9);
    REQUIRE_FALSE(hist.empty());

    hist.clear();

    REQUIRE(hist.empty());
    REQUIRE(hist.rangeCount() == 0);
    REQUIRE(hist.cutoff() == 0);
    REQUIRE(hist.largest() == 0);
    REQUIRE(hist.largestAckedReceived() == 0);

    auto ranges = collectRanges(hist);
    REQUIRE(ranges.empty());
}