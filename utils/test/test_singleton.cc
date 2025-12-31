/*************************************************************************
    > File Name: test_singleton.cc
    > Author: hsz
    > Brief:
    > Created Time: Fri 19 Nov 2021 04:38:34 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#ifndef CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#endif

#include <string>
#include <iostream>
#include <assert.h>

#include "catch/catch.hpp"

#include <utils/singleton.h>
#include <utils/string8.h>

using namespace eular;

class ClassTest {
public:
    ClassTest(bool &isDeconstructionCalled) :
        refIsDeconstructionCalled(isDeconstructionCalled)
    {
        isDeconstructionCalled = false;
    }

    ~ClassTest() { refIsDeconstructionCalled = true; }

    bool &refIsDeconstructionCalled;
};

TEST_CASE("test_singleton_ClassTest", "[singleton]") {
    bool isDeconstructionCalled = false;

    Singleton<ClassTest>::Get(isDeconstructionCalled);
    bool other = false;
    Singleton<ClassTest>::Reset(other);

    REQUIRE(isDeconstructionCalled);
}

TEST_CASE("test_singleton_std_string_Get", "[singleton]") {
    const char *str = "12345";
    auto obj = Singleton<std::string>::Get(str);

    REQUIRE(*obj == std::string(str));
}

TEST_CASE("test_singleton_String8_Get", "[singleton]") {
    const char *str = "67890";

    SObject<eular::String8> obj = Singleton<eular::String8>::Get(str, 4);
    REQUIRE(*obj == eular::String8("6789"));

    // 测试当存在引用时无法Reset
    SObject<eular::String8> obj2 = Singleton<eular::String8>::Reset("-------");
    REQUIRE(*obj == *obj2);
}

TEST_CASE("test_singleton_String8_Reset", "[singleton]") {
    {
        SObject<eular::String8> obj = Singleton<eular::String8>::Get("67890", 4);
        REQUIRE(*obj == eular::String8("6789"));
    }

    // 测试当不存在引用时可以Reset
    const char *str = "-------";
    SObject<eular::String8> obj2 = Singleton<eular::String8>::Reset(str);
    REQUIRE(*obj2 == str);
    
}
