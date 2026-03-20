/*************************************************************************
    > File Name: test_frame_path.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>

#include "proto/frame/path.h"

using eular::utp::FramePathChallenge;
using eular::utp::FramePathResponse;

TEST_CASE("Path frame: challenge encode/decode", "[FramePath]")
{
    FramePathChallenge challenge;
    challenge.data = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};

    std::array<uint8_t, 64> buffer{};
    int32_t encodeLen = challenge.encode(buffer.data(), buffer.size());
    REQUIRE(encodeLen == FRAME_PATH_FRAME_SIZE);

    FramePathChallenge decoded;
    int32_t decodeLen = decoded.decode(buffer.data(), encodeLen);
    REQUIRE(decodeLen == FRAME_PATH_FRAME_SIZE);
    REQUIRE(decoded.data == challenge.data);
}

TEST_CASE("Path frame: response encode/decode", "[FramePath]")
{
    FramePathResponse response;
    response.data = {0x10, 0x20, 0x30, 0x40, 0x41, 0x42, 0x43, 0x44};

    std::array<uint8_t, 64> buffer{};
    int32_t encodeLen = response.encode(buffer.data(), buffer.size());
    REQUIRE(encodeLen == FRAME_PATH_FRAME_SIZE);

    FramePathResponse decoded;
    int32_t decodeLen = decoded.decode(buffer.data(), encodeLen);
    REQUIRE(decodeLen == FRAME_PATH_FRAME_SIZE);
    REQUIRE(decoded.data == response.data);
}

TEST_CASE("Path frame: decode with wrong frame type should fail", "[FramePath]")
{
    FramePathResponse response;
    response.data = {1, 2, 3, 4, 5, 6, 7, 8};

    std::array<uint8_t, 64> buffer{};
    int32_t encodeLen = response.encode(buffer.data(), buffer.size());
    REQUIRE(encodeLen == FRAME_PATH_FRAME_SIZE);

    FramePathChallenge decoded;
    REQUIRE(decoded.decode(buffer.data(), encodeLen) < 0);
}
