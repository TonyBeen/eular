#include <stdint.h>
#include <string.h>

#include "catch/catch.hpp"
#include "utils/alloc.h"

TEST_CASE("malloc_realloc_and_free_work", "[alloc]") {
    char *data = static_cast<char *>(Malloc(8));
    REQUIRE(data != nullptr);
    memcpy(data, "abc", 4);

    data = static_cast<char *>(Realloc(data, 16));
    REQUIRE(data != nullptr);
    CHECK(strcmp(data, "abc") == 0);

    Free(data);
}

TEST_CASE("aligned_alloc_returns_aligned_pointer", "[alloc]") {
    const size_t alignment = 64;
    void *ptr = AlignedAlloc(128, alignment);
    REQUIRE(ptr != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);

    memset(ptr, 0x5A, 128);
    AlignedFree(ptr);
}

TEST_CASE("aligned_realloc_preserves_existing_bytes", "[alloc]") {
    const size_t alignment = 64;
    unsigned char *ptr = static_cast<unsigned char *>(AlignedAlloc(32, alignment));
    REQUIRE(ptr != nullptr);
    for (size_t i = 0; i < 32; ++i) {
        ptr[i] = static_cast<unsigned char>(i);
    }

    ptr = static_cast<unsigned char *>(AlignedRealloc(ptr, 96, 32, alignment));
    REQUIRE(ptr != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);
    for (size_t i = 0; i < 32; ++i) {
        CHECK(ptr[i] == static_cast<unsigned char>(i));
    }

    AlignedFree(ptr);
}

TEST_CASE("aligned_free_accepts_nullptr", "[alloc]") {
    AlignedFree(nullptr);
    SUCCEED();
}