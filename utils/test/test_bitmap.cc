/*************************************************************************
    > File Name: test_bitmap.cc
    > Author: hsz
    > Brief:
    > Created Time: 2022-05-23 11:52:14 Monday
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"
#include "utils/bitmap.h"

TEST_CASE("test_constructer", "[bitmap]") {
    eular::BitMap bitMapObj1;
    REQUIRE(0 == bitMapObj1.count());

    uint32_t size = 17;
    eular::BitMap bitMapObj2(size);
    REQUIRE(size <= bitMapObj2.capacity());
    REQUIRE(size == bitMapObj2.size());
    bitMapObj2.set(1, true);
    REQUIRE(true == bitMapObj2.at(1));

    eular::BitMap fromCopy(bitMapObj2);
    REQUIRE(size <= fromCopy.capacity());
    REQUIRE(true == fromCopy.at(1));
}

TEST_CASE("test_set_at", "[bitmap]") {
    eular::BitMap bitMapObj(16);

    const uint32_t count = 2;

    for (uint32_t i = 0; i < count; ++i) {
        bitMapObj.set(i, true);
    }

    REQUIRE(count == bitMapObj.count());
    REQUIRE(16 <= bitMapObj.capacity());
    REQUIRE_FALSE(bitMapObj.set(16, true));

    for (uint32_t i = 0; i < count; ++i) {
        REQUIRE(bitMapObj.at(i));
    }
}

TEST_CASE("test_resize_size", "[bitmap]") {
    uint32_t size = 17;
    eular::BitMap bitMapObj(size);
    REQUIRE(size == bitMapObj.size());

    for (uint32_t i = 0; i < bitMapObj.size(); ++i) {
        bitMapObj.set(i, true);
    }
    REQUIRE(bitMapObj.count() == size);

    size = 33;
    bitMapObj.resize(size);
    REQUIRE(size == bitMapObj.size());
    REQUIRE(size <= bitMapObj.capacity());
}

TEST_CASE("test_clear", "[bitmap]") {
    eular::BitMap bitMapObj(16);

    const uint32_t count = 2;

    for (uint32_t i = 0; i < count; ++i) {
        bitMapObj.set(i, true);
    }

    bitMapObj.clear();
    REQUIRE(0 == bitMapObj.count());
}
