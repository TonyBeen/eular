/*************************************************************************
    > File Name: test_frame_reset_stream.cc
    > Author: eular
    > Brief:
    > Created Time: Sun 22 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>

#include "proto/frame/reset_stream.h"

using eular::utp::FrameResetStream;

TEST_CASE("FrameResetStream: encode/decode roundtrip", "[Frame][ResetStream]")
{
    FrameResetStream frame;
    frame.error_code = 123;
    frame.stream_id = 42;
    frame.final_size = 9001;

    std::array<uint8_t, FRAME_RESET_STREAM_SIZE> buffer{};
    const int32_t encoded = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encoded == FRAME_RESET_STREAM_SIZE);

    FrameResetStream decoded;
    const int32_t used = decoded.decode(buffer.data(), static_cast<size_t>(encoded));
    REQUIRE(used == FRAME_RESET_STREAM_SIZE);
    REQUIRE(decoded.error_code == frame.error_code);
    REQUIRE(decoded.stream_id == frame.stream_id);
    REQUIRE(decoded.final_size == frame.final_size);
}
