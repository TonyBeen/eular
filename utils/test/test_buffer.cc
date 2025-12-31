#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <unordered_map>

#include "catch/catch.hpp"
#include "utils/buffer.h"
#include "utils/utils.h"

using namespace eular;

TEST_CASE("test_Constructer", "[ByteBuffer]") {
    {
        ByteBuffer defaultBuffer;
        CHECK(defaultBuffer.size() == 0);
        CHECK(defaultBuffer.capacity() == 0);
    }

    {
        ByteBuffer buffer(128);
        CHECK(buffer.capacity() == 128);
    }

    {
        ByteBuffer buffer("Hello");
        CHECK(buffer.size() == 5);

        ByteBuffer buf(nullptr);
        CHECK(buf.size() == 0);
        CHECK(buf.capacity() == 0);
    }

    {
        const uint8_t *data = (const uint8_t *)"Hello";
        ByteBuffer buffer(data, 5);
        CHECK(buffer.size() == 5);
    }

    {
        ByteBuffer buffer(128);
        ByteBuffer buf = buffer;
        CHECK(buf.const_data() == buffer.const_data());
    }

    {
        ByteBuffer buffer;
        ByteBuffer buf;
        buf = buffer;
        CHECK(buf.const_data() == buffer.const_data());
    }

    {
        ByteBuffer buffer(128);
        ByteBuffer buf = std::move(buffer);
        CHECK(buffer.capacity() == 0);
        buffer.append("Hello");
        CHECK(buffer.size() == 5);
    }
}

TEST_CASE("test_apend_insert", "[ByteBuffer]") {
    ByteBuffer buffer(128);
    buffer.append("Hello", 5);
    buffer.insert((const uint8_t *)"***", 3, 1);

    CHECK(buffer.size() == 8);
    CHECK(std::string((char *)buffer.data(), buffer.size()) == "H***ello");
}

TEST_CASE("set_", "[ByteBuffer]") {
    ByteBuffer buffer(128);
    buffer.append("Hello");
    buffer.append("***");

    auto pBegin = buffer.const_data() + 5;
    size_t copySize = 3;

    buffer.set(pBegin, copySize);
    REQUIRE(buffer.size() == 3);

    CHECK(buffer[0] == '*');
    CHECK(buffer[1] == '*');
    CHECK(buffer[2] == '*');
}

TEST_CASE("begin_end", "[ByteBuffer]") {
    ByteBuffer buffer(128);
    buffer.append("Hello");

    uint32_t count = 0;
    for (auto it = buffer.begin(); it != buffer.end(); ++it)
    {
        ++count;
    }
    CHECK(count == buffer.size());
}

TEST_CASE("std_hash_", "[ByteBuffer]") {
    ByteBuffer buffer(128);
    buffer.append("Hello");

    std::unordered_map<ByteBuffer, size_t> hashMap;

    hashMap.insert(std::make_pair(buffer, ByteBuffer::Hash(buffer)));

    buffer.append("***");
    hashMap.insert(std::make_pair(buffer, ByteBuffer::Hash(buffer)));

    CHECK(2 == hashMap.size());
}

TEST_CASE("reserve_resize", "[ByteBuffer]") {
    {
        ByteBuffer buffer;
        buffer.reserve(64);
        CHECK(buffer.capacity() == 64);

        memcpy(buffer.data(), "Hello", 6);

        buffer.resize(5);
        CHECK(std::string("Hello") == (char *)buffer.data());
    }

    {
        ByteBuffer buffer;
        buffer.resize(64);
        CHECK(buffer.size() == 64);

        memset(buffer.data(), 0, buffer.size());
    }
}