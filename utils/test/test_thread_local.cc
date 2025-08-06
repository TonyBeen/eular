/*************************************************************************
    > File Name: test_thread_local.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年12月18日 星期三 12时00分54秒
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <unordered_map>
#include <thread>

#include "catch/catch.hpp"
#include "utils/thread_local.h"
#include "utils/utils.h"

struct Foo {
    uint32_t    _id = 0;
    std::string _name;

    Foo(uint32_t id, const std::string& name) : _id(id), _name(name) {}
};

TEST_CASE("test_get_set", "[ThreadLocalStorage]") {
    const char *key = "something";
    std::string name = "alice";
    eular::ThreadLocalStorage::Current()->set<Foo>(key, {16, name});

    std::thread th([key, name] () {
        CHECK(nullptr == eular::ThreadLocalStorage::Current()->get<Foo>(key));
        eular::ThreadLocalStorage::Current()->set<Foo>(key, Foo{18, name});
    });

    th.join();
    auto slot = eular::ThreadLocalStorage::Current()->get<Foo>(key);
    CHECK(slot != nullptr);
    CHECK(slot->value()._id == 16);
    CHECK(slot->value()._name == name);
}