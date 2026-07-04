/*************************************************************************
    > File Name: test_map.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 05 Dec 2022 09:58:35 AM CST
 ************************************************************************/

#include <assert.h>
#include <stdio.h>

#include <iostream>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <utils/map.h>
#include <utils/platform.h>
#include <utils/string8.h>

#include "catch/catch.hpp"

static_assert(!std::is_reference<decltype(std::declval<const eular::Map<eular::String8, int>&>().value("x"))>::value,
              "Map::value should return by value to avoid dangling references");
static_assert(!std::is_reference<decltype(std::declval<const eular::Map<eular::String8, int>&>()["x"])>::value,
              "Map::operator[] const should return by value to avoid dangling references");

struct MergeTrackedValue {
    MergeTrackedValue(int v = 0) : value(v) {}
    MergeTrackedValue(const MergeTrackedValue& other) : value(other.value) { ++copyCount; }
    MergeTrackedValue& operator=(const MergeTrackedValue& other)
    {
        value = other.value;
        ++copyCount;
        return *this;
    }

    int        value;
    static int copyCount;
};

int MergeTrackedValue::copyCount = 0;

struct EmplaceTrackedValue {
    explicit EmplaceTrackedValue(int v) : value(v) { ++directCount; }
    EmplaceTrackedValue(const EmplaceTrackedValue& other) : value(other.value) { ++copyCount; }
    EmplaceTrackedValue(EmplaceTrackedValue&& other) noexcept : value(other.value)
    {
        other.value = -1;
        ++moveCount;
    }

    int        value;
    static int directCount;
    static int copyCount;
    static int moveCount;
};

int EmplaceTrackedValue::directCount = 0;
int EmplaceTrackedValue::copyCount = 0;
int EmplaceTrackedValue::moveCount = 0;

struct RangeEraseTrackedKey {
    RangeEraseTrackedKey(int v = 0) : value(v) {}
    RangeEraseTrackedKey(const RangeEraseTrackedKey& other) : value(other.value) { ++copyCount; }
    RangeEraseTrackedKey& operator=(const RangeEraseTrackedKey& other)
    {
        value = other.value;
        ++copyCount;
        return *this;
    }

    bool operator<(const RangeEraseTrackedKey& other) const { return value < other.value; }

    int        value;
    static int copyCount;
};

int RangeEraseTrackedKey::copyCount = 0;

struct LifetimeTrackedValue {
    LifetimeTrackedValue(int v = 0) : value(v) { ++liveCount; }
    LifetimeTrackedValue(const LifetimeTrackedValue& other) : value(other.value) { ++liveCount; }
    ~LifetimeTrackedValue() { --liveCount; }
    LifetimeTrackedValue& operator=(const LifetimeTrackedValue& other)
    {
        value = other.value;
        return *this;
    }

    int        value;
    static int liveCount;
};

int LifetimeTrackedValue::liveCount = 0;

struct ThrowOnCopyValue {
    ThrowOnCopyValue(int v = 0) : value(v) {}
    ThrowOnCopyValue(const ThrowOnCopyValue& other) : value(other.value)
    {
        if (copiesUntilThrow == 0) {
            throw std::runtime_error("copy failed");
        }
        if (copiesUntilThrow > 0) {
            --copiesUntilThrow;
        }
    }
    ThrowOnCopyValue& operator=(const ThrowOnCopyValue& other)
    {
        value = other.value;
        return *this;
    }

    int        value;
    static int copiesUntilThrow;
};

int ThrowOnCopyValue::copiesUntilThrow = -1;

struct DescendingString8Compare {
    bool operator()(const eular::String8& lhs, const eular::String8& rhs) const { return lhs > rhs; }
};

static constexpr int kMapBenchmarkDataSize = 100000;
static constexpr int kMapRangeEraseBenchmarkDataSize = 50000;

static void stdMapTryEmplace(std::map<int, int>& mapObj, int key, int value)
{
#if __cplusplus >= 201703L
    mapObj.try_emplace(key, value);
#else
    mapObj.emplace(key, value);
#endif
}

