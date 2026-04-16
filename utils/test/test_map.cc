/*************************************************************************
    > File Name: test_map.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 05 Dec 2022 09:58:35 AM CST
 ************************************************************************/

#include <iostream>
#include <stdio.h>
#include <assert.h>
#include <type_traits>

#include <utils/map.h>
#include <utils/string8.h>

#include "catch/catch.hpp"

static_assert(!std::is_reference<decltype(std::declval<const eular::Map<eular::String8, int> &>().value("x"))>::value,
              "Map::value should return by value to avoid dangling references");
static_assert(!std::is_reference<decltype(std::declval<const eular::Map<eular::String8, int> &>()["x"])>::value,
              "Map::operator[] const should return by value to avoid dangling references");

TEST_CASE("test_insert", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    auto duplicate = mapObj.insert("world", 300); // will failed
    CHECK(mapObj.size() == 2);
    CHECK(duplicate == mapObj.end());
    CHECK(mapObj.value("world") == 200);
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

TEST_CASE("test_erase_iterator_from_shared_copy", "[map]") {
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

TEST_CASE("test_const_value_default_returns_value", "[map]") {
    const eular::Map<eular::String8, int> mapObj;
    int value = mapObj.value("missing", 1234);
    CHECK(value == 1234);
}

TEST_CASE("test_iterator_increment_and_decrement", "[map]") {
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

TEST_CASE("test_begin_end_on_shared_copy_do_not_mutate_peer", "[map]") {
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

TEST_CASE("test_erase_end_is_noop", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);

    auto next = mapObj.erase(mapObj.end());

    CHECK(next == mapObj.end());
    CHECK(mapObj.size() == 1);
    CHECK(mapObj.value("hello") == 100);
}

TEST_CASE("test_const_operator_brackets_returns_copy", "[map]") {
    eular::Map<eular::String8, int> source;
    source.insert("hello", 100);

    const eular::Map<eular::String8, int> mapObj(source);
    int value = mapObj["hello"];
    int missing = mapObj["missing"];

    CHECK(value == 100);
    CHECK(missing == 0);
}

TEST_CASE("test_clear", "[map]") {
    eular::Map<eular::String8, int> mapObj;
    mapObj.insert("hello", 100);
    mapObj.insert("world", 200);
    CHECK(mapObj.size() == 2);

    mapObj.clear();
    CHECK(mapObj.size() == 0);
}
