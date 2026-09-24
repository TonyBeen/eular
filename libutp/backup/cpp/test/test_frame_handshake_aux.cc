/*************************************************************************
    > File Name: test_frame_handshake_aux.cc
    > Author: eular
    > Brief:
    > Created Time: Thu 02 Apr 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include <array>

#include "proto/frame/handshake_done.h"
#include "proto/frame/handshake_delay.h"

using eular::utp::FrameHandshakeDelay;
using eular::utp::FrameHandshakeDone;
using eular::utp::Status;

TEST_CASE("HandshakeDone frame encode/decode", "[FrameHandshakeDone]")
{
    FrameHandshakeDone frame;
    frame.ack_handshake_pn = 123456789ULL;

    std::array<uint8_t, FRAME_HANDSHAKE_DONE_SIZE> bytes{};
    Status st;
    REQUIRE(frame.encode(bytes.data(), bytes.size(), st) == FRAME_HANDSHAKE_DONE_SIZE);
    REQUIRE(st.ok());

    FrameHandshakeDone decoded;
    REQUIRE(decoded.decode(bytes.data(), bytes.size(), st) == FRAME_HANDSHAKE_DONE_SIZE);
    REQUIRE(st.ok());
    REQUIRE(decoded.ack_handshake_pn == frame.ack_handshake_pn);
}

TEST_CASE("HandshakeDelay frame encode/decode", "[FrameHandshakeDelay]")
{
    FrameHandshakeDelay frame;
    frame.delay_time_us = 987654;

    std::array<uint8_t, FRAME_HANDSHAKE_DELAY_SIZE> bytes{};
    Status st;
    REQUIRE(frame.encode(bytes.data(), bytes.size(), st) == FRAME_HANDSHAKE_DELAY_SIZE);
    REQUIRE(st.ok());

    FrameHandshakeDelay decoded;
    REQUIRE(decoded.decode(bytes.data(), bytes.size(), st) == FRAME_HANDSHAKE_DELAY_SIZE);
    REQUIRE(st.ok());
    REQUIRE(decoded.delay_time_us == frame.delay_time_us);
}