TEST_CASE("test_insert", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    auto duplicate = mapObj.insert("world", 300);  // will failed
    CHECK(mapObj.size() == 2);
    CHECK(duplicate == mapObj.end());
    CHECK(mapObj.value("world") == 200);
}

TEST_CASE("test_find", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto it = mapObj.find("hello");
    CHECK(it != mapObj.end());
}

TEST_CASE("test_value", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    const auto& val = mapObj.value("hello");
    CHECK(val == 100);
}

TEST_CASE("test_operator[]", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto& val = mapObj["hello"];
    CHECK(val == 100);

    val = 400;
    CHECK(mapObj.value("hello") == 400);
}

TEST_CASE("test_erase", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto it = mapObj.insert("test", 10000);
    CHECK(it != mapObj.end());

    mapObj.erase("test");
    CHECK(mapObj.find("test") == mapObj.end());
}

TEST_CASE("test_foreach", "[map]")
{
    std::map<std::string, int32_t> stdMap = {
        {"hello", 100},
        {"world", 200},
    };

    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    for (auto it = mapObj.begin(); it != mapObj.end(); ++it) {
        auto stdIt = stdMap.find(it.key().c_str());
        CHECK(stdIt != stdMap.end());
        CHECK(stdIt->second == it.value());
    }
}

TEST_CASE("test_foreach_erase", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    mapObj.insert("test1", 300);
    mapObj.insert("test2", 400);
    mapObj.insert("test3", 500);
    CHECK(mapObj.size() == 5);

    for (auto it = mapObj.begin(); it != mapObj.end();) {
        if (it.key() == "hello") {
            it = mapObj.erase(it);
        } else {
            ++it;
        }
    }
    CHECK(mapObj.size() == 4);
}

TEST_CASE("test_reforeach", "[map]")
{
    std::map<std::string, int32_t> stdMap = {
        {"hello", 100},
        {"world", 200},
    };

    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    for (auto it = mapObj.rbegin(); it != mapObj.rend(); --it) {
        auto stdIt = stdMap.find(it.key().c_str());
        CHECK(stdIt != stdMap.end());
        CHECK(stdIt->second == it.value());
    }
}

TEST_CASE("test_copy", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto mapNew = mapObj;
    mapNew.insert("new", 300);  // 与 mapObj 不使用同一个内存
    CHECK(mapNew.size() == 3);
    CHECK(mapNew.value("hello") == 100);
    CHECK(mapNew.value("world") == 200);
}

TEST_CASE("test_assign", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    eular::Map<eular::String8, int> mapNew;
    mapNew = mapObj;
    mapNew.insert("new", 300);  // 与 mapObj 不使用同一个内存
    CHECK(mapNew.size() == 3);
    CHECK(mapNew.value("hello") == 100);
    CHECK(mapNew.value("world") == 200);
}

TEST_CASE("test_empty_map_lazy_paths", "[map]")
{
    eular::Map<eular::String8, int> empty;
    const auto&                     constEmpty = empty;

    CHECK(empty.size() == 0);
    CHECK(constEmpty.begin() == constEmpty.end());
    CHECK(constEmpty.rbegin() == constEmpty.rend());
    CHECK(constEmpty.find("missing") == constEmpty.end());
    CHECK(constEmpty.value("missing", 42) == 42);

    eular::Map<eular::String8, int> copied(empty);
    CHECK(copied.size() == 0);
    copied.insert("copied", 1);
    CHECK(copied.size() == 1);
    CHECK(empty.size() == 0);

    eular::Map<eular::String8, int> assigned;
    assigned = empty;
    CHECK(assigned.size() == 0);
    assigned.insert("assigned", 2);
    CHECK(assigned.size() == 1);
    CHECK(empty.size() == 0);

    eular::Map<eular::String8, int> moved(std::move(empty));
    CHECK(moved.size() == 0);
    CHECK(empty.size() == 0);

    eular::Map<eular::String8, int> initListEmpty({});
    CHECK(initListEmpty.size() == 0);
}

