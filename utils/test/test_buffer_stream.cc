/*************************************************************************
    > File Name: test_buffer_stream.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年04月27日 星期六 11时39分05秒
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <stdio.h>
#include <iostream>

#include "catch/catch.hpp"
#include "utils/buffer_stream.h"

#define BYTE_64     64
#define BYTE_128    128

#define MALE    1
#define FEMALE  0

typedef struct DeviceInfo {
    char        _name[BYTE_64];
    char        _path[BYTE_128];
    uint32_t    _age;
    bool        _sex;

    DeviceInfo() :
        _age(0),
        _sex(MALE)
    {
        memset(_name, 0, BYTE_64);
        memset(_path, 0, BYTE_128);
    }
} DeviceInfo;

// NOTE 必须要在buffer_stream_utils之前声明, 否则输入数组类型时编译报错
eular::BufferStream &operator<<(eular::BufferStream &stream, const DeviceInfo &info);
eular::BufferStream &operator>>(eular::BufferStream &stream, DeviceInfo &info);

#include "utils/buffer_stream_utils.h"

eular::BufferStream &operator<<(eular::BufferStream &stream, const DeviceInfo &info)
{
    stream << info._name;
    stream << info._path;
    stream << info._age;
    stream << info._sex;
    return stream;
}

eular::BufferStream &operator>>(eular::BufferStream &stream, DeviceInfo &info)
{
    stream >> info._name;
    stream >> info._path;
    stream >> info._age;
    stream >> info._sex;
    return stream;
}

// 测试结构体DeviceInfo输入输出
TEST_CASE("buffer_stream_DeviceInfo_write_read", "[buffer_stream]") {
    DeviceInfo deviceInfo;

    strcpy(deviceInfo._name, "HelloWorld");
    strcpy(deviceInfo._path, "/path/to/Hello");
    deviceInfo._age = 18;
    deviceInfo._sex = MALE;

    eular::ByteBuffer buffer(sizeof(DeviceInfo));
    eular::BufferStream stream(buffer);

    stream << deviceInfo;

    DeviceInfo deviceInfo2;
    stream >> deviceInfo2;

    CHECK(std::string(deviceInfo._name) == deviceInfo2._name);
    CHECK(std::string(deviceInfo._path) == deviceInfo2._path);
    CHECK(deviceInfo._age == deviceInfo2._age);
    CHECK(deviceInfo._sex == deviceInfo2._sex);
}

// 测试DeviceInfo结构体数组输入输出
TEST_CASE("buffer_stream_DeviceInfo_Vector_write_read", "[buffer_stream]") {
    const int32_t VEC_SIZE = 8; 
    DeviceInfo pDeviceInfoVec[VEC_SIZE];

    const char *name = "HelloWorld";
    const char *path = "/path/to/Hello";
    for (int32_t i = 0; i < VEC_SIZE; ++i)
    {
        strcpy(pDeviceInfoVec[i]._name, name);
        strcpy(pDeviceInfoVec[i]._path, path);
        pDeviceInfoVec[i]._age = 18 + i;
        pDeviceInfoVec[i]._sex = (i & 0x01) ? MALE : FEMALE;
    }

    eular::ByteBuffer buffer(sizeof(DeviceInfo) * VEC_SIZE);
    eular::BufferStream stream(buffer);

    stream << pDeviceInfoVec;
    // stream.operator<<<DeviceInfo, 8>(pDeviceInfoVec);

    DeviceInfo pDeviceInfoVec2[VEC_SIZE];
    stream >> pDeviceInfoVec2;

    for (int32_t i = 0; i < VEC_SIZE; ++i)
    {
        CHECK(std::string(pDeviceInfoVec[i]._name) == pDeviceInfoVec2[i]._name);
        CHECK(std::string(pDeviceInfoVec[i]._path) == pDeviceInfoVec2[i]._path);
        CHECK(pDeviceInfoVec[i]._age == pDeviceInfoVec2[i]._age);
        CHECK(pDeviceInfoVec[i]._sex == pDeviceInfoVec2[i]._sex);
    }
}

// 测试结构体DeviceInfo输入输出
TEST_CASE("buffer_stream_char_array_char_pointer_write_read", "[buffer_stream]") {
    {
        char charArray[BYTE_64] = {0};
        memset(charArray, '1', sizeof(charArray));
        charArray[31] = '\0';

        eular::ByteBuffer buffer(BYTE_64);
        eular::BufferStream stream(buffer);

        stream << charArray;

        char charArray2[BYTE_64] = {0};
        stream >> charArray2;

        int32_t cmpRet = memcmp(charArray, charArray2, BYTE_64);
        REQUIRE(cmpRet == 0);
    }

    {
        char charArray[BYTE_64] = {0};
        memset(charArray, '1', sizeof(charArray));
        charArray[31] = '\0';

        eular::ByteBuffer buffer(BYTE_64);
        eular::BufferStream stream(buffer);

        const char *ptr = charArray;
        std::string str = ptr;

        stream << str;

        std::string temp;
        stream >> temp;

        REQUIRE(str == temp);
        REQUIRE(str.size() == temp.size());
    }
}