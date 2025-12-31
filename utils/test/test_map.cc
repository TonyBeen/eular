/*************************************************************************
    > File Name: test_map.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 05 Dec 2022 09:58:35 AM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <iostream>
#include <stdio.h>
#include <assert.h>

#include <utils/map.h>
#include <utils/string8.h>

#include "catch/catch.hpp"

TEST_CASE("test_insert", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    mapObj.insert("world", 300); // will failed
    CHECK(mapObj.size() == 2);
}

TEST_CASE("test_find", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto it = mapObj.find("hello");
    CHECK(it != mapObj.end());
}

TEST_CASE("test_value", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    const auto &val = mapObj.value("hello");
    CHECK(val == 100);
}

TEST_CASE("test_operator[]", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto &val = mapObj["hello"];
    CHECK(val == 100);

    val = 400;
    CHECK(mapObj.value("hello") == 400);
}

TEST_CASE("test_erase", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto it = mapObj.insert("test", 10000);
    CHECK(it != mapObj.end());

    mapObj.erase("test");
    CHECK(mapObj.find("test") == mapObj.end());
}

TEST_CASE("test_foreach", "[map]") {
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

TEST_CASE("test_foreach_erase", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    mapObj.insert("test1", 300);
    mapObj.insert("test2", 400);
    mapObj.insert("test3", 500);
    CHECK(mapObj.size() == 5);

    for (auto it = mapObj.begin(); it != mapObj.end(); ) {
        if (it.key() == "hello") {
            it = mapObj.erase(it);
        } else {
            ++it;
        }
    }
    CHECK(mapObj.size() == 4);
}

TEST_CASE("test_reforeach", "[map]") {
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

TEST_CASE("test_copy", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    auto mapNew = mapObj;
    mapNew.insert("new", 300); // 与 mapObj 不使用同一个内存
    CHECK(mapNew.size() == 3);
    CHECK(mapNew.value("hello") == 100);
    CHECK(mapNew.value("world") == 200);
}

TEST_CASE("test_assign", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    eular::Map<eular::String8, int> mapNew;
    mapNew = mapObj;
    mapNew.insert("new", 300); // 与 mapObj 不使用同一个内存
    CHECK(mapNew.size() == 3);
    CHECK(mapNew.value("hello") == 100);
    CHECK(mapNew.value("world") == 200);
}

TEST_CASE("test_clear", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    mapObj.clear();
    CHECK(mapObj.size() == 0);
}