TEST_CASE("test_empty_map_mutable_iteration_and_erase", "[map]")
{
    eular::Map<eular::String8, int> mapObj;

    for (auto it = mapObj.begin(); it != mapObj.end(); ++it) {
        REQUIRE(false);
    }
    CHECK(mapObj.size() == 0);

    auto eraseMissing = mapObj.erase("missing");
    CHECK(eraseMissing == mapObj.end());
    CHECK(mapObj.size() == 0);

    auto eraseEnd = mapObj.erase(mapObj.end());
    CHECK(eraseEnd == mapObj.end());
    CHECK(mapObj.size() == 0);

    mapObj.clear();
    CHECK(mapObj.size() == 0);

    mapObj.insert("after_empty_ops", 7);
    CHECK(mapObj.size() == 1);
    CHECK(mapObj.value("after_empty_ops") == 7);
}

TEST_CASE("test_contains_and_const_iterators_do_not_detach", "[map]")
{
    eular::Map<eular::String8, MergeTrackedValue> mapObj;
    mapObj.insert("alpha", MergeTrackedValue(1));
    mapObj.insert("bravo", MergeTrackedValue(2));

    eular::Map<eular::String8, MergeTrackedValue> shared(mapObj);
    MergeTrackedValue::copyCount = 0;

    CHECK(mapObj.contains("alpha"));
    CHECK_FALSE(mapObj.contains("charlie"));
    CHECK(MergeTrackedValue::copyCount == 0);

    const auto& constMapObj = mapObj;
    CHECK(constMapObj.cbegin() == constMapObj.begin());
    CHECK(constMapObj.cend() == constMapObj.end());
    CHECK(constMapObj.crbegin() == constMapObj.rbegin());
    CHECK(constMapObj.crend() == constMapObj.rend());
    CHECK_FALSE(constMapObj.empty());

    CHECK(shared.contains("bravo"));
    CHECK(MergeTrackedValue::copyCount == 0);
}

TEST_CASE("test_emplace_constructs_value_in_place", "[map]")
{
    EmplaceTrackedValue::directCount = 0;
    EmplaceTrackedValue::copyCount = 0;
    EmplaceTrackedValue::moveCount = 0;

    eular::Map<eular::String8, EmplaceTrackedValue> mapObj;
    auto                                            inserted = mapObj.emplace("alpha", 7);

    REQUIRE(inserted != mapObj.end());
    CHECK(inserted.value().value == 7);
    CHECK(EmplaceTrackedValue::directCount == 1);
    CHECK(EmplaceTrackedValue::copyCount == 0);
    CHECK(EmplaceTrackedValue::moveCount == 0);

    EmplaceTrackedValue::directCount = 0;
    auto duplicate = mapObj.emplace("alpha", 9);
    CHECK(duplicate == mapObj.end());
    CHECK(EmplaceTrackedValue::directCount == 0);
    CHECK(mapObj.find("alpha").value().value == 7);

    EmplaceTrackedValue movable(42);
    EmplaceTrackedValue::directCount = 0;
    EmplaceTrackedValue::copyCount = 0;
    EmplaceTrackedValue::moveCount = 0;

    auto moved = mapObj.emplace(eular::String8("bravo"), std::move(movable));
    REQUIRE(moved != mapObj.end());
    CHECK(moved.value().value == 42);
    CHECK(EmplaceTrackedValue::directCount == 0);
    CHECK(EmplaceTrackedValue::copyCount == 0);
    CHECK(EmplaceTrackedValue::moveCount == 1);
}

TEST_CASE("test_swap_maps", "[map]")
{
    eular::Map<eular::String8, int> left;
    eular::Map<eular::String8, int> right;
    left.insert("left", 1);
    right.insert("right", 2);

    left.swap(right);

    CHECK(left.size() == 1);
    CHECK(right.size() == 1);
    CHECK(left.contains("right"));
    CHECK(right.contains("left"));
    CHECK(left.value("right") == 2);
    CHECK(right.value("left") == 1);

    eular::Map<eular::String8, int> empty;
    left.swap(empty);
    CHECK(left.empty());
    CHECK(empty.contains("right"));
}

