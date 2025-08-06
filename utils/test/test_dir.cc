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

TEST_CASE("test_exists", "[dir]") {
    CHECK(eular::dir::exists("/home/"));
    CHECK_FALSE(eular::dir::exists("~/")); // not support
    CHECK_FALSE(eular::dir::exists(""));
    CHECK_FALSE(eular::dir::exists("/path/to/non-existent"));
}

TEST_CASE("test_absolute", "[dir]") {
    eular::String8 absPath;

    REQUIRE(eular::dir::absolute("/home/", absPath));
    CHECK(absPath == "/home/");

    absPath.clear();
    REQUIRE(eular::dir::absolute("~/VSCode/", absPath));
    CHECK(absPath == "/home/eular/VSCode/");

    absPath.clear();
    CHECK_FALSE(eular::dir::absolute("", absPath));

    absPath.clear();
    CHECK(eular::dir::absolute("~/App/./go/../../VSCode", absPath));
    CHECK(absPath == "/home/eular/VSCode/");
}