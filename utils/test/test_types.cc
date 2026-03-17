/*************************************************************************
    > File Name: test_types.cc
    > Author: hsz
    > Brief:
    > Created Time: Wed 07 Sep 2022 09:53:47 AM CST
 ************************************************************************/

#include <utils/types.hpp>
#include <catch/catch.hpp>
#include <iostream>
#include <assert.h>

using namespace std;
using namespace eular;

TEST_CASE("test_types", "[types]")
{
    {
        uint16_le_t temp(0xff);
        uint16_t temp2(0xff);
        CHECK(temp == temp2);
        uint32_le_t temp32_t = 0x12345678;
        temp2 = temp32_t;
        temp = temp32_t; // operator T => uint16_t;
        CHECK(temp == temp2);
        #if BYTE_ORDER == LITTLE_ENDIAN
        CHECK(temp == 0x5678);
        #else
        CHECK(temp == 0x1234);
        #endif
    }
    
    {
        uint16_le_t temp_16 = 0xff;
        uint32_le_t temp = 0xf;
        uint32_t temp2 = 0xf;
        temp = temp_16; // operator T   =>  uint32_t
        temp2 = temp;
        CHECK(temp == temp2);
    }

    {
        uint64_le_t temp = 0xffff;
        uint64_t temp2 = 0xffff;
        CHECK(temp == temp2);
    }
}