TEST_CASE("test_erase_range", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("alpha", 1);
    mapObj.insert("bravo", 2);
    mapObj.insert("charlie", 3);
    mapObj.insert("delta", 4);

    auto first = mapObj.find("bravo");
    auto last = mapObj.find("delta");
    auto next = mapObj.erase(first, last);

    CHECK(next != mapObj.end());
    CHECK(next.key() == "delta");
    CHECK(mapObj.size() == 2);
    CHECK(mapObj.contains("alpha"));
    CHECK_FALSE(mapObj.contains("bravo"));
    CHECK_FALSE(mapObj.contains("charlie"));
    CHECK(mapObj.contains("delta"));

    next = mapObj.erase(mapObj.begin(), mapObj.end());
    CHECK(next == mapObj.end());
    CHECK(mapObj.empty());
}

TEST_CASE("test_erase_range_does_not_copy_keys_when_detached", "[map]")
{
    eular::Map<RangeEraseTrackedKey, int> mapObj;
    for (int i = 0; i < 8; ++i) {
        mapObj.emplace(RangeEraseTrackedKey(i), i);
    }

    auto first = mapObj.find(RangeEraseTrackedKey(2));
    auto last = mapObj.find(RangeEraseTrackedKey(6));
    RangeEraseTrackedKey::copyCount = 0;

    auto next = mapObj.erase(first, last);

    REQUIRE(next != mapObj.end());
    CHECK(next.key().value == 6);
    CHECK(RangeEraseTrackedKey::copyCount == 0);
    CHECK(mapObj.size() == 4);
    CHECK_FALSE(mapObj.contains(RangeEraseTrackedKey(2)));
    CHECK_FALSE(mapObj.contains(RangeEraseTrackedKey(5)));
    CHECK(mapObj.contains(RangeEraseTrackedKey(6)));
}

TEST_CASE("test_erase_range_from_shared_copy_detaches", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("alpha", 1);
    mapObj.insert("bravo", 2);
    mapObj.insert("charlie", 3);

    eular::Map<eular::String8, int> shared(mapObj);
    auto                            first = mapObj.find("alpha");
    auto                            last = mapObj.find("charlie");
    mapObj.erase(first, last);

    CHECK(mapObj.size() == 1);
    CHECK(mapObj.contains("charlie"));
    CHECK(shared.size() == 3);
    CHECK(shared.contains("alpha"));
    CHECK(shared.contains("bravo"));
    CHECK(shared.contains("charlie"));
}

TEST_CASE("test_move_assign_releases_old_data", "[map]")
{
    LifetimeTrackedValue::liveCount = 0;
    {
        eular::Map<int, LifetimeTrackedValue> dst;
        dst.insert(1, LifetimeTrackedValue(1));

        eular::Map<int, LifetimeTrackedValue> src;
        src.insert(2, LifetimeTrackedValue(2));

        dst = std::move(src);

        CHECK(dst.size() == 1);
        CHECK(dst.find(2) != dst.end());
        CHECK(src.size() == 0);
    }
    CHECK(LifetimeTrackedValue::liveCount == 0);
}

TEST_CASE("test_detach_exception_keeps_shared_data_valid", "[map]")
{
    ThrowOnCopyValue::copiesUntilThrow = -1;
    eular::Map<int, ThrowOnCopyValue> mapObj;
    mapObj.insert(1, ThrowOnCopyValue(10));
    mapObj.insert(2, ThrowOnCopyValue(20));

    eular::Map<int, ThrowOnCopyValue> shared(mapObj);

    ThrowOnCopyValue::copiesUntilThrow = 0;
    CHECK_THROWS(mapObj.begin());
    ThrowOnCopyValue::copiesUntilThrow = -1;

    CHECK(mapObj.size() == 2);
    CHECK(shared.size() == 2);
    CHECK(mapObj.find(1).value().value == 10);
    CHECK(shared.find(2).value().value == 20);

    mapObj.insert(3, ThrowOnCopyValue(30));

    CHECK(mapObj.size() == 3);
    CHECK(shared.size() == 2);
    CHECK(shared.find(3) == shared.end());
}

