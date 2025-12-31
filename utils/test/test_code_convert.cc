/*************************************************************************
    > File Name: test_code_convert.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年07月22日 星期一 09时38分44秒
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include "catch/catch.hpp"
#include "utils/code_convert.h"
#include "utils/endian.hpp"
#include "utils/errors.h"

#define HELLO_STRING        u8"你好"
#define GBK_HELLO_CODE      0xC4E3BAC3u // 小端 GBK(按小端排列): 你 0xC4E3 好 0xBAC3
#define UTF16LE_HELLO_CODE  0x604F7D59u // 小端 大端: 你 0x4F60 好 0x597D

TEST_CASE("UTF8 to GBK", "[CodeConvert]") {
    std::string u8str;
    u8str.reserve(8192);
    const int32_t COUNT = 600;
    for (int32_t i = 0; i < COUNT; ++i) {
        u8str.append(HELLO_STRING);
    }

    eular::CodeConvert codeConvert;
    REQUIRE(codeConvert.convertBegin(eular::CodeConvert::UTF8, eular::CodeConvert::GBK));

    for (int32_t i = 0; i < 3; ++i) {
        std::string gbkstr;
        REQUIRE(Status::OK == codeConvert.convert(u8str, gbkstr));
        CHECK(gbkstr.size() == COUNT * sizeof(GBK_HELLO_CODE));

        // NOTE 小端机将4个字节的数据转成整形时会按照小端序读取, 即C4 E3 BA C3转成uin32会变成 C3BAE3C4
        uint32_t helloCode = *reinterpret_cast<const uint32_t *>(gbkstr.c_str());
#if BYTE_ORDER == LITTLE_ENDIAN
        helloCode = byteswap_32(helloCode);
#endif
        CHECK(GBK_HELLO_CODE == helloCode);

        std::string u8strTemp;
        eular::CodeConvert::GBKToUTF8(gbkstr, u8strTemp);
        CHECK(u8strTemp == u8str);
    }

    codeConvert.convertEnd();
}

TEST_CASE("static UTF8 to GBK", "[CodeConvert]") {
    std::string u8str = HELLO_STRING;
    std::string gbkstr;

    REQUIRE(Status::OK == eular::CodeConvert::UTF8ToGBK(u8str, gbkstr));

    uint32_t helloCode = *reinterpret_cast<const uint32_t *>(gbkstr.c_str());
#if BYTE_ORDER == LITTLE_ENDIAN
    helloCode = byteswap_32(helloCode);
#endif

    CHECK(GBK_HELLO_CODE == helloCode);
}

TEST_CASE("UTF8 to UTF16LE", "[CodeConvert]") {
    std::string u8str;
    u8str.reserve(8192);
    const int32_t COUNT = 600;
    for (int32_t i = 0; i < COUNT; ++i) {
        u8str.append(HELLO_STRING);
    }

    eular::CodeConvert codeConvert;
    REQUIRE(codeConvert.convertBegin(eular::CodeConvert::UTF8, eular::CodeConvert::UTF16LE));

    for (int32_t i = 0; i < 3; ++i) {
        std::string utf16str;
        CHECK(Status::OK == codeConvert.convert(u8str, utf16str));
        CHECK(utf16str.size() == COUNT * sizeof(UTF16LE_HELLO_CODE));

        uint32_t helloCode = *reinterpret_cast<const uint32_t *>(utf16str.c_str());
#if BYTE_ORDER == LITTLE_ENDIAN
        helloCode = byteswap_32(helloCode);
#endif

        CHECK(UTF16LE_HELLO_CODE == helloCode);
    }

    codeConvert.convertEnd();
}

TEST_CASE("static UTF8 to UTF16LE", "[CodeConvert]") {
    std::string u8str = HELLO_STRING;
    std::wstring utf16str;

    eular::CodeConvert::UTF8ToUTF16LE(u8str, utf16str);
    REQUIRE(eular::CodeConvert::UTF8ToUTF16LE(u8str, utf16str) == Status::OK);

    uint32_t helloCode = *reinterpret_cast<const uint32_t *>(utf16str.c_str());
#if BYTE_ORDER == LITTLE_ENDIAN
    helloCode = byteswap_32(helloCode);
#endif

    CHECK(UTF16LE_HELLO_CODE == helloCode);
}

TEST_CASE("CheckStringCode", "[CodeConvert]") {
    std::string u8str = HELLO_STRING;
}