/*************************************************************************
    > File Name: test_hash.cc
    > Author: hsz
    > Brief:
    > Created Time: Thu 16 Feb 2023 05:08:44 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"
#include "utils/hash.h"

class Key : public eular::HashCmptBase
{
public:
    Key(int v) : h(v) { }
    virtual uint32_t hash() const 
    {
        const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&h);
        return eular::HashCmptBase::compute(ptr, sizeof(h));
    }

    bool operator==(const Key &o) const { return this->h == o.h; }

    int h{0};
};

TEST_CASE("test_construction", "[HashMap]") {
    // 测试默认构造函数
    int num = 100;
    eular::HashMap<Key, int> map;
    map.insert(Key(10), num);

    // 测试拷贝构造函数
    decltype(map) map2(map);
    CHECK(map2.size() == map.size());
    {
        const auto &mm = map2;
        CHECK(mm.at(Key(10)) == num);
    }

    // 测试初始化列表构造
    eular::HashMap<Key, int> map3 = {{10, 10}, {20, 20}};
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
    eular::HashMap<Key, int> map1;
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