TEST_CASE("test_erase_iterator_from_shared_copy", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    mapObj.insert("test", 300);

    auto it = mapObj.find("world");
    auto shared = mapObj;
    auto next = mapObj.erase(it);

    CHECK(mapObj.size() == 2);
    CHECK(shared.size() == 3);
    CHECK(mapObj.find("world") == mapObj.end());
    CHECK(shared.find("world") != shared.end());
    CHECK(next == mapObj.end());
}

TEST_CASE("test_const_value_default_returns_value", "[map]")
{
    const eular::Map<eular::String8, int> mapObj;
    int                                   value = mapObj.value("missing", 1234);
    CHECK(value == 1234);
}

TEST_CASE("test_iterator_increment_and_decrement", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("alpha", 1);
    mapObj.insert("bravo", 2);
    mapObj.insert("charlie", 3);

    auto it = mapObj.begin();
    CHECK(it.key() == "alpha");

    auto post = it++;
    CHECK(post.key() == "alpha");
    CHECK(it.key() == "bravo");

    auto pre = ++it;
    CHECK(pre == it);
    CHECK(it.key() == "charlie");

    auto tail = mapObj.end();
    --tail;
    CHECK(tail.key() == "charlie");

    auto tail_post = tail--;
    CHECK(tail_post.key() == "charlie");
    CHECK(tail.key() == "bravo");
}

TEST_CASE("test_begin_end_on_shared_copy_do_not_mutate_peer", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("alpha", 1);
    mapObj.insert("bravo", 2);

    eular::Map<eular::String8, int> shared(mapObj);

    auto it = mapObj.begin();
    CHECK(it.key() == "alpha");
    it.value() = 100;

    CHECK(mapObj.value("alpha") == 100);
    CHECK(shared.value("alpha") == 1);
    CHECK(mapObj.end() == mapObj.end());
    CHECK(shared.begin().value() == 1);
}

TEST_CASE("test_erase_end_is_noop", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);

    auto next = mapObj.erase(mapObj.end());

    CHECK(next == mapObj.end());
    CHECK(mapObj.size() == 1);
    CHECK(mapObj.value("hello") == 100);
}

TEST_CASE("test_const_operator_brackets_returns_copy", "[map]")
{
    eular::Map<eular::String8, int> source;
    source.insert("hello", 100);

    const eular::Map<eular::String8, int> mapObj(source);
    int                                   value = mapObj["hello"];
    int                                   missing = mapObj["missing"];

    CHECK(value == 100);
    CHECK(missing == 0);
}

TEST_CASE("test_merge_moves_unique_nodes_without_extra_copy", "[map]")
{
    eular::Map<eular::String8, MergeTrackedValue> dst;
    eular::Map<eular::String8, MergeTrackedValue> src;

    dst.insert("keep", MergeTrackedValue(1));
    dst.insert("dup", MergeTrackedValue(2));
    src.insert("dup", MergeTrackedValue(20));
    src.insert("move", MergeTrackedValue(30));

    auto moveIt = src.find("move");
    REQUIRE(moveIt != src.end());
    MergeTrackedValue* movedPtr = &moveIt.value();

    MergeTrackedValue::copyCount = 0;
    dst.merge(src);

    CHECK(MergeTrackedValue::copyCount == 0);
    CHECK(dst.size() == 3);
    CHECK(src.size() == 1);
    CHECK(dst.find("move") != dst.end());
    CHECK(src.find("move") == src.end());
    CHECK(src.find("dup") != src.end());
    CHECK(dst.find("dup").value().value == 2);
    CHECK(&dst.find("move").value() == movedPtr);
}

