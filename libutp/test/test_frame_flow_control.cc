/*************************************************************************
    > File Name: test_frame_flow_control.cc
    > Author: eular
    > Brief:
    > Created Time: Mon 04 May 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>

#include "proto/frame/max_data.h"
#include "proto/frame/max_stream_data.h"
#include "proto/frame/data_blocked.h"
#include "proto/frame/stream_data_blocked.h"

using eular::utp::FrameDataBlocked;
using eular::utp::FrameMaxData;
using eular::utp::FrameMaxStreamData;
using eular::utp::FrameStreamDataBlocked;

TEST_CASE("MaxData frame: encode/decode", "[FrameFlowControl]")
{
    FrameMaxData frame;
    frame.maximum_data = 123456789ULL;

    std::array<uint8_t, FRAME_MAX_DATA_SIZE> buffer{};
    const int32_t encoded = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encoded == FRAME_MAX_DATA_SIZE);

    FrameMaxData decoded;
    const int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded));
    REQUIRE(decodedLen == encoded);
    REQUIRE(decoded.maximum_data == frame.maximum_data);
}

TEST_CASE("MaxStreamData frame: encode/decode", "[FrameFlowControl]")
{
    FrameMaxStreamData frame;
    frame.stream_id = 42;
    frame.maximum_stream_data = 9988776655ULL;

    std::array<uint8_t, FRAME_MAX_STREAM_DATA_SIZE> buffer{};
    const int32_t encoded = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encoded == FRAME_MAX_STREAM_DATA_SIZE);

    FrameMaxStreamData decoded;
    const int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded));
    REQUIRE(decodedLen == encoded);
    REQUIRE(decoded.stream_id == frame.stream_id);
    REQUIRE(decoded.maximum_stream_data == frame.maximum_stream_data);
}

TEST_CASE("DataBlocked frame: encode/decode", "[FrameFlowControl]")
{
    FrameDataBlocked frame;
    frame.data_limit = 42424242ULL;

    std::array<uint8_t, FRAME_DATA_BLOCKED_SIZE> buffer{};
    const int32_t encoded = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encoded == FRAME_DATA_BLOCKED_SIZE);

    FrameDataBlocked decoded;
    const int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded));
    REQUIRE(decodedLen == encoded);
    REQUIRE(decoded.data_limit == frame.data_limit);
}

TEST_CASE("StreamDataBlocked frame: encode/decode", "[FrameFlowControl]")
{
    FrameStreamDataBlocked frame;
    frame.stream_id = 7;
    frame.stream_data_limit = 1357911ULL;

    std::array<uint8_t, FRAME_STREAM_DATA_BLOCKED_SIZE> buffer{};
    const int32_t encoded = frame.encode(buffer.data(), buffer.size());
    REQUIRE(encoded == FRAME_STREAM_DATA_BLOCKED_SIZE);

    FrameStreamDataBlocked decoded;
    const int32_t decodedLen = decoded.decode(buffer.data(), static_cast<size_t>(encoded));
    REQUIRE(decodedLen == encoded);
    REQUIRE(decoded.stream_id == frame.stream_id);
    REQUIRE(decoded.stream_data_limit == frame.stream_data_limit);
}
