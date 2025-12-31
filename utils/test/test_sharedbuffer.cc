/*************************************************************************
    > File Name: test_sharedbuffer.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 12 Dec 2022 03:18:41 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <catch/catch.hpp>

#include <memory>
#include <stdint.h>

#include <utils/shared_buffer.h>

TEST_CASE("test_alloc_release", "[SharedBuffer]") {
    eular::SharedBuffer *sharedBuffer = eular::SharedBuffer::alloc(1024);
    CHECK(sharedBuffer != nullptr);
    sharedBuffer->release();
}

TEST_CASE("test_dealloc", "[SharedBuffer]") {
    eular::SharedBuffer *sharedBuffer = eular::SharedBuffer::alloc(1024);
    CHECK(sharedBuffer != nullptr);
    eular::SharedBuffer::dealloc(sharedBuffer);
}

TEST_CASE("test_editResize", "[SharedBuffer]") {
    eular::SharedBuffer *sharedBuffer = eular::SharedBuffer::alloc(1024);
    CHECK(sharedBuffer != nullptr);
    sharedBuffer = sharedBuffer->editResize(2048);
    CHECK(sharedBuffer != nullptr);
    sharedBuffer->release();
}