TEST_CASE("test_merge_preserves_shared_copies", "[map]")
{
    eular::Map<eular::String8, int> dst;
    eular::Map<eular::String8, int> src;
    dst.insert("left", 1);
    src.insert("right", 2);

    eular::Map<eular::String8, int> dstShared(dst);
    eular::Map<eular::String8, int> srcShared(src);

    dst.merge(src);

    CHECK(dst.size() == 2);
    CHECK(src.size() == 0);

    const auto& constDstShared = dstShared;
    const auto& constSrcShared = srcShared;
    CHECK(constDstShared.find("right") == constDstShared.end());
    CHECK(constSrcShared.find("right") != constSrcShared.end());
    CHECK(constSrcShared.find("right").value() == 2);
}

TEST_CASE("test_custom_compare_orders_map", "[map]")
{
    eular::Map<eular::String8, int, DescendingString8Compare> mapObj;
    mapObj.insert("alpha", 1);
    mapObj.insert("charlie", 3);
    mapObj.insert("bravo", 2);

    auto it = mapObj.begin();
    REQUIRE(it != mapObj.end());
    CHECK(it.key() == "charlie");
    ++it;
    REQUIRE(it != mapObj.end());
    CHECK(it.key() == "bravo");
    ++it;
    REQUIRE(it != mapObj.end());
    CHECK(it.key() == "alpha");
}

TEST_CASE("test_nonconst_find_detaches_shared_data", "[map]")
{
    eular::Map<eular::String8, MergeTrackedValue> mapObj;
    mapObj.insert("alpha", MergeTrackedValue(1));
    mapObj.insert("bravo", MergeTrackedValue(2));

    eular::Map<eular::String8, MergeTrackedValue> shared(mapObj);
    MergeTrackedValue::copyCount = 0;
    const auto& constMapObj = mapObj;
    const auto& constShared = shared;

    auto it = mapObj.find("alpha");
    REQUIRE(it != constMapObj.end());
    CHECK(it.value().value == 1);
    CHECK(MergeTrackedValue::copyCount == 2);

    it.value().value = 10;
    CHECK(mapObj.value("alpha").value == 10);
    CHECK(shared.value("alpha").value == 1);

    auto sharedIt = shared.find("alpha");
    REQUIRE(sharedIt != constShared.end());
    CHECK(sharedIt.value().value == 1);
}

TEST_CASE("test_clear_shared_map_keeps_lazy_empty_state", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("alpha", 1);
    mapObj.insert("bravo", 2);

    eular::Map<eular::String8, int> shared(mapObj);
    mapObj.clear();

    CHECK(mapObj.size() == 0);
    CHECK(shared.size() == 2);
    CHECK(shared.value("alpha") == 1);

    const auto& constMapObj = mapObj;
    CHECK(constMapObj.begin() == constMapObj.end());

    mapObj.insert("charlie", 3);
    CHECK(mapObj.size() == 1);
    CHECK(mapObj.value("charlie") == 3);
    CHECK(shared.find("charlie") == shared.end());
}

TEST_CASE("test_clear", "[map]")
{
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    mapObj.clear();
    CHECK(mapObj.size() == 0);
}

TEST_CASE("benchmark_map_insert", "[map][benchmark]")
{
    BENCHMARK("Map insert performance")
    {
        eular::Map<int, int> mapObj;
        for (int i = 0; i < kMapBenchmarkDataSize; ++i) {
            mapObj.insert(i, i);
        }
        return mapObj.size();
    };
}

TEST_CASE("benchmark_std_map_insert", "[map][benchmark]")
{
    BENCHMARK("std::map insert performance")
    {
        std::map<int, int> mapObj;
        for (int i = 0; i < kMapBenchmarkDataSize; ++i) {
            mapObj.emplace(i, i);
        }
        return mapObj.size();
    };
}

TEST_CASE("benchmark_map_emplace", "[map][benchmark]")
{
    BENCHMARK("Map emplace performance")
    {
        eular::Map<int, int> mapObj;
        for (int i = 0; i < kMapBenchmarkDataSize; ++i) {
            mapObj.emplace(i, i);
        }
        return mapObj.size();
    };
}

