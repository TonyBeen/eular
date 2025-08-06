/*************************************************************************
    > File Name: test_has_member.cc
    > Author: hsz
    > Brief:
    > Created Time: Thu 04 Jul 2024 04:08:33 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <stdio.h>
#include <iostream>
#include <string>

#include "catch/catch.hpp"
#include "utils/has_member.hpp"

class Foo
{
public:
    void Hash() {}
    int32_t Hash(int32_t, double) { return 0; }
    void Hash(int32_t) const {}
    // void Hash(std::string) const {}
    void Hash(const std::string &) const {}
};

HAS_MEMBER(FooClass, Hash);

TEST_CASE("test", "[has_member]") {
    CHECK(FooClass::has_member_Hash<Foo, void>::value);
    CHECK(FooClass::has_member_Hash<Foo, int32_t, int32_t, double>::value);
    CHECK(FooClass::has_member_Hash<Foo, void, int32_t>::value);

    CHECK(FooClass::has_member_Hash<Foo, void, std::string>::value);
    CHECK(FooClass::has_member_Hash<Foo, void, std::string&>::value);
    CHECK(FooClass::has_member_Hash<Foo, void, const std::string&>::value);

    CHECK(FooClass::has_member_Hash<Foo, void, double>::value == true); // 隐式类型转换, double -> int32_t
    CHECK(FooClass::has_member_Hash<Foo, double, int32_t>::value == false);
}
