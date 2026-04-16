/*************************************************************************
    > File Name: test_hash.cc
    > Author: hsz
    > Brief:
    > Created Time: Thu 16 Feb 2023 05:08:44 PM CST
 ************************************************************************/

#include "catch/catch.hpp"
#include "utils/hash.h"

#include <set>
#include <functional>
#include <type_traits>

struct Key
{
    Key(int v = 0) : h(v) { }
    bool operator==(const Key &o) const { return this->h == o.h; }

    int h{0};
};

struct KeyHash {
    size_t operator()(const Key &key) const
    {
        return std::hash<int>()(key.h);
    }
};

struct ModKeyHash {
    size_t operator()(const Key &key) const
    {
        return static_cast<size_t>(key.h % 4);
    }
};

static_assert(!std::is_reference<decltype(std::declval<const eular::HashMap<Key, int, KeyHash> &>().at(Key(1)))>::value,
              "const HashMap::at should return by value to avoid dangling references");
static_assert(!std::is_reference<decltype(std::declval<const eular::HashMap<Key, int, KeyHash> &>()[Key(1)])>::value,
              "const HashMap::operator[] should return by value to avoid dangling references");

TEST_CASE("test_construction", "[HashMap]") {
    // 测试默认构造函数
    int num = 100;
    eular::HashMap<Key, int, KeyHash> map;
    map.insert(Key(10), num);

    // 测试拷贝构造函数
    decltype(map) map2(map);
    CHECK(map2.size() == map.size());
    {
        const auto &mm = map2;
        CHECK(mm.at(Key(10)) == num);
    }

    // 测试初始化列表构造
    eular::HashMap<Key, int, KeyHash> map3 = {{Key(10), 10}, {Key(20), 20}};
    CHECK(map3.size() == 2);

    // 测试移动赋值函数
    map3 = std::move(map); // 此时map3与map2共享
    CHECK(map3.size() == 1);
    CHECK(map.size()== 2);

    // 测试移动构造函数
    decltype(map) map4(std::move(map2));
    CHECK(map4.size() == map3.size());
}

TEST_CASE("test_read_write_foreach_erase", "[HashMap]") {
    int num = 100;
    int begin = 0;
    int recyle = 10;
    eular::HashMap<Key, int, KeyHash> map1;
    for (int i = begin; i < recyle; ++i) {
        map1.insert(Key(i), num + i);
    }
    CHECK(map1.size() == recyle);

    map1.at(Key(begin)) = num * recyle;
    const auto &v = map1.at(Key(begin));
    CHECK(v == num * recyle);
    map1.at(Key(begin)) = num;

    for (int i = 0; i < recyle; ++i) {
        auto it = map1.find(Key(i));
        CHECK(it.value() == (num + i));
    }
}

TEST_CASE("test_iterate_across_buckets", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> map;
    std::set<int> visited;

    for (int i = 0; i < 64; ++i) {
        map.insert(Key(i), i * 10);
    }

    for (auto it = map.begin(); it != map.end(); ++it) {
        visited.insert(it.key().h);
        CHECK(it.value() == it.key().h * 10);
    }

    CHECK(visited.size() == 64);
    for (int i = 0; i < 64; ++i) {
        CHECK(visited.count(i) == 1);
    }
}

TEST_CASE("test_erase_from_shared_iterator", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> map;
    for (int i = 0; i < 8; ++i) {
        map.insert(Key(i), i);
    }

    auto it = map.find(Key(3));
    eular::HashMap<Key, int, KeyHash> shared(map);
    auto next = map.erase(it);

    CHECK(map.size() == 7);
    CHECK(shared.size() == 8);
    CHECK(map.find(Key(3)) == map.end());
    CHECK(shared.find(Key(3)) != shared.end());
    CHECK(next != map.end());
}

TEST_CASE("test_const_at_default_returns_value", "[HashMap]") {
    const eular::HashMap<Key, int, KeyHash> map;
    int value = map.at(Key(99), 1234);
    CHECK(value == 1234);
}

TEST_CASE("test_iterator_increment_and_decrement", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> map;
    for (int i = 0; i < 6; ++i) {
        map.insert(Key(i), i * 10);
    }

    auto it = map.begin();
    auto post = it++;
    CHECK(post.key().h != it.key().h);
    CHECK(map.find(post.key()) == post);

    auto pre = ++it;
    CHECK(pre == it);

    auto tail = map.end();
    --tail;
    CHECK(tail != map.end());
    auto tail_post = tail--;
    CHECK(tail_post != tail);

    std::set<int> visited;
    for (auto walk = map.begin(); walk != map.end(); ++walk) {
        visited.insert(walk.key().h);
    }
    CHECK(visited.size() == map.size());
}

TEST_CASE("test_begin_end_on_shared_copy_do_not_mutate_peer", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> map;
    map.insert(Key(1), 10);
    map.insert(Key(2), 20);

    eular::HashMap<Key, int, KeyHash> shared(map);

    auto begin = map.begin();
    CHECK(begin != map.end());
    int first_key = begin.key().h;
    int original_value = begin.value();
    begin.value() = 999;

    CHECK(map.at(Key(first_key)) == 999);
    CHECK(shared.at(Key(first_key)) == original_value);

    auto end = map.end();
    CHECK(end == map.end());
    auto shared_begin = shared.begin();
    int shared_value = shared_begin.value();
    CHECK((shared_value == 10 || shared_value == 20));
}

TEST_CASE("test_duplicate_insert_keeps_existing_value", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> map;

    auto first = map.insert(Key(7), 70);
    auto second = map.insert(Key(7), 700);

    CHECK(map.size() == 1);
    CHECK(first == second);
    CHECK(map.at(Key(7)) == 70);
}

TEST_CASE("test_erase_end_is_noop", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> map;
    map.insert(Key(1), 10);

    auto next = map.erase(map.end());

    CHECK(map.size() == 1);
    CHECK(next == map.end());
    CHECK(map.at(Key(1)) == 10);
}

TEST_CASE("test_const_operator_brackets_returns_copy", "[HashMap]") {
    eular::HashMap<Key, int, KeyHash> source;
    source.insert(Key(5), 50);

    const eular::HashMap<Key, int, KeyHash> map(source);
    int value = map[Key(5)];
    int missing = map[Key(500)];

    CHECK(value == 50);
    CHECK(missing == 0);
}

TEST_CASE("test_custom_hash_functor_support", "[HashMap]") {
    eular::HashMap<Key, int, ModKeyHash> map;

    map.insert(Key(1), 10);
    map.insert(Key(5), 50);
    map.insert(Key(9), 90);

    CHECK(map.size() == 3);
    CHECK(map.find(Key(1)) != map.end());
    CHECK(map.find(Key(5)) != map.end());
    CHECK(map.find(Key(9)) != map.end());
    CHECK(map.at(Key(5)) == 50);
}
