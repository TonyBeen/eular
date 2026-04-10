/*************************************************************************
    > File Name: test_bimap.cc
    > Author: hsz
    > Brief: --benchmark-samples=1
    > Created Time: 2024年03月26日 星期二 10时37分33秒
 ************************************************************************/

#ifndef CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#endif

#include <numeric>
#include <string>
#include <random>
#include <vector>

#include "catch/catch.hpp"
#include "utils/bimap.h"

namespace {

constexpr uint32_t kBenchmarkDataSize = 500000;
constexpr uint32_t kBenchmarkSeed = 0x5EED1234u;

std::vector<uint32_t> makeSequentialKeys(uint32_t size) {
    std::vector<uint32_t> keys(size);
    std::iota(keys.begin(), keys.end(), 0u);
    return keys;
}

std::vector<uint32_t> makeShuffledKeys(uint32_t size, uint32_t seed) {
    std::vector<uint32_t> keys = makeSequentialKeys(size);
    std::mt19937 rng(seed);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

eular::BiMap<uint32_t, uint64_t> makeBenchmarkBimap(uint32_t size) {
    eular::BiMap<uint32_t, uint64_t> bimap;
    for (uint32_t i = 0; i < size; ++i) {
        REQUIRE(bimap.insert(i, i));
    }
    REQUIRE(size == bimap.size());
    return bimap;
}

} // namespace

TEST_CASE("test_bimap_insert", "[BiMap]") {
    eular::BiMap<uint32_t, std::string> bimap = {
        {1,     "One"},
        {2,     "Two"},
        {3,     "Three"},
        {4,     "Four"},
        {5,     "Five"},
        {6,     "Six"},
        {7,     "Seven"},
        {8,     "Eight"},
        {9,     "Nine"},
        {10,    "Ten"}
    };

    REQUIRE(bimap.size() == 10);

    uint32_t key = 11;
    std::string value = "eleven";

    REQUIRE(bimap.insert(key, value));

    REQUIRE(bimap.insert(key, "222") == false);
    REQUIRE(bimap.insert(1111, value) == false);
}

TEST_CASE("insert benchmark", "[BiMap]") {
    auto insertKeys = makeSequentialKeys(kBenchmarkDataSize);

    BENCHMARK_ADVANCED("BiMap insert avg/op (single insert into growing map, fixed-seed workload)")
    (Catch::Benchmark::Chronometer meter) {
        eular::BiMap<uint32_t, uint64_t> bimap;
        REQUIRE(static_cast<uint32_t>(meter.runs()) <= kBenchmarkDataSize);

        meter.measure([&](int i) {
            const uint32_t key = insertKeys[static_cast<size_t>(i)];
            REQUIRE(bimap.insert(key, key));
        });

        REQUIRE(bimap.size() == static_cast<size_t>(meter.runs()));
    };
}

TEST_CASE("test find", "[BiMap]") {
    eular::BiMap<uint32_t, std::string> bimap = {
        {1,     "One"},
        {2,     "Two"},
        {3,     "Three"},
        {4,     "Four"},
        {5,     "Five"},
        {6,     "Six"},
        {7,     "Seven"},
        {8,     "Eight"},
        {9,     "Nine"},
        {10,    "Ten"}
    };

    REQUIRE(bimap.size() == 10);

    auto it = bimap.find("Six");
    REQUIRE(6 == it.key());

    it = bimap.find(10);
    REQUIRE(it.value() == "Ten");
}

TEST_CASE("find benchmark", "[BiMap]") {
    auto queryKeys = makeShuffledKeys(kBenchmarkDataSize, kBenchmarkSeed);
    eular::BiMap<uint32_t, uint64_t> bimap = makeBenchmarkBimap(kBenchmarkDataSize);

    BENCHMARK_ADVANCED("BiMap find by key avg/op (500000 entries, fixed seed)")
    (Catch::Benchmark::Chronometer meter) {
        REQUIRE(static_cast<uint32_t>(meter.runs()) <= kBenchmarkDataSize);
        meter.measure([&](int i) {
            const uint32_t key = queryKeys[static_cast<size_t>(i)];
            auto it = bimap.find(key);
            REQUIRE(it.value() == key);
        });
    };

    BENCHMARK_ADVANCED("BiMap find by value avg/op (500000 entries, fixed seed)")
    (Catch::Benchmark::Chronometer meter) {
        REQUIRE(static_cast<uint32_t>(meter.runs()) <= kBenchmarkDataSize);
        meter.measure([&](int i) {
            const uint64_t value = queryKeys[static_cast<size_t>(i)];
            auto it = bimap.find(value);
            REQUIRE(it.key() == value);
        });
    };
}

TEST_CASE("test erase", "[BiMap]") {

    eular::BiMap<uint32_t, std::string> bimap = {
        {1,     "One"},
        {2,     "Two"},
        {3,     "Three"},
        {4,     "Four"},
        {5,     "Five"},
        {6,     "Six"},
        {7,     "Seven"},
        {8,     "Eight"},
        {9,     "Nine"},
        {10,    "Ten"}
    };

    REQUIRE(bimap.size() == 10);

    bimap.erase(1);
    auto it = bimap.find("One");
    REQUIRE(it == bimap.end());

    bimap.erase("Five");
    it = bimap.find(5);
    REQUIRE(it == bimap.end());
}

TEST_CASE("erase benchmark", "[BiMap]") {
    auto eraseKeys = makeShuffledKeys(kBenchmarkDataSize, kBenchmarkSeed + 1);

    BENCHMARK_ADVANCED("BiMap erase by key avg/op (500000 entries, fixed seed)")
    (Catch::Benchmark::Chronometer meter) {
        REQUIRE(static_cast<uint32_t>(meter.runs()) <= kBenchmarkDataSize);
        eular::BiMap<uint32_t, uint64_t> bimap = makeBenchmarkBimap(kBenchmarkDataSize);

        meter.measure([&](int i) {
            const uint32_t key = eraseKeys[static_cast<size_t>(i)];
            bimap.erase(key);
            auto it = bimap.find(key);
            REQUIRE(it == bimap.end());
        });

        REQUIRE(bimap.size() == kBenchmarkDataSize - static_cast<size_t>(meter.runs()));
    };

    BENCHMARK_ADVANCED("BiMap erase by value avg/op (500000 entries, fixed seed)")
    (Catch::Benchmark::Chronometer meter) {
        REQUIRE(static_cast<uint32_t>(meter.runs()) <= kBenchmarkDataSize);
        eular::BiMap<uint32_t, uint64_t> bimap = makeBenchmarkBimap(kBenchmarkDataSize);

        meter.measure([&](int i) {
            const uint64_t value = eraseKeys[static_cast<size_t>(i)];
            bimap.erase(value);
            auto it = bimap.find(value);
            REQUIRE(it == bimap.end());
        });

        REQUIRE(bimap.size() == kBenchmarkDataSize - static_cast<size_t>(meter.runs()));
    };
}

TEST_CASE("test foreach", "[BiMap]") {

    std::initializer_list<std::pair<uint32_t, std::string>> initList = {
        {1,     "One"},
        {2,     "Two"},
        {3,     "Three"},
        {4,     "Four"},
        {5,     "Five"},
        {6,     "Six"},
        {7,     "Seven"},
        {8,     "Eight"},
        {9,     "Nine"},
        {10,    "Ten"}
    };

    eular::BiMap<uint32_t, std::string> bimap = initList;

    REQUIRE(bimap.size() == 10);

    auto listIt = initList.begin();
    for (auto it = bimap.begin(); it != bimap.end(); ++it, ++listIt)
    {
        // NOTE 遍历时以key为关键字进行的, 底层采用红黑树, 故是一个有序的结构
        REQUIRE(it.key() == listIt->first);
        REQUIRE(it.value() == listIt->second);
    }

    listIt = initList.begin();
    // NOTE 遍历时更新key会产生未定义行为, 比如迭代器失效, 更新value则不会
    for (auto it = bimap.begin(); it != bimap.end(); ++it, ++listIt)
    {
        if (it.key() == 5)
        {
            it.update("five");
        }
        REQUIRE(it.key() == listIt->first);
    }
}

TEST_CASE("test replace", "[BiMap]") {
    std::initializer_list<std::pair<uint32_t, std::string>> initList = {
        {1,     "One"},
        {2,     "Two"},
        {3,     "Three"},
        {4,     "Four"},
        {5,     "Five"},
        {6,     "Six"},
        {7,     "Seven"},
        {8,     "Eight"},
        {9,     "Nine"},
        {10,    "Ten"}
    };

    eular::BiMap<uint32_t, std::string> bimap = initList;

    REQUIRE(bimap.size() == 10);

    // 将One的键更新为11
    REQUIRE_NOTHROW(bimap.replaceKey("One", 11));

    // 将键为11的值更新为Eleven
    REQUIRE_NOTHROW(bimap.replaceValue(11, "Eleven"));

    // 更新一个不存在的键值对
    REQUIRE_NOTHROW(bimap.replaceValue(12, "Twelve"));

    REQUIRE(bimap.contains(12));
    REQUIRE(bimap.contains("Twelve"));

    // 更新一个已存在的值, 会导致抛出异常
    REQUIRE_THROWS(bimap.replaceKey("Eleven", 11));
    REQUIRE_THROWS(bimap.replaceValue(11, "Eleven"));
}

TEST_CASE("test move", "[BiMap]") {
    std::initializer_list<std::pair<uint32_t, std::string>> initList = {
        {1,     "One"},
        {2,     "Two"},
        {3,     "Three"},
        {4,     "Four"},
        {5,     "Five"},
        {6,     "Six"},
        {7,     "Seven"},
        {8,     "Eight"},
        {9,     "Nine"},
        {10,    "Ten"}
    };

    {
        eular::BiMap<uint32_t, std::string> bimap = initList;
        REQUIRE(bimap.size() == initList.size());

        decltype(bimap) otherBiMap(std::move(bimap));
        REQUIRE(otherBiMap.size() == initList.size());
        REQUIRE(bimap.size() == 0);

        auto listIt = initList.begin();
        for (auto it = bimap.begin(); it != bimap.end(); ++it, ++listIt)
        {
            REQUIRE(it.key() == listIt->first);
            REQUIRE(it.value() == listIt->second);
        }
    }

    {
        eular::BiMap<uint32_t, std::string> bimap = initList;
        REQUIRE(bimap.size() == initList.size());

        decltype(bimap) otherBiMap;
        otherBiMap = std::move(bimap);

        REQUIRE(bimap.size() == 0);
        REQUIRE(otherBiMap.size() == initList.size());

        auto listIt = initList.begin();
        for (auto it = bimap.begin(); it != bimap.end(); ++it, ++listIt)
        {
            REQUIRE(it.key() == listIt->first);
            REQUIRE(it.value() == listIt->second);
        }
    }
}