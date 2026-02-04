/*************************************************************************
    > File Name: test_serialize.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 04 Feb 2026 11:01:27 AM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <string>
#include <cassert>

#include <catch/catch.hpp>

#include "utils/serialize.hpp"

template<typename T>
static void require_bitwise_equal(T a, T b) {
    static_assert(std::is_arithmetic<T>::value, "Arithmetic required");
    if (std::is_floating_point<T>::value) {
        if (sizeof(T) == 4) {
            uint32_t ba = 0, bb = 0;
            std::memcpy(&ba, &a, sizeof(ba));
            std::memcpy(&bb, &b, sizeof(bb));
            REQUIRE(ba == bb);
        } else if (sizeof(T) == 8) {
            uint64_t ba = 0, bb = 0;
            std::memcpy(&ba, &a, sizeof(ba));
            std::memcpy(&bb, &b, sizeof(bb));
            REQUIRE(ba == bb);
        } else {
            // fallback to value comparison for unusual sizes
            REQUIRE(a == b);
        }
    } else {
        REQUIRE(a == b);
    }
}

template<typename T>
static void roundtrip_value(const T &value) {
    std::vector<uint8_t> buf(256);
    uint8_t* wp = buf.data();
    size_t wrem = buf.size();

    uint8_t* next = eular::Serialize::SerializeTo<uint8_t, T>(wp, wrem, value);
    REQUIRE(next != nullptr);
    size_t used = buf.size() - wrem;

    const uint8_t* rp = buf.data();
    size_t rrem = used;
    T read_value = T();
    const uint8_t* rn = eular::Serialize::DeserializeFrom<uint8_t, T>(rp, rrem, read_value);
    REQUIRE(rn != nullptr);

    require_bitwise_equal<T>(value, read_value);
}

TEST_CASE("Serialize/Deserialize integers and booleans", "[serialize]") {
    SECTION("unsigned 8")  { roundtrip_value<uint8_t>(0xABu); }
    SECTION("signed 8")    { roundtrip_value<int8_t>(int8_t(-42)); }
    SECTION("unsigned 16") { roundtrip_value<uint16_t>(0x3456u); }
    SECTION("signed 16")   { roundtrip_value<int16_t>(int16_t(-12345)); }
    SECTION("unsigned 32") { roundtrip_value<uint32_t>(0x89ABCDEFu); }
    SECTION("signed 32")   { roundtrip_value<int32_t>(int32_t(-2020202020)); }
    SECTION("unsigned 64") { roundtrip_value<uint64_t>(0x1122334455667788ULL); }
    SECTION("signed 64")   { roundtrip_value<int64_t>(int64_t(-123456789012345LL)); }
    SECTION("bool true")   { roundtrip_value<bool>(true); }
    SECTION("bool false")  { roundtrip_value<bool>(false); }
    SECTION("char")        { roundtrip_value<char>('Z'); }
}

TEST_CASE("Serialize/Deserialize floating point", "[serialize][float]") {
    SECTION("float normal") {
        float f = 3.1415926f;
        roundtrip_value<float>(f);
    }
    SECTION("float NaN") {
        float f;
        uint32_t nan_bits = 0x7FC01234u; // quiet NaN payload
        std::memcpy(&f, &nan_bits, sizeof(f));
        roundtrip_value<float>(f);
    }
    SECTION("float +/-0") {
        float p0 = 0.0f;
        float n0;
        uint32_t n0_bits = 0x80000000u;
        std::memcpy(&n0, &n0_bits, sizeof(n0));
        roundtrip_value<float>(p0);
        roundtrip_value<float>(n0);
    }

    SECTION("double normal") {
        double d = 2.718281828459045;
        roundtrip_value<double>(d);
    }
    SECTION("double NaN") {
        double d;
        uint64_t nan_bits = 0x7FF8000000001234ULL;
        std::memcpy(&d, &nan_bits, sizeof(d));
        roundtrip_value<double>(d);
    }
    SECTION("double +/-0") {
        double p0 = 0.0;
        double n0;
        uint64_t n0_bits = 0x8000000000000000ULL;
        std::memcpy(&n0, &n0_bits, sizeof(n0));
        roundtrip_value<double>(p0);
        roundtrip_value<double>(n0);
    }
}

TEST_CASE("Serialize multiple values in sequence", "[serialize][sequence]") {
    std::vector<uint8_t> buf(512);
    uint8_t* wp = buf.data();
    size_t wrem = buf.size();

    uint8_t  u8  = 0x12;
    uint16_t u16 = 0x3456;
    uint32_t u32 = 0x89ABCDEFu;
    float    f   = 1.5f;
    double   d   = -4.25;

    uint8_t* p;
    p = eular::Serialize::SerializeTo<uint8_t, uint8_t>(wp, wrem, u8);  REQUIRE(p); wp = p;
    p = eular::Serialize::SerializeTo<uint8_t, uint16_t>(wp, wrem, u16); REQUIRE(p); wp = p;
    p = eular::Serialize::SerializeTo<uint8_t, uint32_t>(wp, wrem, u32); REQUIRE(p); wp = p;
    p = eular::Serialize::SerializeTo<uint8_t, float>(wp, wrem, f);      REQUIRE(p); wp = p;
    p = eular::Serialize::SerializeTo<uint8_t, double>(wp, wrem, d);     REQUIRE(p); wp = p;

    size_t used = buf.size() - wrem;
    const uint8_t* rp = buf.data();
    size_t rrem = used;

    uint8_t  ru8 = 0;
    uint16_t ru16 = 0;
    uint32_t ru32 = 0;
    float    rf = 0;
    double   rd = 0;

    const uint8_t* q;
    q = eular::Serialize::DeserializeFrom<uint8_t, uint8_t>(rp, rrem, ru8);  REQUIRE(q); rp = q;
    q = eular::Serialize::DeserializeFrom<uint8_t, uint16_t>(rp, rrem, ru16); REQUIRE(q); rp = q;
    q = eular::Serialize::DeserializeFrom<uint8_t, uint32_t>(rp, rrem, ru32); REQUIRE(q); rp = q;
    q = eular::Serialize::DeserializeFrom<uint8_t, float>(rp, rrem, rf);      REQUIRE(q); rp = q;
    q = eular::Serialize::DeserializeFrom<uint8_t, double>(rp, rrem, rd);     REQUIRE(q); rp = q;

    REQUIRE(ru8 == u8);
    REQUIRE(ru16 == u16);
    REQUIRE(ru32 == u32);

    // bitwise compare floats/doubles
    uint32_t obf = 0, rbf = 0;
    std::memcpy(&obf, &f, sizeof(obf));
    std::memcpy(&rbf, &rf, sizeof(rbf));
    REQUIRE(obf == rbf);

    uint64_t obd = 0, rbd = 0;
    std::memcpy(&obd, &d, sizeof(obd));
    std::memcpy(&rbd, &rd, sizeof(rbd));
    REQUIRE(obd == rbd);
}