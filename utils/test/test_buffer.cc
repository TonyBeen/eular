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

        ByteBuffer emptyBuffer(static_cast<const uint8_t *>(nullptr), 0);
        CHECK(emptyBuffer.size() == 0);
        CHECK(emptyBuffer.capacity() == 0);
        CHECK(emptyBuffer.const_data() != nullptr);
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

TEST_CASE("operator_index_checks_logical_size", "[ByteBuffer]") {
    ByteBuffer buffer(128);
    buffer.append("Hello");

    CHECK(buffer[0] == 'H');
    CHECK(buffer[4] == 'o');
    CHECK_THROWS(buffer[5]);
    CHECK_THROWS(buffer[100]);

    const ByteBuffer constBuffer(buffer);
    CHECK(constBuffer[1] == 'e');
    CHECK_THROWS(constBuffer[5]);
}

TEST_CASE("data_detaches_shared_buffer_before_write", "[ByteBuffer]") {
    ByteBuffer buffer;
    buffer.append("Hello");
    ByteBuffer shared = buffer;

    REQUIRE(shared.const_data() == buffer.const_data());

    uint8_t *writable = buffer.data();
    writable[0] = 'Y';

    REQUIRE(shared.const_data() != buffer.const_data());
    CHECK(std::string((const char *)buffer.const_data(), buffer.size()) == "Yello");
    CHECK(std::string((const char *)shared.const_data(), shared.size()) == "Hello");
}

TEST_CASE("set_rejects_offset_past_size", "[ByteBuffer]") {
    ByteBuffer buffer;
    buffer.append("Hello");

    CHECK(buffer.set((const uint8_t *)"***", 3, 6) == 0);
    REQUIRE(buffer.size() == 5);
    CHECK(std::string((char *)buffer.data(), buffer.size()) == "Hello");
}

TEST_CASE("append_handles_self_and_shared_source", "[ByteBuffer]") {
    {
        ByteBuffer buffer;
        buffer.append("Hello");
        buffer.append(buffer);

        REQUIRE(buffer.size() == 10);
        CHECK(std::string((char *)buffer.data(), buffer.size()) == "HelloHello");
    }

    {
        ByteBuffer source;
        source.append("Hello");
        ByteBuffer shared = source;
        source.append(shared);

        REQUIRE(source.size() == 10);
        CHECK(std::string((char *)source.data(), source.size()) == "HelloHello");
        REQUIRE(shared.size() == 5);
        CHECK(std::string((char *)shared.data(), shared.size()) == "Hello");
    }
}

TEST_CASE("set_and_insert_handle_internal_source_pointers", "[ByteBuffer]") {
    {
        ByteBuffer buffer;
        buffer.append("Hello***");

        const uint8_t *stars = buffer.const_data() + 5;
        CHECK(buffer.set(stars, 3) == 3);

        REQUIRE(buffer.size() == 3);
        CHECK(std::string((char *)buffer.data(), buffer.size()) == "***");
    }

    {
        ByteBuffer buffer;
        buffer.append("abcdef");

        const uint8_t *slice = buffer.const_data() + 2;
        CHECK(buffer.insert(slice, 3, 1) == 3);

        REQUIRE(buffer.size() == 9);
        CHECK(std::string((char *)buffer.data(), buffer.size()) == "acdebcdef");
    }
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