TEST_CASE("benchmark_std_map_try_emplace", "[map][benchmark]")
{
    BENCHMARK("std::map try_emplace performance")
    {
        std::map<int, int> mapObj;
        for (int i = 0; i < kMapBenchmarkDataSize; ++i) {
            stdMapTryEmplace(mapObj, i, i);
        }
        return mapObj.size();
    };
}

TEST_CASE("benchmark_map_find", "[map][benchmark]")
{
    eular::Map<int, int> mapObj;
    for (int i = 0; i < kMapBenchmarkDataSize; ++i) {
        mapObj.insert(i, i);
    }

    BENCHMARK("Map find performance")
    {
        int  index = kMapBenchmarkDataSize / 2;
        auto it = mapObj.find(index);
        return it != mapObj.end() ? it.value() : -1;
    };
}

TEST_CASE("benchmark_std_map_find", "[map][benchmark]")
{
    std::map<int, int> mapObj;
    for (int i = 0; i < kMapBenchmarkDataSize; ++i) {
        mapObj.emplace(i, i);
    }

    BENCHMARK("std::map find performance")
    {
        int  index = kMapBenchmarkDataSize / 2;
        auto it = mapObj.find(index);
        return it != mapObj.end() ? it->second : -1;
    };
}

TEST_CASE("benchmark_map_erase_range", "[map][benchmark]")
{
    static constexpr int kEraseBegin = kMapRangeEraseBenchmarkDataSize / 4;
    static constexpr int kEraseEnd = kMapRangeEraseBenchmarkDataSize * 3 / 4;

    BENCHMARK_ADVANCED("Map erase range performance")(Catch::Benchmark::Chronometer meter)
    {
        std::vector<eular::Map<int, int>>           maps;
        std::vector<eular::Map<int, int>::iterator> firstIters;
        std::vector<eular::Map<int, int>::iterator> lastIters;
        maps.reserve(static_cast<size_t>(meter.runs()));
        firstIters.reserve(static_cast<size_t>(meter.runs()));
        lastIters.reserve(static_cast<size_t>(meter.runs()));

        for (int run = 0; run < meter.runs(); ++run) {
            maps.emplace_back();
            for (int i = 0; i < kMapRangeEraseBenchmarkDataSize; ++i) {
                maps.back().emplace(i, i);
            }
            firstIters.push_back(maps.back().find(kEraseBegin));
            lastIters.push_back(maps.back().find(kEraseEnd));
        }

        meter.measure([&](int run) {
            size_t index = static_cast<size_t>(run);
            maps[index].erase(firstIters[index], lastIters[index]);
            return maps[index].size();
        });
    };
}

TEST_CASE("benchmark_std_map_erase_range", "[map][benchmark]")
{
    static constexpr int kEraseBegin = kMapRangeEraseBenchmarkDataSize / 4;
    static constexpr int kEraseEnd = kMapRangeEraseBenchmarkDataSize * 3 / 4;

    BENCHMARK_ADVANCED("std::map erase range performance")(Catch::Benchmark::Chronometer meter)
    {
        std::vector<std::map<int, int>>           maps;
        std::vector<std::map<int, int>::iterator> firstIters;
        std::vector<std::map<int, int>::iterator> lastIters;
        maps.reserve(static_cast<size_t>(meter.runs()));
        firstIters.reserve(static_cast<size_t>(meter.runs()));
        lastIters.reserve(static_cast<size_t>(meter.runs()));

        for (int run = 0; run < meter.runs(); ++run) {
            maps.emplace_back();
            for (int i = 0; i < kMapRangeEraseBenchmarkDataSize; ++i) {
                maps.back().emplace(i, i);
            }
            firstIters.push_back(maps.back().find(kEraseBegin));
            lastIters.push_back(maps.back().find(kEraseEnd));
        }

        meter.measure([&](int run) {
            size_t index = static_cast<size_t>(run);
            maps[index].erase(firstIters[index], lastIters[index]);
            return maps[index].size();
        });
    };
}
