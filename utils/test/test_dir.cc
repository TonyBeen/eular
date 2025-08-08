/*************************************************************************
    > File Name: test_dir.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月22日 星期五 15时19分44秒
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <stdio.h>
#include <iostream>

#include "catch/catch.hpp"
#include "utils/dir.h"
#include "utils/string8.h"

TEST_CASE("test_exists", "[dir]") {
#if defined(OS_WINDOWS)
    CHECK(eular::dir::exists("C:\\Windows"));
    CHECK_FALSE(eular::dir::exists("~/")); // not support
    CHECK_FALSE(eular::dir::exists(""));
    CHECK_FALSE(eular::dir::exists("/path/to/non-existent"));
#else
    CHECK(eular::dir::exists("/home/"));
    CHECK_FALSE(eular::dir::exists("~/")); // not support
    CHECK_FALSE(eular::dir::exists(""));
    CHECK_FALSE(eular::dir::exists("/path/to/non-existent"));
#endif
}

TEST_CASE("test_absolute", "[dir]") {
    std::string absPath;

    REQUIRE(eular::dir::absolute("/home/./", absPath));
    CHECK(absPath == "/home/");

#if defined(OS_WINDOWS)
    absPath.clear();
    REQUIRE(eular::dir::absolute("C:\\Windows\\System32", absPath));
    CHECK(absPath == "C:/Windows/System32/");
#else
    absPath.clear();
    REQUIRE(eular::dir::absolute("~/VSCode/", absPath));
    CHECK(absPath == "/home/eular/VSCode/");
#endif

    absPath.clear();
    CHECK_FALSE(eular::dir::absolute("", absPath));

#if defined(OS_WINDOWS)
    absPath.clear();
    CHECK(eular::dir::absolute("C:\\Windows\\..\\ProgramData\\", absPath));
    CHECK(absPath == "C:/ProgramData/");
#else
    absPath.clear();
    CHECK(eular::dir::absolute("~/App/./go/../../VSCode", absPath));
    CHECK(absPath == "/home/eular/VSCode/");
#endif
}