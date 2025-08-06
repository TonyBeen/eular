/*************************************************************************
    > File Name: test_bimap.cc
    > Author: hsz
    > Brief: --benchmark-samples=1
    > Created Time: 2024年03月26日 星期二 10时37分33秒
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#ifndef CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#endif

#include <string>
#include <random>
#include <thread>

#include "catch/catch.hpp"
#include "utils/bimap.h"

TEST_CASE("test_bimap_insert", "[bimap]") {
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

TEST_CASE("insert benchmark", "[bimap]") {

    {
        uint32_t insertSize = 500000;

        BENCHMARK("Map insert performance") {
            eular::BiMap<uint32_t, uint64_t> bimap;

            for (uint32_t i = 0; i < insertSize; ++i)
            {
                REQUIRE(bimap.insert(i, i));
            }

            REQUIRE(insertSize == bimap.size());

            bimap.clear();
        };
    }
}

TEST_CASE("test find", "[bimap]") {
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

TEST_CASE("find benchmark", "[bimap]") {

    {
        uint32_t insertSize = 500000;

        eular::BiMap<uint32_t, uint64_t> bimap;

        for (uint32_t i = 0; i < insertSize; ++i)
        {
            REQUIRE(bimap.insert(i, i));
        }

        REQUIRE(insertSize == bimap.size());

        BENCHMARK("Map find by key performance") {
            std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
            std::chrono::milliseconds mills =
                std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
            srand(mills.count());

            uint32_t key = rand() % (insertSize - 1);
            auto it = bimap.find(key);
            REQUIRE(it.value() == key);
        };

        BENCHMARK("Map find by value performance") {
            std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
            std::chrono::milliseconds mills =
                std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
            srand(mills.count());

            uint64_t value = rand() % (insertSize - 1);
            auto it = bimap.find(value);
            REQUIRE(it.key() == value);
        };

        bimap.clear();
    }
}

TEST_CASE("test erase", "[bimap]") {

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

TEST_CASE("erase benchmark", "[bimap]") {

    {
        uint32_t insertSize = 500000;

        eular::BiMap<uint32_t, uint64_t> bimap;

        for (uint32_t i = 0; i < insertSize; ++i)
        {
            REQUIRE(bimap.insert(i, i));
        }

        REQUIRE(insertSize == bimap.size());

        BENCHMARK("Map erase by key performance") {
            std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
            std::chrono::milliseconds mills =
                std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
            srand(mills.count());

            uint32_t key = rand() % (insertSize - 1);
            bimap.erase(key);
            auto it = bimap.find(key);
            REQUIRE(it == bimap.end());
        };

        BENCHMARK("Map erase by value performance") {
            std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
            std::chrono::milliseconds mills =
                std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
            srand(mills.count());

            uint64_t value = rand() % (insertSize - 1);
            bimap.erase(value);
            auto it = bimap.find(value);
            REQUIRE(it == bimap.end());
        };

        bimap.clear();
    }
}

TEST_CASE("test foreach", "[bimap]") {

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

TEST_CASE("test replace", "[bimap]") {
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

TEST_CASE("test move", "[bimap]") {
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