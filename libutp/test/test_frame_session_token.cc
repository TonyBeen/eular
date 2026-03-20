/*************************************************************************
    > File Name: test_frame_session_token.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>

#include "proto/frame/session_token.h"

using eular::utp::FrameSessionToken;

TEST_CASE("SessionToken frame: encode/decode", "[FrameSessionToken]")
{
    FrameSessionToken frame;
    frame.token_validity_period = 3600;
    frame.token = {0x01, 0x02, 0x03, 0x04, 0x05};
    frame.token_size = static_cast<uint8_t>(frame.token.size());

    std::array<uint8_t, 128> buffer{};
    int32_t encodeLen = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encodeLen == FRAME_SESSION_TOKEN_HDR_SIZE + static_cast<int32_t>(frame.token.size()));

    FrameSessionToken decoded;
    int32_t decodeLen = decoded.decode(buffer.data(), encodeLen);
    REQUIRE(decodeLen == encodeLen);
    REQUIRE(decoded.token_validity_period == frame.token_validity_period);
    REQUIRE(decoded.token_size == frame.token_size);
    REQUIRE(decoded.token == frame.token);
}

TEST_CASE("SessionToken frame: size mismatch should fail", "[FrameSessionToken]")
{
    FrameSessionToken frame;
    frame.token = {0xAA, 0xBB, 0xCC};
    frame.token_size = 4; // intentional mismatch

    std::array<uint8_t, 64> buffer{};
    REQUIRE(frame.encode(buffer.data(), buffer.size()) < 0);
}

TEST_CASE("SessionToken frame: truncated decode should fail", "[FrameSessionToken]")
{
    FrameSessionToken frame;
    frame.token = {0x10, 0x20, 0x30, 0x40};
    frame.token_size = static_cast<uint8_t>(frame.token.size());

    std::array<uint8_t, 64> buffer{};
    int32_t encodeLen = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encodeLen > 0);

    FrameSessionToken decoded;
    REQUIRE(decoded.decode(buffer.data(), encodeLen - 1) < 0);
}
